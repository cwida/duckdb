#include "duckdb/optimizer/cse_optimizer.hpp"

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
	ProjectionIndex column_index;

	CSENode() : count(1), column_index() {
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
	//! Short circuit argument tracking
	bool short_circuited = false;
	//! True while visiting the child of a TRY or HAS_ERROR, whose errors must stay inside the capture
	bool inside_try = false;
};

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
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE && !expr.IsVolatile() && !state.inside_try) {
		// we can't move aggregates to a projection, so we only consider the children of the aggregate
		auto node = state.expression_count.find(expr);
		if (node == state.expression_count.end()) {
			// first time we encounter this expression, insert this node with [count = 1]
			// but only if it is not an interior argument of a short circuit sensitive expression.
			if (!state.short_circuited) {
				state.expression_count[expr] = CSENode();
			}
		} else {
			// we encountered this expression before, increment the occurrence count
			node->second.count++;
		}
	}

	// If we have a function that uses short circuiting, then we can only extract CSEs from the leftmost
	// side of the argument tree (child_no == 0)
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION:
	case ExpressionClass::BOUND_CASE: {
		// Save the short circuit reference
		const auto save_short_circuit = state.short_circuited;
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			CountExpressions(child, state);
			state.short_circuited = true;
		});
		state.short_circuited = save_short_circuit;
		break;
	}
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
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { CountExpressions(child, state); });
		break;
	}
}

void CommonSubExpressionOptimizer::PerformCSEReplacement(unique_ptr<Expression> &expr_ptr, CSEReplacementState &state) {
	Expression &expr = *expr_ptr;
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
	if (!state.inside_try && state.expression_count.find(expr) != state.expression_count.end()) {
		auto &node = state.expression_count[expr];
		if (node.count > 1) {
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
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](unique_ptr<Expression> &child) { PerformCSEReplacement(child, state); });
	state.inside_try = save_inside_try;
}

void CommonSubExpressionOptimizer::ExtractCommonSubExpressions(LogicalOperator &op) {
	D_ASSERT(op.children.size() == 1);

	// first we count for each expression with children how many types it occurs
	CSEReplacementState state;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *child) { CountExpressions(**child, state); });
	// check if there are any expressions to extract
	bool perform_replacement = false;
	for (auto &expr : state.expression_count) {
		if (expr.second.count > 1) {
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
