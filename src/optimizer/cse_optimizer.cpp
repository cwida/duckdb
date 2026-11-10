#include "duckdb/optimizer/cse_optimizer.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

//! The CSENode contains information about a common subexpression; how many times it occurs, and the column index in the
//! underlying projection
struct CSENode {
	idx_t count;
	//! Whether an occurrence sits where the operator evaluates it for every row; only such an occurrence may hoist it
	bool has_eager_occurrence;
	ProjectionIndex column_index;

	CSENode() : count(0), has_eager_occurrence(false), column_index() {
	}

	bool ShouldExtract() const {
		return count > 1 && has_eager_occurrence;
	}
};

//! The CSEReplacementState
struct CSEReplacementState {
	//! The projection index of the new projection
	TableIndex projection_index;
	//! Map of expression -> CSENode
	expression_map_t<CSENode> expression_count;
	//! Map of column bindings to column indexes in the projection expression list
	column_binding_map_t<ProjectionIndex> column_map;
	//! The set of expressions of the resulting projection
	vector<unique_ptr<Expression>> expressions;
	//! Cached expressions that are kept around so the expression_map always contains valid expressions
	vector<unique_ptr<Expression>> cached_expressions;
	//! True while visiting a child the executor may skip: after the first CASE condition, or a later operand
	bool inside_lazy_child = false;
	//! True while visiting the child of a TRY or HAS_ERROR, whose errors must stay inside the capture
	bool inside_try = false;
	//! Whether the root of each aggregate filter stays intact (ungrouped aggregates evaluate their filters directly)
	bool keep_aggregate_filters = false;
	//! True when visiting the root of a kept filter; its sub-expressions are still extracted
	bool at_kept_filter_root = false;
	//! True when visiting an expression the executor evaluates through Select
	bool at_select_position = false;
	//! The kept filters counted so far; identical filters are evaluated once, so they are counted once
	expression_set_t counted_filters;
};

//! Whether child is the filter of an aggregate whose root stays with the aggregate
static bool IsKeptFilterRoot(const CSEReplacementState &state, const Expression &parent, const Expression &child) {
	if (!state.keep_aggregate_filters || parent.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	if (parent.Cast<BoundAggregateExpression>().GetFilter().get() != &child) {
		return false;
	}
	// the planner moves volatile filters into the projection, one per aggregate: their sub-expressions are shared
	return !child.IsVolatile();
}

//! Whether the executor evaluates child through Select: an operand of a conjunction at a Select position, or a filter
//! kept with its aggregate (CASE conditions are marked by the CASE walk)
static bool IsSelectPosition(const CSEReplacementState &state, const Expression &parent, const Expression &child,
                             bool parent_at_select_position) {
	switch (parent.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION:
		return parent_at_select_position;
	case ExpressionClass::BOUND_AGGREGATE:
		return IsKeptFilterRoot(state, parent, child);
	default:
		return false;
	}
}

//! Whether the executor can skip an operand after the first for rows the earlier ones decide (conjunctions: in Select)
static bool EvaluatesLaterChildrenLazily(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION ||
	       expr.GetExpressionType() == ExpressionType::OPERATOR_COALESCE;
}

void CommonSubExpressionOptimizer::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		ExtractCommonSubExpressions(op);
		break;
	default:
		break;
	}
	LogicalOperatorVisitor::VisitOperator(op);
}

void CommonSubExpressionOptimizer::CountExpressions(Expression &expr, CSEReplacementState &state) {
	const bool kept_filter_root = state.at_kept_filter_root;
	state.at_kept_filter_root = false;
	const bool select_position = state.at_select_position;
	state.at_select_position = false;
	// we only consider expressions with children for CSE elimination
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_PARAMETER:
	// a lambda has no value of its own, so there is nothing to share between two occurrences of it
	case ExpressionClass::BOUND_LAMBDA:
		return;
	default:
		break;
	}
	if (kept_filter_root && !state.counted_filters.insert(expr).second) {
		return;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE && !expr.IsVolatile() && !state.inside_try &&
	    !kept_filter_root) {
		// we can't move aggregates to a projection, so we only consider the children of the aggregate
		auto &node = state.expression_count[expr];
		node.count++;
		// hoisting for a lazy occurrence alone would evaluate it for rows that never reach it; a hoisted conjunction
		// runs through Execute, which evaluates the operands Select would skip
		const bool short_circuits = select_position && expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION;
		if (!state.inside_lazy_child && !short_circuits) {
			node.has_eager_occurrence = true;
		}
	}

	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		// every condition runs through Select, and every child after the first condition is lazy
		const auto save_inside_lazy_child = state.inside_lazy_child;
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		for (auto &case_check : case_expr.CaseChecksMutable()) {
			state.at_select_position = true;
			CountExpressions(*case_check.when_expr, state);
			state.inside_lazy_child = true;
			CountExpressions(*case_check.then_expr, state);
		}
		CountExpressions(*case_expr.ElseMutable(), state);
		state.inside_lazy_child = save_inside_lazy_child;
		return;
	}
	if (EvaluatesLaterChildrenLazily(expr)) {
		const auto save_inside_lazy_child = state.inside_lazy_child;
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			state.at_select_position = IsSelectPosition(state, expr, child, select_position);
			CountExpressions(child, state);
			state.inside_lazy_child = true;
		});
		state.inside_lazy_child = save_inside_lazy_child;
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_OPERATOR: {
		const auto save_inside_try = state.inside_try;
		const auto expression_type = expr.GetExpressionType();
		if (expression_type == ExpressionType::OPERATOR_TRY || expression_type == ExpressionType::OPERATOR_HAS_ERROR) {
			state.inside_try = true;
		}
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { CountExpressions(child, state); });
		state.inside_try = save_inside_try;
		break;
	}
	default:
		// recursively count the children
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			state.at_kept_filter_root = IsKeptFilterRoot(state, expr, child);
			state.at_select_position = IsSelectPosition(state, expr, child, select_position);
			CountExpressions(child, state);
		});
		break;
	}
}

