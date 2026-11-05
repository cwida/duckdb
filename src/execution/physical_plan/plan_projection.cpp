#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalProjection &op) {
	D_ASSERT(op.children.size() == 1);
	auto &plan = CreatePlan(*op.children[0]);

#ifdef DEBUG
	for (auto &expr : op.expressions) {
		D_ASSERT(!expr->IsWindow());
		D_ASSERT(!expr->IsAggregate());
	}
#endif
	if (plan.GetTypes().size() == op.types.size()) {
		// check if this projection can be omitted entirely
		// this happens if a projection simply emits the columns in the same order
		// e.g. PROJECTION(#0, #1, #2, #3, ...)
		bool omit_projection = true;
		for (idx_t i = 0; i < op.types.size(); i++) {
			if (op.expressions[i]->GetExpressionType() == ExpressionType::BOUND_REF) {
				auto &bound_ref = op.expressions[i]->Cast<BoundReferenceExpression>();
				if (bound_ref.Index() == i && op.types[i].EqualsIncludingCollation(plan.GetTypes()[i])) {
					continue;
				}
			}
			omit_projection = false;
			break;
		}
		if (omit_projection) {
			// the projection only directly projects the child' columns: omit it entirely
			return plan;
		}
	}

	auto &proj = Make<PhysicalProjection>(op.types, std::move(op.expressions), op.estimated_cardinality);
	proj.children.push_back(plan);
	return proj;
}

} // namespace duckdb
