#include "duckdb/optimizer/rule/case_simplification.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"

namespace duckdb {

CaseSimplificationRule::CaseSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a CaseExpression that has a ConstantExpression as a check
	auto op = make_uniq<CaseExpressionMatcher>();
	root = std::move(op);
}

unique_ptr<Expression> CaseSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundCaseExpression>();
	for (idx_t i = 0; i < root.CaseChecksMutable().size(); i++) {
		auto &case_check = root.CaseChecksMutable()[i];
		if (case_check.when_expr->IsFoldable()) {
			// the WHEN check is a foldable expression
			// use an ExpressionExecutor to execute the expression
			Value condition;
			if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), *case_check.when_expr, condition) ||
			    !condition.DefaultTryCastAs(LogicalType::BOOLEAN)) {
				// the condition cannot be evaluated: leave it to execution
				continue;
			}

			// fold based on the constant condition
			if (condition.IsNull() || !BooleanValue::Get(condition)) {
				// the condition is always false: remove this case check
				root.CaseChecksMutable().erase_at(i);
				i--;
			} else {
				// the condition is always true
				// move the THEN clause to the ELSE of the case
				root.ElseMutable() = std::move(case_check.then_expr);
				// remove this case check and any case checks after this one
				root.CaseChecksMutable().erase(root.CaseChecksMutable().begin() + NumericCast<int64_t>(i),
				                               root.CaseChecksMutable().end());
				break;
			}
		}
	}
	if (root.CaseChecksMutable().empty()) {
		// no case checks left: return the ELSE expression
		return Expression::PreserveReturnType(root.GetReturnType(), std::move(root.ElseMutable()));
	}
	return nullptr;
}

} // namespace duckdb
