#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/operator/persistent/physical_update.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/catalog/duck_catalog.hpp"

namespace duckdb {

PhysicalOperator &DuckCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                          PhysicalOperator &plan) {
	auto &update = planner.Make<PhysicalUpdate>(op.types, op.table.Cast<DuckTableEntry>(), op.table.GetStorage(),
	                                            op.columns, std::move(op.expressions), std::move(op.bound_defaults),
	                                            std::move(op.bound_constraints), op.estimated_cardinality,
	                                            op.return_chunk, op.capture_old_rows, std::move(op.old_row_columns));
	auto &cast_update = update.Cast<PhysicalUpdate>();
	cast_update.update_is_del_and_insert = op.update_is_del_and_insert;
	cast_update.children.push_back(plan);
	return update;
}

PhysicalOperator &Catalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op) {
	auto &plan = planner.CreatePlan(*op.children[0]);
	return PlanUpdate(context, planner, op, plan);
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalUpdate &op) {
	D_ASSERT(op.children.size() == 1);
	dependencies.AddDependency(op.table);
	return op.table.catalog.PlanUpdate(context, *this, op);
}

} // namespace duckdb
