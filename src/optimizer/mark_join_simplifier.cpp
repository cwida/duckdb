#include "duckdb/optimizer/mark_join_simplifier.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

MarkJoinSimplifier::MarkJoinSimplifier(Optimizer &optimizer) : optimizer(optimizer) {
}

static bool ComparisonInputIsNotNull(NotNullExpressionAnalyzer &analyzer, LogicalOperator &op, const Expression &expr) {
	// These comparisons can produce NULL from nested members.
	if (expr.GetReturnType().id() == LogicalTypeId::TUPLE || expr.GetReturnType().id() == LogicalTypeId::UNION) {
		return false;
	}
	return analyzer.IsNotNull(op, expr);
}

static bool JoinConditionsCannotBeNull(NotNullExpressionAnalyzer &analyzer, LogicalComparisonJoin &join) {
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison()) {
			return false;
		}
		if (condition.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM ||
		    condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		if (!ComparisonInputIsNotNull(analyzer, *join.children[0], condition.GetLHS()) ||
		    !ComparisonInputIsNotNull(analyzer, *join.children[1], condition.GetRHS())) {
			return false;
		}
	}
	return true;
}

static bool IsMarker(const Expression &expr, ColumnBinding marker) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	const auto &column = expr.Cast<BoundColumnRefExpression>();
	return column.Depth() == 0 && column.Binding() == marker;
}

static bool IsMarkerPredicate(const Expression &expr, ColumnBinding marker) {
	if (expr.GetReturnType() != LogicalType::BOOLEAN) {
		return false;
	}
	if (IsMarker(expr, marker)) {
		return true;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::VALUE_CONSTANT:
		return true;
	case ExpressionType::OPERATOR_NOT:
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
	case ExpressionType::CONJUNCTION_AND:
	case ExpressionType::CONJUNCTION_OR:
		break;
	default:
		if (!BoundComparisonExpression::IsComparison(expr)) {
			return false;
		}
	}
	bool supported = true;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { supported &= IsMarkerPredicate(child, marker); });
	return supported;
}

struct MarkerPredicate {
	bool accepts_true = false;
	bool accepts_false = false;
	bool accepts_null = false;

	bool AcceptsOnlyTrue() const {
		return accepts_true && !accepts_false && !accepts_null;
	}

	bool AcceptsOneNonNullValue() const {
		return accepts_true != accepts_false;
	}
};

static MarkerPredicate AnalyzeMarkerPredicate(ClientContext &context, const Expression &expr, ColumnBinding marker) {
	MarkerPredicate result;
	if (IsMarker(expr, marker)) {
		result.accepts_true = true;
		return result;
	}
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT &&
	    IsMarker(*expr.Cast<BoundOperatorExpression>().GetChildren()[0], marker)) {
		result.accepts_false = true;
		return result;
	}
	if (!IsMarkerPredicate(expr, marker)) {
		return result;
	}
	auto evaluate = [&](const Value &marker_value, bool &accepted) {
		auto copy = expr.Copy();
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    copy, [&](BoundColumnRefExpression &, unique_ptr<Expression> &ref) {
			    ref = make_uniq<BoundConstantExpression>(marker_value);
		    });
		Value evaluated;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *copy, evaluated)) {
			return false;
		}
		accepted = !evaluated.IsNull() && evaluated.GetValue<bool>();
		return true;
	};
	if (!evaluate(Value::BOOLEAN(true), result.accepts_true) ||
	    !evaluate(Value::BOOLEAN(false), result.accepts_false) ||
	    !evaluate(Value(LogicalType::BOOLEAN), result.accepts_null)) {
		return {};
	}
	return result;
}

static void SimplifyNullSafeSemiJoinConditions(NotNullExpressionAnalyzer &analyzer, LogicalComparisonJoin &join) {
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		// A UNION's active member may be NULL even when the UNION itself is not.
		if (condition.GetLHS().GetReturnType().id() == LogicalTypeId::UNION) {
			continue;
		}
		if (analyzer.IsNotNull(*join.children[0], condition.GetLHS()) ||
		    analyzer.IsNotNull(*join.children[1], condition.GetRHS())) {
			condition.SetComparisonType(ExpressionType::COMPARE_EQUAL);
		}
	}
}