void CommonSubExpressionOptimizer::PerformCSEReplacement(unique_ptr<Expression> &expr_ptr, CSEReplacementState &state) {
	Expression &expr = *expr_ptr;
	const bool kept_filter_root = state.at_kept_filter_root;
	state.at_kept_filter_root = false;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &bound_column_ref = expr.Cast<BoundColumnRefExpression>();
		// bound column ref, check if this one has already been recorded in the expression list
		auto column_entry = state.column_map.find(bound_column_ref.Binding());
		if (column_entry == state.column_map.end()) {
			// not there yet: push the expression
			auto new_col_ref = make_uniq<BoundColumnRefExpression>(
			    bound_column_ref.GetAlias(), bound_column_ref.GetReturnType(), bound_column_ref.Binding());
			auto new_column_index = ColumnBinding::PushExpression(state.expressions, std::move(new_col_ref));
			state.column_map[bound_column_ref.Binding()] = new_column_index;
			bound_column_ref.BindingMutable() = ColumnBinding(state.projection_index, new_column_index);
		} else {
			// else: just update the column binding!
			bound_column_ref.BindingMutable() = ColumnBinding(state.projection_index, column_entry->second);
		}
		return;
	}
	// check if this child is eligible for CSE elimination
	if (!state.inside_try && !kept_filter_root && state.expression_count.find(expr) != state.expression_count.end()) {
		auto &node = state.expression_count[expr];
		if (node.ShouldExtract()) {
			// this expression occurs more than once! push it into the projection
			// check if it has already been pushed into the projection
			auto alias = expr.GetAlias();
			auto type = expr.GetReturnType();
			if (!node.column_index.IsValid()) {
				// has not been pushed yet: push it
				node.column_index = ColumnBinding::PushExpression(state.expressions, std::move(expr_ptr));
			} else {
				state.cached_expressions.push_back(std::move(expr_ptr));
			}
			// replace the original expression with a bound column ref
			expr_ptr = make_uniq<BoundColumnRefExpression>(alias, type,
			                                               ColumnBinding(state.projection_index, node.column_index));
			return;
		}
	}
	// this expression only occurs once, we can't perform CSE elimination
	// look into the children to see if we can replace them
	const auto save_inside_try = state.inside_try;
	const auto expression_type = expr.GetExpressionType();
	if (expression_type == ExpressionType::OPERATOR_TRY || expression_type == ExpressionType::OPERATOR_HAS_ERROR) {
		state.inside_try = true;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
		state.at_kept_filter_root = IsKeptFilterRoot(state, expr, *child);
		PerformCSEReplacement(child, state);
	});
	state.inside_try = save_inside_try;
}

void CommonSubExpressionOptimizer::ExtractCommonSubExpressions(LogicalOperator &op) {
	D_ASSERT(op.children.size() == 1);

	// first we count for each expression with children how many types it occurs
	CSEReplacementState state;
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggr = op.Cast<LogicalAggregate>();
		state.keep_aggregate_filters = aggr.groups.empty() && aggr.grouping_sets.size() <= 1;
	}
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *child) { CountExpressions(**child, state); });
	// check if there are any expressions to extract
	bool perform_replacement = false;
	for (auto &expr : state.expression_count) {
		if (expr.second.ShouldExtract()) {
			perform_replacement = true;
			break;
		}
	}
	if (!perform_replacement) {
		// no CSEs to extract
		return;
	}
	state.projection_index = binder.GenerateTableIndex();
	// we found common subexpressions to extract
	// now we iterate over all the expressions and perform the actual CSE elimination

	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *child) { PerformCSEReplacement(*child, state); });
	D_ASSERT(state.expressions.size() > 0);
	// create a projection node as the child of this node
	auto projection = make_uniq<LogicalProjection>(state.projection_index, std::move(state.expressions));
	if (op.children[0]->has_estimated_cardinality) {
		projection->SetEstimatedCardinality(op.children[0]->estimated_cardinality);
	}
	projection->children.push_back(std::move(op.children[0]));
	op.children[0] = std::move(projection);
}

} // namespace duckdb
