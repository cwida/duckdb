#include "duckdb/execution/operator/schema/physical_create_index.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/planner/logical_operator.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/planner/constraints/bound_not_null_constraint.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/execution/index/index_type.hpp"

namespace duckdb {

PhysicalCreateIndex::PhysicalCreateIndex(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table_p,
                                         const vector<column_t> &column_ids, unique_ptr<CreateIndexInfo> info,
                                         vector<unique_ptr<Expression>> unbound_expressions_p,
                                         idx_t estimated_cardinality, IndexType index_type,
                                         unique_ptr<IndexBuildBindData> bind_data,
                                         unique_ptr<AlterTableInfo> alter_table_info)

    : PhysicalOperator(physical_plan, PhysicalOperatorType::CREATE_INDEX, op.types, estimated_cardinality),
      table(table_p.Cast<DuckTableEntry>()), info(std::move(info)),
      unbound_expressions(std::move(unbound_expressions_p)), index_type(std::move(index_type)),
      bind_data(std::move(bind_data)), alter_table_info(std::move(alter_table_info)) {
	// Convert the logical column ids to physical column ids.
	for (auto &column_id : column_ids) {
		storage_ids.push_back(table.GetColumns().LogicalToPhysical(LogicalIndex(column_id)).index);
	}

	for (idx_t i = 0; i < unbound_expressions.size(); ++i) {
		auto &expr = unbound_expressions[i];
		indexed_column_types.push_back(expr->GetReturnType());
		indexed_columns.push_back(i);
	}

	// Row id is always last
	rowid_column.push_back(unbound_expressions.size());
}

//---------------------------------------------------------------------------------------------------------------------
// Sink
//---------------------------------------------------------------------------------------------------------------------
class CreateIndexGlobalSinkState : public GlobalSinkState {
public:
	unique_ptr<IndexBuildGlobalState> gstate;
};

unique_ptr<GlobalSinkState> PhysicalCreateIndex::GetGlobalSinkState(ClientContext &context) const {
	auto gstate = make_uniq<CreateIndexGlobalSinkState>();

	IndexBuildInitGlobalStateInput global_state_input {bind_data.get(),     context,    table, *info,
	                                                   unbound_expressions, storage_ids};
	gstate->gstate = index_type.build_global_init(global_state_input);

	return std::move(gstate);
}

class CreateIndexLocalSinkState : public LocalSinkState {
public:
	unique_ptr<IndexBuildLocalState> lstate;
	DataChunk key_chunk;
	DataChunk row_chunk;
};

unique_ptr<LocalSinkState> PhysicalCreateIndex::GetLocalSinkState(ExecutionContext &context) const {
	auto lstate = make_uniq<CreateIndexLocalSinkState>();

	IndexBuildInitLocalStateInput local_state_input {bind_data.get(), context.client,      table,
	                                                 *info,           unbound_expressions, storage_ids};
	lstate->lstate = index_type.build_local_init(local_state_input);

	lstate->key_chunk.InitializeEmpty(indexed_column_types);
	lstate->row_chunk.InitializeEmpty({LogicalType::ROW_TYPE});

	return std::move(lstate);
}

SinkResultType PhysicalCreateIndex::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CreateIndexGlobalSinkState>();
	auto &lstate = input.local_state.Cast<CreateIndexLocalSinkState>();

	// FIXME: use unified format instead of Flatten
	chunk.Flatten();

	// Reference the key columns and rowid column
	lstate.key_chunk.ReferenceColumns(chunk, indexed_columns);
	lstate.row_chunk.ReferenceColumns(chunk, rowid_column);

	// PRIMARY KEY columns cannot be NULL. UNIQUE allows NULLs.
	if (alter_table_info && info->constraint_type == IndexConstraintType::PRIMARY) {
		for (idx_t i = 0; i < lstate.key_chunk.ColumnCount(); i++) {
			if (VectorOperations::HasNull(lstate.key_chunk.data[i])) {
				throw ConstraintException("NOT NULL constraint failed: %s", info->GetIndexName());
			}
		}
	}

	// Sink into the index
	IndexBuildSinkInput sink_input {bind_data.get(), *gstate.gstate, *lstate.lstate, table, *info};
	index_type.build_sink(sink_input, lstate.key_chunk, lstate.row_chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalCreateIndex::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<CreateIndexGlobalSinkState>();
	auto &lstate = input.local_state.Cast<CreateIndexLocalSinkState>();

	IndexBuildCombineInput combine_input {bind_data.get(), *gstate.gstate, *lstate.lstate, table, *info};
	index_type.build_combine(combine_input);

	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalCreateIndex::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<CreateIndexGlobalSinkState>();

	// Finalize the index
	IndexBuildFinalizeInput finalize_input {*gstate.gstate};
	auto bound_index = index_type.build_finalize(finalize_input);

	// Vacuum excess memory and verify.
	bound_index->Vacuum();

	bound_index->Verify();

	D_ASSERT(!bound_index->ToString(true).empty());

	bound_index->VerifyAllocations();

	auto &storage = table.GetStorage();
	if (!storage.IsMainTable()) {
		throw TransactionException(
		    "Transaction conflict: cannot add an index to a table that has been altered or dropped");
	}

	auto &schema = table.schema;
	info->column_ids = storage_ids;

	// Read local storage before ALTER moves it; CREATE UNIQUE INDEX also needs it for the delete index.
	auto &local_storage = LocalStorage::Get(context, storage.db);
	auto local_table_storage = local_storage.GetStorage(storage);

	auto check_mode = ConstraintCheckMode::DEFAULT;
	idx_t index_oid = DConstants::INVALID_INDEX;
	if (!alter_table_info) {
		// Ensure that the index does not yet exist in the catalog.
		auto entry =
		    schema.GetEntry(schema.GetCatalogTransaction(context), CatalogType::INDEX_ENTRY, info->GetIndexName());
		if (entry) {
			if (info->on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
				throw CatalogException("Index with name %s already exists!", info->GetIndexName());
			}
			// IF NOT EXISTS on existing index. We are done.
			return SinkFinalizeType::READY;
		}

		auto index_entry = schema.CreateIndex(schema.GetCatalogTransaction(context), *info, table).get();
		D_ASSERT(index_entry);
		auto &index = index_entry->Cast<DuckIndexEntry>();
		index.initial_index_size = bound_index->GetInMemorySize();
		index_oid = index.oid;
	} else {
		// PRIMARY KEY columns cannot be NULL.
		if (info->constraint_type == IndexConstraintType::PRIMARY) {
			for (const auto &column_id : storage_ids) {
				BoundNotNullConstraint not_null {PhysicalIndex(column_id)};
				local_storage.VerifyNewConstraint(storage, not_null);
			}
		}
		auto &constraint_info = alter_table_info->Cast<AddConstraintInfo>();
		check_mode = constraint_info.constraint->Cast<UniqueConstraint>().check_mode;
		index_oid = DatabaseManager::Get(context).NextOid();
		constraint_info.constraint->SetBackingIndexOid(index_oid);

		auto &catalog = Catalog::GetCatalog(context, info->GetQualifiedName().Catalog());
		catalog.Alter(context, *alter_table_info);
	}

	// Add the index to the storage.
	auto index_entry = storage.GetDataTableInfo()->GetIndexes().AddIndex(std::move(bound_index), index_oid, check_mode);
	if (local_table_storage) {
		// Existing transaction-local rows are verified when committing, so we only add a delete index.
		index_entry->InitializeLocalDeleteIndex(local_table_storage->delete_indexes);
	}

	return SinkFinalizeType::READY;
}

} // namespace duckdb
