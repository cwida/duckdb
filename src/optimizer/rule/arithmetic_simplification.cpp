#include "duckdb/optimizer/rule/arithmetic_simplification.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

ArithmeticSimplificationRule::ArithmeticSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on an OperatorExpression that has a ConstantExpression as child
	auto op = make_uniq<FunctionExpressionMatcher>();
	op->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	op->matchers.push_back(make_uniq<ExpressionMatcher>());
	op->policy = SetMatcher::Policy::SOME;
	op->function = make_uniq<ManyFunctionMatcher>(identifier_set_t {"+", "-", "*", "/", "//"});
	// and only with numeric results
	op->type = make_uniq<NumericTypeMatcher>();
	op->matchers[0]->type = make_uniq<NumericTypeMatcher>();
	op->matchers[1]->type = make_uniq<NumericTypeMatcher>();
	root = std::move(op);
}

unique_ptr<Expression> ArithmeticSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                           bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant = bindings[1].get().Cast<BoundConstantExpression>();
	idx_t constant_child = root.GetChildren()[0].get() == &constant ? 0 : 1;
	D_ASSERT(root.GetChildren().size() == 2);
	(void)root;
	// any arithmetic operator involving NULL is always NULL
	if (constant.GetValue().IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(root.GetReturnType()));
	}
	auto &func_name = root.Function().GetName();
	if (func_name == "/") {
		if (constant_child == 1 && constant.GetValue() == 1 &&
		    (root.GetChildren()[0]->GetReturnType().id() == LogicalTypeId::FLOAT ||
		     root.GetChildren()[0]->GetReturnType().id() == LogicalTypeId::DOUBLE)) {
			return std::move(root.GetChildrenMutable()[0]);
		}
		return nullptr;
	}
	if (!root.GetReturnType().IsIntegral()) {
		return nullptr;
	}
	if (func_name == "+") {
		if (constant.GetValue() == 0) {
			// addition with 0
			// we can remove the entire operator and replace it with the non-constant child
			return Expression::PreserveReturnType(root.GetReturnType(),
			                                      std::move(root.GetChildrenMutable()[1 - constant_child]));
		}
	} else if (func_name == "-") {
		if (constant_child == 1 && constant.GetValue() == 0) {
			// subtraction by 0
			// we can remove the entire operator and replace it with the non-constant child
			return Expression::PreserveReturnType(root.GetReturnType(),
			                                      std::move(root.GetChildrenMutable()[1 - constant_child]));
		}
	} else if (func_name == "*") {
		if (constant.GetValue() == 1) {
			// multiply with 1, replace with non-constant child
			return Expression::PreserveReturnType(root.GetReturnType(),
			                                      std::move(root.GetChildrenMutable()[1 - constant_child]));
		} else if (constant.GetValue() == 0) {
			// multiply by zero: replace with constant or null
			return ExpressionRewriter::ConstantOrNull(GetContext(),
			                                          std::move(root.GetChildrenMutable()[1 - constant_child]),
			                                          Value::Numeric(root.GetReturnType(), 0));
		}
	} else if (func_name == "//") {
		if (constant_child == 1) {
			if (constant.GetValue() == 1) {
				// divide by 1, replace with non-constant child
				return Expression::PreserveReturnType(root.GetReturnType(),
				                                      std::move(root.GetChildrenMutable()[1 - constant_child]));
			} else if (constant.GetValue() == 0 && !Settings::Get<ErrorOnDivisionByZeroSetting>(rewriter.context)) {
				// divide by 0, replace with NULL
				return make_uniq<BoundConstantExpression>(Value(root.GetReturnType()));
			}
		}
	} else {
		throw InternalException("Unrecognized function name in ArithmeticSimplificationRule");
	}
	return nullptr;
}
} // namespace duckdb
