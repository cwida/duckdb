#include "duckdb/execution/operator/helper/physical_secure_view.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalSecureView &op) {
	D_ASSERT(op.children.size() == 1);
	auto &plan = CreatePlan(*op.children[0]);
	return Make<PhysicalSecureView>(plan, op.view_name, op.pushed_filters, op.estimated_cardinality);
}

} // namespace duckdb
