#include "duckdb/execution/operator/schema/physical_alter_column.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/column_rewrite.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

//! Installs the computed column in the new table version
static void InstallColumn(ClientContext &context, DuckTableEntry &table_entry) {
	auto &storage = table_entry.GetStorage();
	storage.FinishColumnRewrite(context);
	// the catalog entry was created before the transaction-local rows moved over to its storage
	LocalStorage::Get(context, storage.db).SetTableEntry(table_entry);
}

optional_ptr<DuckTableEntry> PhysicalAlterColumn::GetPendingTable(ClientContext &context, const QualifiedName &table) {
	// look the table up as Catalog::Alter did
	auto &catalog = Catalog::GetCatalog(context, table.Catalog());
	CatalogEntryRetriever retriever(context);
	auto lookup =
	    catalog.LookupEntry(retriever, EntryLookupInfo(CatalogType::TABLE_ENTRY, table), OnEntryNotFound::RETURN_NULL);
	if (!lookup.Found() || lookup.entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto table_entry = lookup.entry->Cast<TableCatalogEntry>().TryGetDuckTableEntry();
	if (!table_entry || !table_entry->GetStorage().GetColumnRewrite()) {
		return nullptr;
	}
	return table_entry;
}

void PhysicalAlterColumn::AbortPendingColumn(ClientContext &context, DuckTableEntry &table_entry) {
	if (!table_entry.GetStorage().GetColumnRewrite()) {
		return;
	}
	ValidChecker::Invalidate(MetaTransaction::Get(context), "an ALTER TABLE failed to compute its column");
	table_entry.GetStorage().AbortColumnRewrite();
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class AlterColumnSinkState : public GlobalSinkState {
public:
	//! The new table version, if the ALTER left a column to compute
	optional_ptr<DuckTableEntry> table_entry;
};

unique_ptr<GlobalSinkState> PhysicalAlterColumn::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<AlterColumnSinkState>();
}

SinkResultType PhysicalAlterColumn::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType PhysicalAlterColumn::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<AlterColumnSinkState>();
	auto table_entry = GetPendingTable(context, table);
	if (!table_entry) {
		return SinkFinalizeType::READY;
	}
	if (table_entry->GetStorage().GetColumnRewrite()->RowGroupCount() == 0) {
		InstallColumn(context, *table_entry);
		return SinkFinalizeType::READY;
	}
	gstate.table_entry = table_entry;
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class AlterColumnSourceState : public GlobalSourceState {
public:
	explicit AlterColumnSourceState(optional_ptr<DuckTableEntry> table_entry_p)
	    : table_entry(table_entry_p), next_row_group(0), rewritten_row_groups(0) {
		if (table_entry) {
			rewrite = table_entry->GetStorage().GetColumnRewrite();
			row_group_count = rewrite->RowGroupCount();
		}
	}

	idx_t MaxThreads() override {
		// a volatile expression is computed in row order, by a single thread
		return rewrite && !rewrite->IsVolatile() ? row_group_count : 1;
	}

	optional_ptr<DuckTableEntry> table_entry;
	optional_ptr<ColumnRewrite> rewrite;
	idx_t row_group_count = 0;
	atomic<idx_t> next_row_group;
	atomic<idx_t> rewritten_row_groups;
};

unique_ptr<GlobalSourceState> PhysicalAlterColumn::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<AlterColumnSourceState>(sink_state->Cast<AlterColumnSinkState>().table_entry);
}

SourceResultType PhysicalAlterColumn::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                      OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<AlterColumnSourceState>();
	auto index = state.next_row_group++;
	if (index >= state.row_group_count) {
		return SourceResultType::FINISHED;
	}
	state.rewrite->RewriteRowGroup(index);
	if (++state.rewritten_row_groups < state.row_group_count) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	// the thread that rewrote the last row group installs the column
	InstallColumn(context.client, *state.table_entry);
	return SourceResultType::FINISHED;
}

ProgressData PhysicalAlterColumn::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<AlterColumnSourceState>();
	ProgressData progress;
	if (state.row_group_count == 0) {
		progress.SetInvalid();
		return progress;
	}
	progress.done = static_cast<double>(state.rewritten_row_groups.load());
	progress.total = static_cast<double>(state.row_group_count);
	return progress;
}

} // namespace duckdb
