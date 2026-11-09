#include "duckdb/execution/operator/helper/physical_external_resource.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_external_resource.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalExternalResource &op) {
	auto &result = Make<PhysicalExternalResource>(std::move(op.data), op.types, op.estimated_cardinality);
	if (!op.children.empty()) {
		// REGISTER EXTERNAL RESOURCES FROM <source>: the source rows are sunk into the operator
		auto &source = CreatePlan(*op.children[0]);
		result.children.push_back(source);
	}
	return result;
}

} // namespace duckdb
