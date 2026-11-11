#include "duckdb/execution/physical_plan_generator.hpp"

#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_window.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/set/physical_union.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/function/window_function.hpp"

namespace duckdb {

static vector<unique_ptr<Expression>> CreatePartitionedRowNumExpression(ClientContext &client,
                                                                        const vector<LogicalType> &types) {
	vector<unique_ptr<Expression>> res;
	auto expr = GetBuiltinWindowFunction(client, RowNumberFun::Name, {})->Bind(client);
	expr->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	expr->WindowEndMutable() = WindowBoundary::UNBOUNDED_FOLLOWING;
	for (idx_t i = 0; i < types.size(); i++) {
		expr->PartitionsMutable().push_back(make_uniq<BoundReferenceExpression>(types[i], i));
		ExpressionBinder::PushCollation(client, expr->PartitionsMutable().back(), types[i]);
	}
	res.push_back(std::move(expr));
	return res;
}

static JoinCondition CreateNotDistinctComparison(ClientContext &context, const LogicalType &type, idx_t i) {
	auto left = make_uniq<BoundReferenceExpression>(type, i);
	auto right = make_uniq<BoundReferenceExpression>(type, i);
	JoinCondition cond(std::move(left), std::move(right), ExpressionType::COMPARE_NOT_DISTINCT_FROM);
	ExpressionBinder::PushCollation(context, cond.LeftReference(), type);
	ExpressionBinder::PushCollation(context, cond.RightReference(), type);

	return cond;
}

//! The types to compare the columns of EXCEPT / INTERSECT with - a collation of either side applies (as for UNION)
static vector<LogicalType> GetComparisonTypes(ClientContext &context, const LogicalSetOperation &op,
                                              const vector<LogicalType> &left_types,
                                              const vector<LogicalType> &right_types) {
	// the types only differ in their collations here, but a collation can be missing from a child (e.g. when the
	// projection applying a COLLATE was removed) or from the operator (it takes the types of the left child)
	vector<LogicalType> types;
	for (idx_t i = 0; i < left_types.size(); i++) {
		auto type = LogicalType::ForceMaxLogicalType(context, left_types[i], right_types[i]);
		// the operator types can be outdated (e.g. compressed children), only take a collation from them
		if (i < op.types.size() && op.types[i].id() == type.id()) {
			type = LogicalType::ForceMaxLogicalType(context, op.types[i], type);
		}
		types.push_back(std::move(type));
	}
	return types;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalSetOperation &op) {
	ArenaLinkedList<reference<PhysicalOperator>> children(physical_plan->ArenaRef());
	for (auto &child : op.children) {
		children.push_back(CreatePlan(*child));
	}
	for (idx_t i = 1; i < children.size(); i++) {
		if (children[i].get().GetTypes() != children[0].get().GetTypes()) {
			throw InvalidInputException("Type mismatch for SET OPERATION");
		}
	}

	optional_ptr<PhysicalOperator> result;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_UNION:
		// UNION
		result = Make<PhysicalUnion>(op.types, std::move(children), op.estimated_cardinality, op.allow_out_of_order);
		break;
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		if (children.size() != 2) {
			throw InternalException("EXCEPT / INTERSECT must have exactly two children");
		}
		auto &left = children[0];
		auto &right = children[1];
		auto types = GetComparisonTypes(context, op, left.get().GetTypes(), right.get().GetTypes());
		vector<JoinCondition> conditions;
		// create equality condition for all columns
		for (idx_t i = 0; i < types.size(); i++) {
			conditions.push_back(CreateNotDistinctComparison(context, types[i], i));
		}
		// For EXCEPT ALL / INTERSECT ALL we push a window operator with a ROW_NUMBER into the scans and join to get bag
		// semantics.
		if (op.setop_all) {
			vector<LogicalType> window_types = types;
			window_types.push_back(LogicalType::BIGINT);

			auto select_list = CreatePartitionedRowNumExpression(context, types);
			auto &left_window =
			    Make<PhysicalWindow>(window_types, std::move(select_list), left.get().estimated_cardinality);
			left_window.children.push_back(left);
			left = left_window;

			select_list = CreatePartitionedRowNumExpression(context, types);
			auto &right_window =
			    Make<PhysicalWindow>(window_types, std::move(select_list), right.get().estimated_cardinality);
			right_window.children.push_back(right);
			right = right_window;

			// add window expression result to join condition
			conditions.push_back(CreateNotDistinctComparison(context, LogicalType::BIGINT, types.size()));
			// join (created below) now includes the row number result column
			op.types.push_back(LogicalType::BIGINT);
		}

		// EXCEPT is ANTI join
		// INTERSECT is SEMI join

		JoinType join_type = op.type == LogicalOperatorType::LOGICAL_EXCEPT ? JoinType::ANTI : JoinType::SEMI;
		result = Make<PhysicalHashJoin>(op, left, right, std::move(conditions), join_type, op.estimated_cardinality);

		// For EXCEPT ALL / INTERSECT ALL we need to remove the row number column again
		if (op.setop_all) {
			// Restore the logical operator's types: the ROW_NUMBER column is an implementation detail of the physical
			// plan.
			op.types.pop_back();

			vector<unique_ptr<Expression>> select_list;
			for (idx_t i = 0; i < types.size(); i++) {
				select_list.push_back(make_uniq<BoundReferenceExpression>(types[i], i));
			}
			auto &proj = Make<PhysicalProjection>(types, std::move(select_list), op.estimated_cardinality);
			proj.children.push_back(*result);
			result = proj;
		}
		break;
	}
	default:
		throw InternalException("Unexpected operator type for set operation");
	}

	// if the ALL specifier is not given, we have to ensure distinct results. Hence, push a GROUP BY ALL
	if (!op.setop_all) { // no ALL, use distinct semantics
		auto &types = result->GetTypes();
		vector<unique_ptr<Expression>> groups, aggregates /* left empty */;
		for (idx_t i = 0; i < types.size(); i++) {
			groups.push_back(make_uniq<BoundReferenceExpression>(types[i], i));
		}
		auto &group_by = Make<PhysicalHashAggregate>(context, op.types, std::move(aggregates), std::move(groups),
		                                             result->estimated_cardinality);
		group_by.children.push_back(*result);
		result = group_by;
	}

	D_ASSERT(result);
	return *result;
}

} // namespace duckdb
