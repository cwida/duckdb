#include "duckdb/planner/expression_iterator.hpp"

#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/planner/tableref/list.hpp"

namespace duckdb {

void ExpressionIterator::EnumerateChildren(const Expression &expr,
                                           const std::function<void(const Expression &child)> &callback) {
	EnumerateChildren((Expression &)expr, [&](unique_ptr<Expression> &child) { callback(*child); });
}

void ExpressionIterator::EnumerateChildren(Expression &expr, const std::function<void(Expression &child)> &callback) {
	EnumerateChildren(expr, [&](unique_ptr<Expression> &child) { callback(*child); });
}

void ExpressionIterator::EnumerateChildren(Expression &expr,
                                           const std::function<void(unique_ptr<Expression> &child)> &callback) {
	switch (expr.expression_class) {
	case ExpressionClass::BOUND_AGGREGATE: {
		auto &aggr_expr = (BoundAggregateExpression &)expr;
		for (auto &child : aggr_expr.children) {
			callback(child);
		}
		if (aggr_expr.filter) {
			callback(aggr_expr.filter);
		}
		break;
	}
	case ExpressionClass::BOUND_BETWEEN: {
		auto &between_expr = (BoundBetweenExpression &)expr;
		callback(between_expr.input);
		callback(between_expr.lower);
		callback(between_expr.upper);
		break;
	}
	case ExpressionClass::BOUND_CASE: {
		auto &case_expr = (BoundCaseExpression &)expr;
		callback(case_expr.check);
		callback(case_expr.result_if_true);
		callback(case_expr.result_if_false);
		break;
	}
	case ExpressionClass::BOUND_CAST: {
		auto &cast_expr = (BoundCastExpression &)expr;
		callback(cast_expr.child);
		break;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp_expr = (BoundComparisonExpression &)expr;
		callback(comp_expr.left);
		callback(comp_expr.right);
		break;
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj_expr = (BoundConjunctionExpression &)expr;
		for (auto &child : conj_expr.children) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func_expr = (BoundFunctionExpression &)expr;
		for (auto &child : func_expr.children) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op_expr = (BoundOperatorExpression &)expr;
		for (auto &child : op_expr.children) {
			callback(child);
		}
		break;
	}
	case ExpressionClass::BOUND_SUBQUERY: {
		auto &subquery_expr = (BoundSubqueryExpression &)expr;
		if (subquery_expr.child) {
			callback(subquery_expr.child);
		}
		break;
	}
	case ExpressionClass::BOUND_WINDOW: {
		auto &window_expr = (BoundWindowExpression &)expr;
		for (auto &partition : window_expr.partitions) {
			callback(partition);
		}
		for (auto &order : window_expr.orders) {
			callback(order.expression);
		}
		for (auto &child : window_expr.children) {
			callback(child);
		}
		if (window_expr.start_expr) {
			callback(window_expr.start_expr);
		}
		if (window_expr.end_expr) {
			callback(window_expr.end_expr);
		}
		if (window_expr.offset_expr) {
			callback(window_expr.offset_expr);
		}
		if (window_expr.default_expr) {
			callback(window_expr.default_expr);
		}
		break;
	}
	case ExpressionClass::BOUND_UNNEST: {
		auto &unnest_expr = (BoundUnnestExpression &)expr;
		callback(unnest_expr.child);
		break;
	}
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_DEFAULT:
	case ExpressionClass::BOUND_PARAMETER:
	case ExpressionClass::BOUND_REF:
		// these node types have no children
		break;
	default:
		throw InternalException("ExpressionIterator used on unbound expression");
	}
}

void ExpressionIterator::EnumerateExpression(unique_ptr<Expression> &expr,
                                             const std::function<void(Expression &child)> &callback) {
	if (!expr) {
		return;
	}
	callback(*expr);
	ExpressionIterator::EnumerateChildren(*expr,
	                                      [&](unique_ptr<Expression> &child) { EnumerateExpression(child, callback); });
}

void ExpressionIterator::EnumerateTableRefChildren(BoundTableRef &ref,
                                                   const std::function<void(Expression &child)> &callback) {
	switch (ref.type) {
	case TableReferenceType::CROSS_PRODUCT: {
		auto &bound_crossproduct = (BoundCrossProductRef &)ref;
		EnumerateTableRefChildren(*bound_crossproduct.left, callback);
		EnumerateTableRefChildren(*bound_crossproduct.right, callback);
		break;
	}
	case TableReferenceType::JOIN: {
		auto &bound_join = (BoundJoinRef &)ref;
		EnumerateExpression(bound_join.condition, callback);
		EnumerateTableRefChildren(*bound_join.left, callback);
		EnumerateTableRefChildren(*bound_join.right, callback);
		break;
	}
	case TableReferenceType::SUBQUERY: {
		auto &bound_subquery = (BoundSubqueryRef &)ref;
		EnumerateQueryNodeChildren(*bound_subquery.subquery, callback);
		break;
	}
	default:
		D_ASSERT(ref.type == TableReferenceType::TABLE_FUNCTION || ref.type == TableReferenceType::BASE_TABLE ||
		         ref.type == TableReferenceType::EMPTY);
		break;
	}
}

void ExpressionIterator::EnumerateQueryNodeChildren(BoundQueryNode &node,
                                                    const std::function<void(Expression &child)> &callback) {
	switch (node.type) {
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &bound_setop = (BoundSetOperationNode &)node;
		EnumerateQueryNodeChildren(*bound_setop.left, callback);
		EnumerateQueryNodeChildren(*bound_setop.right, callback);
		break;
	}
	default:
		D_ASSERT(node.type == QueryNodeType::SELECT_NODE);
		auto &bound_select = (BoundSelectNode &)node;
		for (idx_t i = 0; i < bound_select.select_list.size(); i++) {
			EnumerateExpression(bound_select.select_list[i], callback);
		}
		EnumerateExpression(bound_select.where_clause, callback);
		for (idx_t i = 0; i < bound_select.groups.size(); i++) {
			EnumerateExpression(bound_select.groups[i], callback);
		}
		EnumerateExpression(bound_select.having, callback);
		for (idx_t i = 0; i < bound_select.aggregates.size(); i++) {
			EnumerateExpression(bound_select.aggregates[i], callback);
		}
		for (idx_t i = 0; i < bound_select.unnests.size(); i++) {
			EnumerateExpression(bound_select.unnests[i], callback);
		}
		for (idx_t i = 0; i < bound_select.windows.size(); i++) {
			EnumerateExpression(bound_select.windows[i], callback);
		}
		if (bound_select.from_table) {
			EnumerateTableRefChildren(*bound_select.from_table, callback);
		}
		break;
	}
	for (idx_t i = 0; i < node.modifiers.size(); i++) {
		switch (node.modifiers[i]->type) {
		case ResultModifierType::DISTINCT_MODIFIER:
			for (auto &expr : ((BoundDistinctModifier &)*node.modifiers[i]).target_distincts) {
				EnumerateExpression(expr, callback);
			}
			break;
		case ResultModifierType::ORDER_MODIFIER:
			for (auto &order : ((BoundOrderModifier &)*node.modifiers[i]).orders) {
				EnumerateExpression(order.expression, callback);
			}
			break;
		default:
			break;
		}
	}
}

} // namespace duckdb
