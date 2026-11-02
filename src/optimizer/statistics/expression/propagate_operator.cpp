#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! Casting a variable-size value to VARIANT throws if the encoded row exceeds the maximum size, even for TRY_CAST
static bool ContainsThrowingVariantCast(const Expression &expr) {
	if (BoundCastExpression::IsCast(expr)) {
		auto &cast = expr.Cast<BoundFunctionExpression>();
		auto source_type = BoundCastExpression::SourceType(cast);
		if (BoundCastExpression::TargetType(cast).id() == LogicalTypeId::VARIANT &&
		    source_type.id() != LogicalTypeId::VARIANT && !TypeIsConstantSize(source_type.InternalType())) {
			return true;
		}
	}
	bool result = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { result = result || ContainsThrowingVariantCast(child); });
	return result;
}

//! Folding a NULL check removes its child from the plan
static bool CanFoldNullCheck(const BoundOperatorExpression &expr) {
	auto &child = *expr.GetChildren()[0];
	return !child.IsVolatile() && !ContainsThrowingVariantCast(child);
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundOperatorExpression &expr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	bool all_have_stats = true;
	vector<unique_ptr<BaseStatistics>> child_stats;
	child_stats.reserve(expr.GetChildrenMutable().size());
	for (auto &child : expr.GetChildrenMutable()) {
		auto stats = PropagateExpression(child);
		if (!stats) {
			all_have_stats = false;
		}
		child_stats.push_back(std::move(stats));
	}
	if (!all_have_stats) {
		return nullptr;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::OPERATOR_COALESCE:
		// COALESCE, merge stats of all children
		for (idx_t i = 0; i < expr.GetChildrenMutable().size(); i++) {
			D_ASSERT(child_stats[i]);
			if (!child_stats[i]->CanHaveNoNull()) {
				// this child is always NULL, we can remove it from the coalesce
				// UNLESS there is only one node remaining
				if (expr.GetChildrenMutable().size() > 1) {
					expr.GetChildrenMutable().erase_at(i);
					child_stats.erase_at(i);
					i--;
					removed_expressions = true;
				}
			} else if (!child_stats[i]->CanHaveNull()) {
				// coalesce child cannot have NULL entries
				// this is the last coalesce node that influences the result
				// we can erase any children after this node
				if (i + 1 < expr.GetChildrenMutable().size()) {
					expr.GetChildrenMutable().erase(expr.GetChildrenMutable().begin() + NumericCast<int64_t>(i + 1),
					                                expr.GetChildrenMutable().end());
					child_stats.erase(child_stats.begin() + NumericCast<int64_t>(i + 1), child_stats.end());
					removed_expressions = true;
				}
				break;
			}
		}
		D_ASSERT(!expr.GetChildrenMutable().empty());
		D_ASSERT(expr.GetChildrenMutable().size() == child_stats.size());
		if (expr.GetChildrenMutable().size() == 1) {
			// coalesce of one entry: simply return that entry
			expr_ptr = Expression::PreserveReturnType(expr.GetReturnType(), std::move(expr.GetChildrenMutable()[0]));
		} else {
			// coalesce of multiple entries
			// merge the stats
			for (idx_t i = 1; i < expr.GetChildrenMutable().size(); i++) {
				child_stats[0]->Merge(*child_stats[i]);
			}
		}
		return std::move(child_stats[0]);
	case ExpressionType::OPERATOR_IS_NULL:
		if (!CanFoldNullCheck(expr)) {
			return nullptr;
		}
		if (!child_stats[0]->CanHaveNull()) {
			// child has no null values: x IS NULL will always be false
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
			return PropagateExpression(expr_ptr);
		}
		if (!child_stats[0]->CanHaveNoNull()) {
			// child has no valid values: x IS NULL will always be true
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
			return PropagateExpression(expr_ptr);
		}
		return nullptr;
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		if (!CanFoldNullCheck(expr)) {
			return nullptr;
		}
		if (!child_stats[0]->CanHaveNull()) {
			// child has no null values: x IS NOT NULL will always be true
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
			return PropagateExpression(expr_ptr);
		}
		if (!child_stats[0]->CanHaveNoNull()) {
			// child has no valid values: x IS NOT NULL will always be false
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
			return PropagateExpression(expr_ptr);
		}
		return nullptr;
	default:
		return nullptr;
	}
}

} // namespace duckdb
