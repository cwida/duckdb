#include "duckdb/execution/operator/helper/physical_external_resource.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_external_resource.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalExternalResource &op) {
	return Make<PhysicalExternalResource>(std::move(op.data), op.types, op.estimated_cardinality);
}

} // namespace duckdb
