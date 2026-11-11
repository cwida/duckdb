#include "duckdb/optimizer/outer_join_simplification.hpp"

#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Setup
//===--------------------------------------------------------------------===//

OuterJoinSimplification::OuterJoinSimplification() {
}

OuterJoinSimplification::OuterJoinSimplification(column_binding_set_t required_columns_p)
    : required_columns(std::move(required_columns_p)), initialized_required_columns(true) {
}

//===--------------------------------------------------------------------===//
// Column Binding Helpers
//===--------------------------------------------------------------------===//

void OuterJoinSimplification::AddColumnReferences(const Expression &expr, column_binding_set_t &bindings) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0) {
			bindings.insert(colref.Binding());
		}
	});
}

void OuterJoinSimplification::AddRequiredColumns(const Expression &expr) {
	AddColumnReferences(expr, required_columns);
}

void OuterJoinSimplification::AddRequiredColumns(const JoinCondition &condition) {
	if (condition.IsComparison()) {
		AddRequiredColumns(condition.GetLHS());
		AddRequiredColumns(condition.GetRHS());
	} else {
		AddRequiredColumns(condition.GetJoinExpression());
	}
}

void OuterJoinSimplification::AddRequiredColumns(const vector<JoinCondition> &conditions) {
	for (const auto &condition : conditions) {
		AddRequiredColumns(condition);
	}
}

bool OuterJoinSimplification::GetColumnBinding(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		if (!BoundCastExpression::IsCast(expr)) {
			return false;
		}
		auto &cast = expr.Cast<BoundFunctionExpression>();
		return GetColumnBinding(BoundCastExpression::Child(cast), binding);
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	binding = colref.Binding();
	return true;
}

bool OuterJoinSimplification::GetNullPreservingColumnBinding(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		if (!BoundCastExpression::IsCast(expr)) {
			return false;
		}
		auto &cast = expr.Cast<BoundFunctionExpression>();
		if (BoundCastExpression::IsTryCast(cast) ||
		    !BoundCastExpression::CastIsInvertible(BoundCastExpression::SourceType(cast), cast.GetReturnType())) {
			return false;
		}
		return GetNullPreservingColumnBinding(BoundCastExpression::Child(cast), binding);
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	binding = colref.Binding();
	return true;
}

//===--------------------------------------------------------------------===//
// NULL Constraint Tracking
//===--------------------------------------------------------------------===//

void OuterJoinSimplification::HandleExpression(const Expression &expr) {
	ColumnBinding binding;
	if (GetColumnBinding(expr, binding)) {
		null_filtered_columns.insert(binding);
	}
}

void OuterJoinSimplification::HandleFilterExpression(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
		const auto &is_not_null = expr.Cast<BoundOperatorExpression>();
		HandleExpression(*is_not_null.GetChildren()[0]);
		AddRequiredColumns(expr);
		return;
	}

	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
		const auto &is_null = expr.Cast<BoundOperatorExpression>();
		if (!HandleIsNullExpression(*is_null.GetChildren()[0])) {
			AddRequiredColumns(expr);
		}
		return;
	}

	if (!BoundComparisonExpression::IsComparison(expr)) {
		AddRequiredColumns(expr);
		return;
	}

	AddRequiredColumns(expr);
	if (!FiltersNulls(expr.GetExpressionType())) {
		return;
	}
	const auto &comparison = expr.Cast<BoundFunctionExpression>();
	HandleExpression(BoundComparisonExpression::Left(comparison));
	HandleExpression(BoundComparisonExpression::Right(comparison));
}

bool OuterJoinSimplification::HandleIsNullExpression(const Expression &expr) {
	ColumnBinding binding;
	if (GetNullPreservingColumnBinding(expr, binding)) {
		null_required_columns.insert(binding);
		return true;
	}
	return false;
}

