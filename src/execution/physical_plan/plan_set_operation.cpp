#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/set/physical_union.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb {

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalSetOperation &op) {
	D_ASSERT(op.children.size() == 2);

	auto left = CreatePlan(*op.children[0]);
	auto right = CreatePlan(*op.children[1]);

	if (left->GetTypes() != right->GetTypes()) {
		throw Exception("Type mismatch for SET OPERATION");
	}

	switch (op.type) {
	case LogicalOperatorType::LOGICAL_UNION:
		// UNION
		return make_unique<PhysicalUnion>(op.types, move(left), move(right), op.estimated_cardinality);
	default: {
		// EXCEPT/INTERSECT
		D_ASSERT(op.type == LogicalOperatorType::LOGICAL_EXCEPT || op.type == LogicalOperatorType::LOGICAL_INTERSECT);
		auto &types = left->GetTypes();
		vector<JoinCondition> conditions;
		// create equality condition for all columns
		for (idx_t i = 0; i < types.size(); i++) {
			JoinCondition cond;
			cond.comparison = ExpressionType::COMPARE_EQUAL;
			cond.left = make_unique<BoundReferenceExpression>(types[i], i);
			cond.right = make_unique<BoundReferenceExpression>(types[i], i);
			cond.null_values_are_equal = true;
			conditions.push_back(move(cond));
		}
		// EXCEPT is ANTI join
		// INTERSECT is SEMI join
		PerfectHashJoinStats join_stats; // used in inner joins only
		JoinType join_type = op.type == LogicalOperatorType::LOGICAL_EXCEPT ? JoinType::ANTI : JoinType::SEMI;
		return make_unique<PhysicalHashJoin>(op, move(left), move(right), move(conditions), join_type,
		                                     op.estimated_cardinality, join_stats);
	}
	}
}

} // namespace duckdb
