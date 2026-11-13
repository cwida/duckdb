#include "duckdb/planner/logical_operator_visitor.hpp"

#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/list.hpp"

namespace duckdb {

void LogicalOperatorVisitor::VisitOperator(unique_ptr<LogicalOperator> &op) {
	VisitOperator(*op);
}

void LogicalOperatorVisitor::VisitOperator(LogicalOperator &op) {
	VisitOperatorChildren(op);
	VisitOperatorExpressions(op);
}

void LogicalOperatorVisitor::VisitOperatorChildren(LogicalOperator &op) {
	if (op.HasProjectionMap()) {
		VisitOperatorWithProjectionMapChildren(op);
	} else {
		for (auto &child : op.children) {
			VisitOperator(child);
		}
	}
}

void LogicalOperatorVisitor::VisitOperatorWithProjectionMapChildren(LogicalOperator &op) {
	D_ASSERT(op.HasProjectionMap());
	for (idx_t child_index = 0; child_index < op.children.size(); child_index++) {
		auto projection_map = GetProjectionMap(op, child_index);
		if (!projection_map) {
			throw NotImplementedException("VisitOperatorWithProjectionMapChildren for %s", EnumUtil::ToString(op.type));
		}
		VisitChildOfOperatorWithProjectionMap(op.children[child_index], *projection_map);
	}
}

optional_ptr<vector<ProjectionIndex>> LogicalOperatorVisitor::GetProjectionMap(LogicalOperator &op, idx_t child_index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN: {
		auto &join = op.Cast<LogicalJoin>();
		return child_index == 0 ? optional_ptr<vector<ProjectionIndex>>(join.left_projection_map)
		                        : optional_ptr<vector<ProjectionIndex>>(join.right_projection_map);
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		return op.Cast<LogicalOrder>().projection_map;
	case LogicalOperatorType::LOGICAL_FILTER:
		return op.Cast<LogicalFilter>().projection_map;
	default:
		return nullptr;
	}
}

void LogicalOperatorVisitor::RemapProjectionMap(vector<ProjectionIndex> &projection_map,
                                                const vector<ColumnBinding> &child_bindings_before,
                                                const vector<ColumnBinding> &child_bindings_after) {
	if (projection_map.empty() || child_bindings_before == child_bindings_after) {
		return;
	}
	vector<ProjectionIndex> new_projection_map;
	new_projection_map.reserve(projection_map.size());
	for (auto projection_index : projection_map) {
		auto &desired_binding = child_bindings_before[projection_index.GetIndex()];
		auto entry = std::find(child_bindings_after.begin(), child_bindings_after.end(), desired_binding);
		if (entry == child_bindings_after.end()) {
			projection_map.clear();
			return;
		}
		new_projection_map.emplace_back(NumericCast<idx_t>(entry - child_bindings_after.begin()));
	}
	projection_map = std::move(new_projection_map);
}

void LogicalOperatorVisitor::VisitChildOfOperatorWithProjectionMap(unique_ptr<LogicalOperator> &child,
                                                                   vector<ProjectionIndex> &projection_map) {
	const auto child_bindings_before = child->GetColumnBindings();
	VisitOperator(child);
	const auto child_bindings_after = child->GetColumnBindings();
	RemapProjectionMap(projection_map, child_bindings_before, child_bindings_after);
}

template <class OPERATOR, class FUNC>
static void EnumerateOperatorExpressions(OPERATOR &op, const FUNC &callback) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
		auto &get = op.template Cast<LogicalExpressionGet>();
		for (auto &expr_list : get.expressions) {
			for (auto &expr : expr_list) {
				callback(&expr);
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		auto &order = op.template Cast<LogicalOrder>();
		for (auto &node : order.orders) {
			callback(&node.expression);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_TOP_N: {
		auto &order = op.template Cast<LogicalTopN>();
		for (auto &node : order.orders) {
			callback(&node.expression);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_TOP_KEY_FILTER: {
		auto &filter = op.template Cast<LogicalTopKeyFilter>();
		for (auto &node : filter.orders) {
			callback(&node.expression);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		auto &distinct = op.template Cast<LogicalDistinct>();
		for (auto &target : distinct.distinct_targets) {
			callback(&target);
		}
		if (distinct.order_by) {
			for (auto &order : distinct.order_by->orders) {
				callback(&order.expression);
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		auto &rec = op.template Cast<LogicalRecursiveCTE>();

		for (auto &target : rec.key_targets) {
			callback(&target);
		}
		for (auto &aggregate : rec.payload_aggregates) {
			callback(&aggregate);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_INSERT: {
		auto &insert = op.template Cast<LogicalInsert>();
		if (insert.on_conflict_info.on_conflict_condition) {
			callback(&insert.on_conflict_info.on_conflict_condition);
		}
		if (insert.on_conflict_info.do_update_condition) {
			callback(&insert.on_conflict_info.do_update_condition);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN: {
		auto &join = op.template Cast<LogicalDependentJoin>();
		if (join.condition) {
			callback(&join.condition);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.template Cast<LogicalComparisonJoin>();
		for (auto &expr : join.duplicate_eliminated_columns) {
			callback(&expr);
		}
		for (auto &cond : join.conditions) {
			if (cond.IsComparison()) {
				callback(&cond.LeftReference());
				callback(&cond.RightReference());
			} else {
				callback(&cond.JoinExpressionReference());
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &join = op.template Cast<LogicalAnyJoin>();
		callback(&join.condition);
		break;
	}
	case LogicalOperatorType::LOGICAL_LIMIT: {
		auto &limit = op.template Cast<LogicalLimit>();
		if (limit.limit_val.GetExpression()) {
			callback(&limit.limit_val.GetExpression());
		}
		if (limit.offset_val.GetExpression()) {
			callback(&limit.offset_val.GetExpression());
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggr = op.template Cast<LogicalAggregate>();
		for (auto &group : aggr.groups) {
			callback(&group);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_MERGE_INTO: {
		auto &merge_into = op.template Cast<LogicalMergeInto>();
		for (auto &entry : merge_into.actions) {
			for (auto &action : entry.second) {
				if (action->condition) {
					callback(&action->condition);
				}
				for (auto &expr : action->expressions) {
					callback(&expr);
				}
			}
		}
		break;
	}
	default:
		break;
	}
	for (auto &expression : op.expressions) {
		callback(&expression);
	}
}

void LogicalOperatorVisitor::EnumerateExpressions(LogicalOperator &op,
                                                  const std::function<void(unique_ptr<Expression> *child)> &callback) {
	EnumerateOperatorExpressions(op, callback);
}

void LogicalOperatorVisitor::EnumerateExpressions(
    const LogicalOperator &op, const std::function<void(const unique_ptr<Expression> *child)> &callback) {
	EnumerateOperatorExpressions(op, callback);
}

void LogicalOperatorVisitor::VisitOperatorExpressions(LogicalOperator &op) {
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *child) { VisitExpression(child); });
}

void LogicalOperatorVisitor::VisitExpression(unique_ptr<Expression> *expression) {
	auto &expr = **expression;
	unique_ptr<Expression> result;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_AGGREGATE:
		result = VisitReplace(expr.Cast<BoundAggregateExpression>(), expression);
		break;
	case ExpressionClass::BOUND_CASE:
		result = VisitReplace(expr.Cast<BoundCaseExpression>(), expression);
		break;
	case ExpressionClass::BOUND_COLUMN_REF:
		result = VisitReplace(expr.Cast<BoundColumnRefExpression>(), expression);
		break;
	case ExpressionClass::BOUND_CONJUNCTION:
		result = VisitReplace(expr.Cast<BoundConjunctionExpression>(), expression);
		break;
	case ExpressionClass::BOUND_CONSTANT:
		result = VisitReplace(expr.Cast<BoundConstantExpression>(), expression);
		break;
	case ExpressionClass::BOUND_FUNCTION:
		result = VisitReplace(expr.Cast<BoundFunctionExpression>(), expression);
		break;
	case ExpressionClass::BOUND_LAMBDA:
		result = VisitReplace(expr.Cast<BoundLambdaExpression>(), expression);
		break;
	case ExpressionClass::BOUND_SUBQUERY:
		result = VisitReplace(expr.Cast<BoundSubqueryExpression>(), expression);
		break;
	case ExpressionClass::BOUND_OPERATOR:
		result = VisitReplace(expr.Cast<BoundOperatorExpression>(), expression);
		break;
	case ExpressionClass::BOUND_PARAMETER:
		result = VisitReplace(expr.Cast<BoundParameterExpression>(), expression);
		break;
	case ExpressionClass::BOUND_REF:
		result = VisitReplace(expr.Cast<BoundReferenceExpression>(), expression);
		break;
	case ExpressionClass::BOUND_DEFAULT:
		result = VisitReplace(expr.Cast<BoundDefaultExpression>(), expression);
		break;
	case ExpressionClass::BOUND_WINDOW:
		result = VisitReplace(expr.Cast<BoundWindowExpression>(), expression);
		break;
	case ExpressionClass::BOUND_UNNEST:
		result = VisitReplace(expr.Cast<BoundUnnestExpression>(), expression);
		break;
	default:
		throw InternalException("Unrecognized expression type in logical operator visitor");
	}
	if (result) {
		*expression = std::move(result);
	} else {
		// visit the children of this node
		VisitExpressionChildren(expr);
	}
}

void LogicalOperatorVisitor::VisitExpressionChildren(Expression &expr) {
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &expr) { VisitExpression(&expr); });
}

// these are all default methods that can be overridden
// we don't care about coverage here
// LCOV_EXCL_START
unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundAggregateExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundCaseExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundColumnRefExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundConjunctionExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundConstantExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundLambdaExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundDefaultExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundFunctionExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundOperatorExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundParameterExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundReferenceExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundSubqueryExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundWindowExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

unique_ptr<Expression> LogicalOperatorVisitor::VisitReplace(BoundUnnestExpression &expr,
                                                            unique_ptr<Expression> *expr_ptr) {
	return nullptr;
}

// LCOV_EXCL_STOP

} // namespace duckdb