bool OuterJoinSimplification::IsNullFilter(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
	    expr.GetExpressionType() != ExpressionType::OPERATOR_IS_NULL) {
		return false;
	}
	auto &is_null = expr.Cast<BoundOperatorExpression>();
	auto &child = *is_null.GetChildren()[0];
	return GetNullPreservingColumnBinding(child, binding);
}

void OuterJoinSimplification::InitializeRequiredColumns(LogicalOperator &op) {
	if (initialized_required_columns) {
		return;
	}
	for (const auto &binding : op.GetColumnBindings()) {
		required_columns.insert(binding);
	}
	initialized_required_columns = true;
}

bool OuterJoinSimplification::FiltersNulls(ExpressionType comparison_type) {
	return comparison_type != ExpressionType::COMPARE_DISTINCT_FROM &&
	       comparison_type != ExpressionType::COMPARE_NOT_DISTINCT_FROM;
}

//===--------------------------------------------------------------------===//
// Join Rewrite Helpers
//===--------------------------------------------------------------------===//

vector<ColumnBinding> OuterJoinSimplification::GetRightBindings(LogicalJoin &join) {
	return LogicalOperator::MapBindings(join.children[1]->GetColumnBindings(), join.right_projection_map);
}

bool OuterJoinSimplification::HasNullRequiredColumns(const vector<ColumnBinding> &bindings) {
	for (const auto &binding : bindings) {
		if (null_required_columns.find(binding) != null_required_columns.end()) {
			return true;
		}
	}
	return false;
}

bool OuterJoinSimplification::HasRequiredColumns(const vector<ColumnBinding> &bindings) {
	for (const auto &binding : bindings) {
		if (required_columns.find(binding) != required_columns.end()) {
			return true;
		}
	}
	return false;
}

void OuterJoinSimplification::EraseNullRequiredColumns(LogicalOperator &child) {
	for (const auto &binding : child.GetColumnBindings()) {
		null_required_columns.erase(binding);
	}
}

void OuterJoinSimplification::MarkEliminatedNullColumns(const vector<ColumnBinding> &bindings) {
	for (const auto &binding : bindings) {
		eliminated_null_columns.insert(binding);
	}
}

bool OuterJoinSimplification::TryConvertLeftToAntiJoin(LogicalComparisonJoin &join) {
	if (join.join_type != JoinType::LEFT) {
		return false;
	}

	auto right_bindings = GetRightBindings(join);
	if (!HasNullRequiredColumns(right_bindings) || HasRequiredColumns(right_bindings)) {
		return false;
	}

	column_binding_set_t match_non_null_rhs;
	for (const auto &condition : join.conditions) {
		if (!condition.IsComparison() || !FiltersNulls(condition.GetComparisonType())) {
			continue;
		}
		ColumnBinding rhs_binding;
		if (!GetColumnBinding(condition.GetRHS(), rhs_binding)) {
			continue;
		}
		match_non_null_rhs.insert(rhs_binding);
	}

	for (const auto &binding : right_bindings) {
		if (null_required_columns.find(binding) == null_required_columns.end()) {
			continue;
		}
		if (match_non_null_rhs.find(binding) == match_non_null_rhs.end()) {
			continue;
		}
		join.join_type = JoinType::ANTI;
		return true;
	}
	return false;
}

