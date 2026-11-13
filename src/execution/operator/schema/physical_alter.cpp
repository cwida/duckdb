#include "duckdb/execution/operator/schema/physical_alter.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/alter_database_info.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/operator/schema/physical_alter_column.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class AlterSourceState : public GlobalSourceState {
public:
	explicit AlterSourceState(ClientContext &context) : context(context) {
	}
	~AlterSourceState() override {
		// the executor destroys this state after its tasks are done and before the query ends the transaction
		if (!pending_table) {
			return;
		}
		try {
			PhysicalAlterColumn::AbortPendingColumn(context, *pending_table);
		} catch (...) { // NOLINT: the transaction rollback still gives back the blocks
		}
	}

	ClientContext &context;
	//! The new table version whose column the PhysicalAlterColumn above computes
	optional_ptr<DuckTableEntry> pending_table;
};

unique_ptr<GlobalSourceState> PhysicalAlter::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<AlterSourceState>(context);
}

SourceResultType PhysicalAlter::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	if (info->type == AlterType::ALTER_DATABASE) {
		auto &db_info = info->Cast<AlterDatabaseInfo>();
		auto &db_manager = DatabaseManager::Get(context.client);
		db_manager.Alter(context.client, db_info);
	} else {
		auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
		info->defer_column_rewrite = defer_column_rewrite;
		catalog.Alter(context.client, *info);
		if (defer_column_rewrite) {
			// a column that is not installed by the end of the query is given up on
			auto &state = input.global_state.Cast<AlterSourceState>();
			state.pending_table = PhysicalAlterColumn::GetPendingTable(context.client, info->GetQualifiedName());
		}
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
