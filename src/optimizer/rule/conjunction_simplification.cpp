
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/conjunction_simplification.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

ConjunctionSimplificationRule::ConjunctionSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a ComparisonExpression that has a ConstantExpression as a check
	auto op = make_uniq<ConjunctionExpressionMatcher>();
	op->matchers.push_back(make_uniq<FoldableConstantMatcher>());
	op->policy = SetMatcher::Policy::SOME;
	root = std::move(op);
}

unique_ptr<Expression> ConjunctionSimplificationRule::RemoveExpression(BoundConjunctionExpression &conj,
                                                                       const Expression &expr) {
	for (idx_t i = 0; i < conj.GetChildrenMutable().size(); i++) {
		if (conj.GetChildrenMutable()[i].get() == &expr) {
			// erase the expression
			conj.GetChildrenMutable().erase_at(i);
			break;
		}
	}
	if (conj.GetChildrenMutable().size() == 1) {
		// one expression remaining: simply return that expression and erase the conjunction
		return std::move(conj.GetChildrenMutable()[0]);
	}
	return nullptr;
}

unique_ptr<Expression> ConjunctionSimplificationRule::Apply(LogicalOperator &op,
                                                            vector<reference<Expression>> &bindings, bool &changes_made,
                                                            bool is_root) {
	auto &conjunction = bindings[0].get().Cast<BoundConjunctionExpression>();
	auto &constant_expr = bindings[1].get();
	// the constant_expr is a scalar expression that we have to fold
	// use an ExpressionExecutor to execute the expression
	D_ASSERT(constant_expr.IsFoldable());
	if (!ExpressionRewriter::CanConstantFold(GetContext(), constant_expr)) {
		return nullptr;
	}
	Value constant_value;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), constant_expr, constant_value)) {
		return nullptr;
	}
	constant_value = constant_value.DefaultCastAs(LogicalType::BOOLEAN);
	if (constant_value.IsNull()) {
		// FALSE and NULL have the same effect in filters and joins
		if (conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_OR || !is_root ||
		    (op.type != LogicalOperatorType::LOGICAL_FILTER && op.type != LogicalOperatorType::LOGICAL_ANY_JOIN)) {
			return nullptr;
		}
		// Continue until every NULL is removed
		changes_made = true;
		return RemoveExpression(conjunction, constant_expr);
	}
	if (conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		if (!BooleanValue::Get(constant_value)) {
			// FALSE in AND, result of expression is false
			return make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
		} else {
			// TRUE in AND, remove the expression from the set
			return RemoveExpression(conjunction, constant_expr);
		}
	} else {
		D_ASSERT(conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_OR);
		if (!BooleanValue::Get(constant_value)) {
			// FALSE in OR, remove the expression from the set
			return RemoveExpression(conjunction, constant_expr);
		} else {
			// TRUE in OR, result of expression is true
			return make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		}
	}
}

} // namespace duckdb
