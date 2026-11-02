#include "duckdb/optimizer/rule/constant_order_normalization.hpp"

#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

static bool MultiplicationConstantBelongsAtEnd(const BoundFunctionExpression &expr) {
	return expr.Function().GetName() == "*";
}

class RecursiveFunctionExpressionMatcher : public ExpressionMatcher {
public:
	explicit RecursiveFunctionExpressionMatcher(vector<unique_ptr<FunctionExpressionMatcher>> func_matchers)
	    : func_matchers(std::move(func_matchers)) {
	}
	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		FunctionExpressionMatcher *target_matcher = nullptr;
		for (const auto &matcher : func_matchers) {
			if (matcher->Match(expr, bindings)) {
				target_matcher = matcher.get();
				break;
			}
		}
		if (target_matcher == nullptr) {
			return false;
		}
		bindings.clear();
		RecursiveMatch(target_matcher, expr, bindings);
		bindings.push_back(expr);
		return true;
	}

private:
	void RecursiveMatch(FunctionExpressionMatcher *func_matcher, Expression &expr,
	                    vector<reference<Expression>> &bindings) {
		vector<reference<Expression>> curr_bindings;
		if (func_matcher->Match(expr, curr_bindings)) {
			auto &func_expr = expr.Cast<BoundFunctionExpression>();
			for (auto &child : func_expr.GetChildren()) {
				RecursiveMatch(func_matcher, *(child.get()), bindings);
			}
		} else {
			bindings.push_back(expr);
		}
	}

	vector<unique_ptr<FunctionExpressionMatcher>> func_matchers;
};

ConstantOrderNormalizationRule::ConstantOrderNormalizationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// '+' and '*' satisfy commutative law and associative law.
	auto add_matcher = make_uniq<FunctionExpressionMatcher>();
	add_matcher->function = make_uniq<SpecificFunctionMatcher>("+");
	add_matcher->type = make_uniq<IntegerTypeMatcher>();
	auto left_expression_matcher = make_uniq<ExpressionMatcher>();
	auto right_expression_matcher = make_uniq<ExpressionMatcher>();
	left_expression_matcher->type = make_uniq<IntegerTypeMatcher>();
	right_expression_matcher->type = make_uniq<IntegerTypeMatcher>();
	add_matcher->matchers.push_back(std::move(left_expression_matcher));
	add_matcher->matchers.push_back(std::move(right_expression_matcher));
	add_matcher->policy = SetMatcher::Policy::ORDERED;

	auto multiply_matcher = make_uniq<FunctionExpressionMatcher>();
	multiply_matcher->function = make_uniq<SpecificFunctionMatcher>("*");
	multiply_matcher->type = make_uniq<IntegerTypeMatcher>();
	left_expression_matcher = make_uniq<ExpressionMatcher>();
	right_expression_matcher = make_uniq<ExpressionMatcher>();
	left_expression_matcher->type = make_uniq<IntegerTypeMatcher>();
	right_expression_matcher->type = make_uniq<IntegerTypeMatcher>();
	multiply_matcher->matchers.push_back(std::move(left_expression_matcher));
	multiply_matcher->matchers.push_back(std::move(right_expression_matcher));
	multiply_matcher->policy = SetMatcher::Policy::ORDERED;

	vector<unique_ptr<FunctionExpressionMatcher>> func_matchers;
	func_matchers.push_back(std::move(add_matcher));
	func_matchers.push_back(std::move(multiply_matcher));
	auto op = make_uniq<RecursiveFunctionExpressionMatcher>(std::move(func_matchers));
	root = std::move(op);
}

unique_ptr<Expression> ConstantOrderNormalizationRule::Apply(LogicalOperator &op,
                                                             vector<reference<Expression>> &bindings,
                                                             bool &changes_made, bool is_root) {
	auto &root = bindings.back().get().Cast<BoundFunctionExpression>();
	const auto expression_count = bindings.size() - 1;

	// Put all constant expressions in front.
	vector<reference<Expression>> ordered_bindings;
	vector<reference<Expression>> remain_bindings;
	idx_t last_constant_position = 0;
	for (idx_t i = 0; i < bindings.size() - 1; ++i) {
		if (bindings[i].get().IsFoldable()) {
			ordered_bindings.push_back(bindings[i]);
			last_constant_position = i;
		} else {
			remain_bindings.push_back(bindings[i]);
		}
	}

	if (ordered_bindings.empty()) {
		return nullptr;
	}

	if (ordered_bindings.size() == 1) {
		// Multiplication trees can lose their folded constant subtree in the bottom-up rewriter
		// (e.g. 1 * 2 * x -> 2 * x). Normalize that remaining binary form to x * 2 so it
		// matches the shape produced by the other multiplication variants.
		if (!MultiplicationConstantBelongsAtEnd(root) || last_constant_position == expression_count - 1) {
			return nullptr;
		}
		remain_bindings.push_back(ordered_bindings[0]);
		ordered_bindings = std::move(remain_bindings);
	} else {
		if (last_constant_position == ordered_bindings.size() - 1) {
			return nullptr;
		}
		ordered_bindings.insert(ordered_bindings.end(), remain_bindings.begin(), remain_bindings.end());
	}

	// Reconstruct the expression.
	FunctionBinder binder(rewriter.context);
	unique_ptr<Expression> new_root = ordered_bindings[0].get().Copy();
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(new_root));
	for (idx_t i = 1; i < ordered_bindings.size(); ++i) {
		// Right child.
		children.push_back(ordered_bindings[i].get().Copy());
		new_root = binder.BindScalarFunction(Identifier::DefaultSchema(), root.Function().GetName(),
		                                     std::move(children), root.IsOperator());
		children.clear();
		// Left child.
		children.push_back(std::move(new_root));
	}

	D_ASSERT(children.size() == 1);
	D_ASSERT(children[0]->GetReturnType() == root.GetReturnType());

	return Expression::PreserveReturnType(root.GetReturnType(), std::move(children[0]));
}

} // namespace duckdb
