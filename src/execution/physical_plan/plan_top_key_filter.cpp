#include "duckdb/execution/operator/filter/physical_top_key_filter.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_top_key_filter.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalTopKeyFilter &op) {
	D_ASSERT(op.children.size() == 1);
	auto &plan = CreatePlan(*op.children[0]);
	auto &filter = Make<PhysicalTopKeyFilter>(op.types, std::move(op.orders), op.limit, std::move(op.dynamic_filter),
	                                          op.estimated_cardinality);
	filter.children.push_back(plan);
	return filter;
}

} // namespace duckdb