void OuterJoinSimplification::SimplifyOuterJoinType(LogicalComparisonJoin &join) {
	bool preserves_null_extended_rows[2] = {join.join_type == JoinType::LEFT || join.join_type == JoinType::OUTER,
	                                        join.join_type == JoinType::RIGHT || join.join_type == JoinType::OUTER};
	for (idx_t child_idx = 0; child_idx < 2; child_idx++) {
		for (const auto &binding : join.children[child_idx]->GetColumnBindings()) {
			if (null_filtered_columns.find(binding) != null_filtered_columns.end()) {
				// Rejecting NULLs in one child removes preservation of NULL extended rows for the other child.
				preserves_null_extended_rows[1 - child_idx] = false;
				break;
			}
		}
	}

	if (!preserves_null_extended_rows[0] && !preserves_null_extended_rows[1]) {
		join.join_type = JoinType::INNER;
		return;
	}

	if (preserves_null_extended_rows[0] && !preserves_null_extended_rows[1]) {
		D_ASSERT(join.join_type == JoinType::LEFT || join.join_type == JoinType::OUTER);
		join.join_type = JoinType::LEFT;
	} else if (!preserves_null_extended_rows[0] && preserves_null_extended_rows[1]) {
		D_ASSERT(join.join_type == JoinType::RIGHT || join.join_type == JoinType::OUTER);
		join.join_type = JoinType::RIGHT;
	} else {
		D_ASSERT(join.join_type == JoinType::OUTER);
		join.join_type = JoinType::OUTER;
	}
}

//===--------------------------------------------------------------------===//
// Operator Visitors
//===--------------------------------------------------------------------===//

void OuterJoinSimplification::VisitComparisonJoin(LogicalComparisonJoin &join, LogicalOperator &op) {
	switch (join.join_type) {
	case JoinType::INNER:
	case JoinType::SEMI:
		VisitInnerOrSemiJoin(join, op);
		return;
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::OUTER:
		VisitOuterJoin(join, op);
		return;
	default:
		break;
	}
	VisitUnsupportedOperator(op);
}

void OuterJoinSimplification::VisitInnerOrSemiJoin(LogicalComparisonJoin &join, LogicalOperator &op) {
	for (const auto &condition : join.conditions) {
		if (!condition.IsComparison()) {
			continue;
		}
		AddRequiredColumns(condition);
		if (!FiltersNulls(condition.GetComparisonType())) {
			continue;
		}
		HandleExpression(condition.GetLHS());
		HandleExpression(condition.GetRHS());
	}
	VisitOperatorChildren(op);
}

void OuterJoinSimplification::VisitOuterJoin(LogicalComparisonJoin &join, LogicalOperator &op) {
	if (TryConvertLeftToAntiJoin(join)) {
		auto right_bindings = GetRightBindings(join);
		AddRequiredColumns(join.conditions);
		EraseNullRequiredColumns(*join.children[1]);
		VisitOperatorChildren(op);
		// only operators above the join see the right side as NULL, not the right child itself
		MarkEliminatedNullColumns(right_bindings);
		return;
	}

	SimplifyOuterJoinType(join);
	if (join.join_type == JoinType::INNER) {
		VisitOperator(op);
		return;
	}

	AddRequiredColumns(join.conditions);
	// NULL values in the NULL-extended side can be introduced by this join itself,
	// so columns of that side are not guaranteed to be NULL below the join
	if (join.join_type == JoinType::LEFT || join.join_type == JoinType::OUTER) {
		EraseNullRequiredColumns(*join.children[1]);
	}
	if (join.join_type == JoinType::RIGHT || join.join_type == JoinType::OUTER) {
		EraseNullRequiredColumns(*join.children[0]);
	}
	VisitOperatorChildren(op);
}

void OuterJoinSimplification::VisitProjection(LogicalProjection &projection, LogicalOperator &op) {
	vector<pair<idx_t, ColumnBinding>> direct_projection_map;
	for (idx_t col_idx = 0; col_idx < projection.expressions.size(); col_idx++) {
		auto &expr = *projection.expressions[col_idx];
		const ColumnBinding binding(projection.table_index, ProjectionIndex(col_idx));
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			AddRequiredColumns(expr);
			continue;
		}

		auto input_binding = expr.Cast<BoundColumnRefExpression>().Binding();
		direct_projection_map.emplace_back(col_idx, input_binding);
		if (null_filtered_columns.find(binding) != null_filtered_columns.end()) {
			null_filtered_columns.insert(input_binding);
		}
		if (null_required_columns.find(binding) != null_required_columns.end()) {
			null_required_columns.insert(input_binding);
		}
	}
	VisitOperatorChildren(op);

	for (const auto &entry : direct_projection_map) {
		auto col_idx = entry.first;
		auto input_binding = entry.second;
		if (eliminated_null_columns.find(input_binding) == eliminated_null_columns.end()) {
			continue;
		}
		const ColumnBinding output_binding(projection.table_index, ProjectionIndex(col_idx));
		auto type = projection.expressions[col_idx]->GetReturnType();
		projection.expressions[col_idx] = make_uniq<BoundConstantExpression>(Value(type));
		eliminated_null_columns.insert(output_binding);
	}
}

