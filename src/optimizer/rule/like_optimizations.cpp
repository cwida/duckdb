#include "duckdb/optimizer/rule/like_optimizations.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

LikeOptimizationRule::LikeOptimizationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a FunctionExpression that has a foldable ConstantExpression
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	// we match on LIKE ("~~"), NOT LIKE ("!~~"), GLOB ("~~~"), NOT GLOB ("!~~~"), ILIKE ("~~*"), and NOT ILIKE ("!~~*")
	func->function =
	    make_uniq<ManyFunctionMatcher>(identifier_set_t {Identifier("!~~"), Identifier("~~"), Identifier("!~~~"),
	                                                     Identifier("~~~"), Identifier("~~*"), Identifier("!~~*")});
	root = std::move(func);
}

static bool PatternIsConstant(const string &pattern, PatternMatchType match_type) {
	for (idx_t i = 0; i < pattern.size(); i++) {
		if (match_type == PatternMatchType::GLOB) {
			if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[' || pattern[i] == '\\') {
				return false;
			}
		} else {
			if (pattern[i] == '%' || pattern[i] == '_') {
				return false;
			}
		}
	}
	return true;
}

static bool PatternIsPrefix(const string &pattern, PatternMatchType match_type) {
	char wildcard_any = match_type == PatternMatchType::GLOB ? '*' : '%';
	idx_t i;
	for (i = pattern.size(); i > 0; i--) {
		if (pattern[i - 1] != wildcard_any) {
			break;
		}
	}
	if (i == pattern.size()) {
		// no trailing wildcard
		// cannot be a prefix
		return false;
	}
	// continue to look in the string
	// if there is a wildcard in the string (besides at the very end) this is not a prefix match
	for (; i > 0; i--) {
		if (match_type == PatternMatchType::GLOB) {
			if (pattern[i - 1] == '*' || pattern[i - 1] == '?' || pattern[i - 1] == '[' || pattern[i - 1] == '\\') {
				return false;
			}
		} else {
			if (pattern[i - 1] == '%' || pattern[i - 1] == '_') {
				return false;
			}
		}
	}
	return true;
}

static bool PatternIsSuffix(const string &pattern, PatternMatchType match_type) {
	char wildcard_any = match_type == PatternMatchType::GLOB ? '*' : '%';
	idx_t i;
	for (i = 0; i < pattern.size(); i++) {
		if (pattern[i] != wildcard_any) {
			break;
		}
	}
	if (i == 0) {
		// no leading wildcard
		// cannot be a suffix
		return false;
	}
	// continue to look in the string
	// if there is a wildcard in the string (besides at the beginning) this is not a suffix match
	for (; i < pattern.size(); i++) {
		if (match_type == PatternMatchType::GLOB) {
			if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[' || pattern[i] == '\\') {
				return false;
			}
		} else {
			if (pattern[i] == '%' || pattern[i] == '_') {
				return false;
			}
		}
	}
	return true;
}

static bool PatternIsContains(const string &pattern, PatternMatchType match_type) {
	char wildcard_any = match_type == PatternMatchType::GLOB ? '*' : '%';
	idx_t start;
	idx_t end;
	for (start = 0; start < pattern.size(); start++) {
		if (pattern[start] != wildcard_any) {
			break;
		}
	}
	for (end = pattern.size(); end > 0; end--) {
		if (pattern[end - 1] != wildcard_any) {
			break;
		}
	}
	if (start == 0 || end == pattern.size()) {
		// contains requires both a leading AND a trailing wildcard
		return false;
	}
	// check if there are any other special characters in the string
	for (idx_t i = start; i < end; i++) {
		if (match_type == PatternMatchType::GLOB) {
			if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[' || pattern[i] == '\\') {
				return false;
			}
		} else {
			if (pattern[i] == '%' || pattern[i] == '_') {
				return false;
			}
		}
	}
	return true;
}

unique_ptr<Expression> LikeOptimizationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                   bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant_expr = bindings[2].get().Cast<BoundConstantExpression>();
	D_ASSERT(root.GetChildren().size() == 2);

	if (constant_expr.GetValue().IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(root.GetReturnType()));
	}

	bool is_ilike = root.Function().GetName() == "~~*" || root.Function().GetName() == "!~~*";
	if (is_ilike) {
		return nullptr;
	}

	// the constant_expr is a scalar expression that we have to fold
	if (!constant_expr.IsFoldable()) {
		return nullptr;
	}

	Value constant_value;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), constant_expr, constant_value)) {
		return nullptr;
	}
	D_ASSERT(constant_value.type() == constant_expr.GetReturnType());
	auto &patt_str = StringValue::Get(constant_value);

	bool is_not_like = root.Function().GetName() == "!~~" || root.Function().GetName() == "!~~~";
	PatternMatchType match_type = (root.Function().GetName() == "~~~" || root.Function().GetName() == "!~~~")
	                                  ? PatternMatchType::GLOB
	                                  : PatternMatchType::LIKE;
	if (PatternIsConstant(patt_str, match_type)) {
		// Pattern is constant
		return BoundComparisonExpression::Create(
		    is_not_like ? ExpressionType::COMPARE_NOTEQUAL : ExpressionType::COMPARE_EQUAL,
		    std::move(root.GetChildrenMutable()[0]), std::move(root.GetChildrenMutable()[1]));
	} else if (PatternIsPrefix(patt_str, match_type)) {
		// Prefix LIKE pattern : [^%_]*[%]+, ignoring underscore
		return ApplyRule(root, "prefix", patt_str, is_not_like, match_type);
	} else if (PatternIsSuffix(patt_str, match_type)) {
		// Suffix LIKE pattern: [%]+[^%_]*, ignoring underscore
		return ApplyRule(root, "suffix", patt_str, is_not_like, match_type);
	} else if (PatternIsContains(patt_str, match_type)) {
		// Contains LIKE pattern: [%]+[^%_]*[%]+, ignoring underscore
		return ApplyRule(root, "contains", patt_str, is_not_like, match_type);
	}
	return nullptr;
}

unique_ptr<Expression> LikeOptimizationRule::ApplyRule(BoundFunctionExpression &expr, const Identifier &function_name,
                                                       string pattern, bool is_not_like,
                                                       PatternMatchType match_type) const {
	// replace LIKE by an optimized function
	auto result = BindBuiltinScalarFunction(GetContext(), function_name, std::move(expr.GetChildrenMutable()));

	// removing wildcard from the pattern
	char wildcard_any = match_type == PatternMatchType::GLOB ? '*' : '%';
	pattern.erase(std::remove(pattern.begin(), pattern.end(), wildcard_any), pattern.end());

	result->GetChildrenMutable()[1] = make_uniq<BoundConstantExpression>(Value(std::move(pattern)));

	if (!is_not_like) {
		return std::move(result);
	}

	auto negation = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	negation->GetChildrenMutable().push_back(std::move(result));
	return std::move(negation);
}

} // namespace duckdb
