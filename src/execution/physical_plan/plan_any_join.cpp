#include "duckdb/execution/operator/join/physical_blockwise_nl_join.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalAnyJoin &op) {
	// Visit the child nodes.
	D_ASSERT(op.children.size() == 2);
	D_ASSERT(op.condition);
	auto &left = CreatePlan(*op.children[0]);
	auto &right = CreatePlan(*op.children[1]);

	// Create the blockwise NL join.
	return PlanJoinWithProjectionMaps(op, [&]() -> PhysicalOperator & {
		return Make<PhysicalBlockwiseNLJoin>(op, left, right, std::move(op.condition), op.join_type,
		                                     op.estimated_cardinality);
	});
}

} // namespace duckdb