void OuterJoinSimplification::VisitFilter(LogicalFilter &filter, LogicalOperator &op) {
	filter.SplitPredicates(filter.expressions);
	for (const auto &expr : filter.expressions) {
		HandleFilterExpression(*expr);
	}
	VisitOperatorChildren(op);

	for (idx_t i = 0; i < filter.expressions.size(); i++) {
		ColumnBinding binding;
		if (!IsNullFilter(*filter.expressions[i], binding)) {
			continue;
		}
		if (eliminated_null_columns.find(binding) == eliminated_null_columns.end()) {
			continue;
		}
		filter.expressions.erase_at(i);
		i--;
	}
}

void OuterJoinSimplification::VisitOrder(LogicalOrder &order, LogicalOperator &op) {
	for (const auto &order_node : order.orders) {
		AddRequiredColumns(*order_node.expression);
	}
	VisitOperatorChildren(op);
}

void OuterJoinSimplification::VisitTopN(LogicalTopN &top_n, LogicalOperator &op) {
	for (const auto &order_node : top_n.orders) {
		AddRequiredColumns(*order_node.expression);
	}
	OuterJoinSimplification child_simplification(required_columns);
	child_simplification.VisitOperator(*op.children[0]);
}

void OuterJoinSimplification::VisitAggregate(LogicalAggregate &aggregate, LogicalOperator &op) {
	column_binding_set_t child_required_columns;
	for (const auto &expr : aggregate.groups) {
		AddColumnReferences(*expr, child_required_columns);
	}
	for (const auto &expr : aggregate.expressions) {
		AddColumnReferences(*expr, child_required_columns);
	}
	OuterJoinSimplification child_simplification(std::move(child_required_columns));
	child_simplification.VisitOperator(*op.children[0]);
}

void OuterJoinSimplification::VisitUnsupportedOperator(LogicalOperator &op) {
	for (auto &child : op.children) {
		column_binding_set_t child_required_columns;
		for (const auto &binding : child->GetColumnBindings()) {
			child_required_columns.insert(binding);
		}
		OuterJoinSimplification outer_join_simplification(std::move(child_required_columns));
		outer_join_simplification.VisitOperator(*child);
	}
}

void OuterJoinSimplification::VisitOperator(LogicalOperator &op) {
	InitializeRequiredColumns(op);

	switch (op.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		VisitComparisonJoin(join, op);
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op.Cast<LogicalProjection>();
		VisitProjection(projection, op);
		return;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		VisitFilter(filter, op);
		return;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		auto &order = op.Cast<LogicalOrder>();
		VisitOrder(order, op);
		return;
	}
	case LogicalOperatorType::LOGICAL_TOP_N: {
		auto &top_n = op.Cast<LogicalTopN>();
		VisitTopN(top_n, op);
		return;
	}
	case LogicalOperatorType::LOGICAL_LIMIT: {
		OuterJoinSimplification child_simplification(required_columns);
		child_simplification.VisitOperator(*op.children[0]);
		return;
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggregate = op.Cast<LogicalAggregate>();
		VisitAggregate(aggregate, op);
		return;
	}
	default:
		// Passthrough not supported. TODO: could pass through more operators like LOGICAL_UNION
		break;
	}

	VisitUnsupportedOperator(op);
}

} // namespace duckdb
