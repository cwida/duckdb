#include "optimizer/rule/conjunction_simplification.hpp"
#include "execution/expression_executor.hpp"

using namespace duckdb;
using namespace std;

ConjunctionSimplificationRule::ConjunctionSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a ComparisonExpression that has a ConstantExpression as a check
	auto op = make_unique<ConjunctionExpressionMatcher>();
	op->matchers.push_back(make_unique<FoldableConstantMatcher>());
	op->policy = SetMatcher::Policy::SOME;
	root = move(op);
}

unique_ptr<Expression> ConjunctionSimplificationRule::Apply(LogicalOperator &op, vector<Expression *> &bindings,
                                                           bool &changes_made) {
	auto conjunction = (ConjunctionExpression*) bindings[0];
	auto constant_expr = bindings[1];
	// the constant_expr is a scalar expression that we have to fold
	// use an ExpressionExecutor to execute the expression
	assert(constant_expr->IsScalar());
	auto constant_value = ExpressionExecutor::EvaluateScalar(*constant_expr).CastAs(TypeId::BOOLEAN);
	if (conjunction->type == ExpressionType::CONJUNCTION_AND) {
		if (!constant_value.is_null && !constant_value.value_.boolean) {
			// non-null FALSE in AND, result of expression is false
			return make_unique<ConstantExpression>(Value::BOOLEAN(false));
		}
	} else if (conjunction->type == ExpressionType::CONJUNCTION_OR) {
		if (!constant_value.is_null && constant_value.value_.boolean) {
			// non-null TRUE in OR, result of expression is true
			return make_unique<ConstantExpression>(Value::BOOLEAN(true));
		}
	}
	return nullptr;
}
