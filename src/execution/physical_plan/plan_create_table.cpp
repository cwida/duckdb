#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/execution/operator/persistent/physical_batch_insert.hpp"
#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/execution/operator/schema/physical_create_table.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

PhysicalOperator &DuckCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalCreateTable &op, PhysicalOperator &plan) {
	bool parallel_streaming_insert = !PhysicalPlanGenerator::PreserveInsertionOrder(context, plan);
	bool use_batch_index = PhysicalPlanGenerator::UseBatchIndex(context, plan);
	auto num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	if (!parallel_streaming_insert && use_batch_index) {
		auto &insert = planner.Make<PhysicalBatchInsert>(op, op.schema, std::move(op.info), 0U);
		D_ASSERT(op.children.size() == 1);
		insert.children.push_back(plan);
		return insert;
	}

	auto parallel = parallel_streaming_insert && num_threads > 1;
	auto &insert = planner.Make<PhysicalInsert>(op, op.schema, std::move(op.info), 0U, parallel);
	D_ASSERT(op.children.size() == 1);
	insert.children.push_back(plan);
	return insert;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalCreateTable &op) {
	const auto &create_info = op.info->base->Cast<CreateTableInfo>();
	auto &catalog = op.info->schema.catalog;
	// probe the bound schema entry directly: re-resolving by name can hit a sibling schema with the same leaf name
	auto existing_entry = op.info->schema.GetEntry(catalog.GetCatalogTransaction(context), CatalogType::TABLE_ENTRY,
	                                               create_info.GetTableName());
	bool replace = op.info->Base().on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT;
	if ((!existing_entry || replace) && !op.children.empty()) {
		auto &plan = CreatePlan(*op.children[0]);
		return op.schema.catalog.PlanCreateTableAs(context, *this, op, plan);
	}
	return Make<PhysicalCreateTable>(op, op.schema, std::move(op.info), op.estimated_cardinality);
}

} // namespace duckdb
