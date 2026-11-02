
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

void StatisticsPropagator::SimplifyConstantOrNull(vector<unique_ptr<Expression>> &expressions) {
	if (expressions.size() < 2) {
		return;
	}
	bool has_candidate = false;
	for (auto &expr : expressions) {
		has_candidate |= ExpressionIsConstantOrNull(*expr, Value::BOOLEAN(true));
	}
	if (!has_candidate) {
		return;
	}
	for (auto &expr : expressions) {
		// Removing a NULL check can expose more rows to the remaining predicates.
		if (expr->CanThrow() || expr->IsVolatile() || ExpressionBarrier::Contains(*expr)) {
			return;
		}
	}
	for (idx_t expr_idx = 0; expr_idx < expressions.size(); expr_idx++) {
		auto &expr = *expressions[expr_idx];
		if (!ExpressionIsConstantOrNull(expr, Value::BOOLEAN(true))) {
			continue;
		}
		auto &children = expr.Cast<BoundFunctionExpression>().GetChildren();
		D_ASSERT(children.size() >= 2);
		bool redundant = true;
		for (idx_t child_idx = 1; child_idx < children.size(); child_idx++) {
			auto &child = *children[child_idx];
			if (child.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				if (child.Cast<BoundConstantExpression>().GetValue().IsNull()) {
					redundant = false;
					break;
				}
				continue;
			}
			bool rejects_null = false;
			for (idx_t other_idx = 0; other_idx < expressions.size(); other_idx++) {
				if (other_idx != expr_idx && FilterRejectsNull(*expressions[other_idx], child)) {
					rejects_null = true;
					break;
				}
			}
			if (!rejects_null) {
				redundant = false;
				break;
			}
		}
		if (redundant) {
			expressions.erase_at(expr_idx);
			expr_idx--;
			removed_expressions = true;
		}
	}
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundConjunctionExpression &expr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	auto is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
	for (idx_t expr_idx = 0; expr_idx < expr.GetChildrenMutable().size(); expr_idx++) {
		auto &child = expr.GetChildrenMutable()[expr_idx];
		auto stats = PropagateExpression(child);
		if (!child->IsFoldable()) {
			continue;
		}
		// we have a constant in a conjunction
		// we (1) either prune the child
		// or (2) replace the entire conjunction with a constant
		auto constant = ExpressionExecutor::EvaluateScalar(context, *child);
		if (constant.IsNull()) {
			continue;
		}
		auto b = BooleanValue::Get(constant);
		bool prune_child = false;
		bool constant_value = true;
		if (b) {
			// true
			if (is_and) {
				// true in and: prune child
				prune_child = true;
			} else {
				// true in OR: replace with TRUE
				constant_value = true;
			}
		} else {
			// false
			if (is_and) {
				// false in AND: replace with FALSE
				constant_value = false;
			} else {
				// false in OR: prune child
				prune_child = true;
			}
		}
		if (prune_child) {
			expr.GetChildrenMutable().erase_at(expr_idx);
			expr_idx--;
			removed_expressions = true;
			continue;
		}
		expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(constant_value));
		return PropagateExpression(expr_ptr);
	}
	if (is_and) {
		SimplifyConstantOrNull(expr.GetChildrenMutable());
	}
	if (expr.GetChildrenMutable().empty()) {
		// if there are no children left, replace the conjunction with TRUE (for AND) or FALSE (for OR)
		expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(is_and));
		return PropagateExpression(expr_ptr);
	} else if (expr.GetChildrenMutable().size() == 1) {
		// if there is one child left, replace the conjunction with that one child
		expr_ptr = std::move(expr.GetChildrenMutable()[0]);
	}
	return nullptr;
}

} // namespace duckdb