bool MarkJoinSimplifier::TrySimplify(LogicalFilter &filter, LogicalOperator &root,
                                     NotNullExpressionAnalyzer &nullability) {
	if (filter.children[0]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = filter.children[0]->Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::MARK || !join.duplicate_eliminated_columns.empty()) {
		// Retain unresolved correlation and DELIM semantics (issue #22267).
		return false;
	}
	const ColumnBinding marker(join.mark_index, ProjectionIndex(0));
	for (idx_t i = 0; i < filter.expressions.size(); i++) {
		auto predicate = AnalyzeMarkerPredicate(optimizer.context, *filter.expressions[i], marker);
		auto target_type = JoinType::INVALID;
		if (predicate.AcceptsOnlyTrue()) {
			target_type = JoinType::SEMI;
		} else if (predicate.AcceptsOneNonNullValue() && JoinConditionsCannotBeNull(nullability, join)) {
			target_type = predicate.accepts_true ? JoinType::SEMI : JoinType::ANTI;
		}
		if (target_type == JoinType::INVALID) {
			continue;
		}
		// Reordering the converted join can change which rows evaluate volatile expressions.
		if (filter.HasVolatileExpressions()) {
			return false;
		}
		for (auto &condition : join.conditions) {
			// Reordering can evaluate conditions on rows removed by other joins.
			auto can_throw = condition.IsComparison() ? condition.GetLHS().CanThrow() || condition.GetRHS().CanThrow()
			                                          : condition.GetJoinExpression().CanThrow();
			if (can_throw) {
				return false;
			}
			// MARK tuple inequality uses row equality, unlike ordinary nested inequality.
			if (condition.IsComparison() && condition.GetComparisonType() == ExpressionType::COMPARE_NOTEQUAL &&
			    condition.GetLHS().GetReturnType().id() == LogicalTypeId::TUPLE) {
				return false;
			}
		}
		join.ResolveOperatorTypes();
		const auto old_bindings = join.GetColumnBindings();
		vector<unique_ptr<Expression>> expressions;
		for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
			if (old_bindings[col_idx] == marker) {
				expressions.push_back(
				    make_uniq<BoundConstantExpression>(Value::BOOLEAN(target_type == JoinType::SEMI)));
			} else {
				expressions.push_back(make_uniq<BoundColumnRefExpression>(join.types[col_idx], old_bindings[col_idx]));
			}
		}
		join.join_type = target_type;
		join.mark_types.clear();
		filter.expressions.erase_at(i);
		if (target_type == JoinType::SEMI) {
			SimplifyNullSafeSemiJoinConditions(nullability, join);
		}
		auto projection = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(expressions));
		projection->children.push_back(std::move(filter.children[0]));
		CorrelatedColumnBindingReplacer replacer;
		replacer.AddReplacements(old_bindings, projection->GetColumnBindings());
		replacer.BuildIndex();
		replacer.stop_operator = projection.get();
		filter.children[0] = std::move(projection);
		replacer.VisitOperator(root);
		return true;
	}
	return false;
}

void MarkJoinSimplifier::Rewrite(unique_ptr<LogicalOperator> &op, LogicalOperator &root,
                                 NotNullExpressionAnalyzer &nullability) {
	for (auto &child : op->children) {
		Rewrite(child, root, nullability);
	}
	if (op->type != LogicalOperatorType::LOGICAL_FILTER) {
		return;
	}
	auto &filter = op->Cast<LogicalFilter>();
	if (TrySimplify(filter, root, nullability) && filter.expressions.empty() && !filter.HasProjectionMap()) {
		op = std::move(filter.children[0]);
	}
}

void MarkJoinSimplifier::Optimize(unique_ptr<LogicalOperator> &plan) {
	NotNullExpressionAnalyzer nullability(optimizer.context, *plan);
	Rewrite(plan, *plan, nullability);
}

} // namespace duckdb
