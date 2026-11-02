#include "duckdb/optimizer/rule/string_prefix.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/optimizer/matcher/type_matcher_id.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

static unique_ptr<Expression> RewriteAsEquality(unique_ptr<Expression> input, const Value &constant) {
	return BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(input),
	                                         make_uniq<BoundConstantExpression>(constant));
}

static unique_ptr<BoundConstantExpression> RewriteAsNull(const BoundFunctionExpression &comparison) {
	return make_uniq<BoundConstantExpression>(Value(comparison.GetReturnType()));
}

StringPrefixRule::StringPrefixRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::COMPARE_EQUAL);
	op->policy = SetMatcher::Policy::UNORDERED;

	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function =
	    make_uniq<ManyFunctionMatcher>(identifier_set_t {"left", "array_slice", "list_slice", "substring", "substr"});
	func->type = make_uniq<TypeMatcherId>(LogicalTypeId::VARCHAR);

	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());

	// Allow both two-argument and three-argument prefix extraction functions
	func->policy = SetMatcher::Policy::SOME_ORDERED;
	op->matchers.push_back(std::move(func));

	auto constant = make_uniq<ConstantExpressionMatcher>();
	constant->type = make_uniq<TypeMatcherId>(LogicalTypeId::VARCHAR);
	op->matchers.push_back(std::move(constant));

	root = std::move(op);
}

InstrPrefixRule::InstrPrefixRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::COMPARE_EQUAL);
	op->policy = SetMatcher::Policy::UNORDERED;

	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function = make_uniq<ManyFunctionMatcher>(identifier_set_t {"instr", "position", "strpos"});
	func->type = make_uniq<TypeMatcherId>(LogicalTypeId::BIGINT);
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	op->matchers.push_back(std::move(func));

	auto constant = make_uniq<ConstantExpressionMatcher>();
	constant->type = make_uniq<TypeMatcherId>(LogicalTypeId::BIGINT);
	op->matchers.push_back(std::move(constant));

	root = std::move(op);
}

unique_ptr<Expression> StringPrefixRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                               bool &changes_made, bool is_root) {
	const auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &func = bindings[1].get().Cast<BoundFunctionExpression>();
	const auto &constant = bindings[4].get().Cast<BoundConstantExpression>();
	auto &children = func.GetChildrenMutable();

	if (constant.GetValue().IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(comparison.GetReturnType()));
	}

	int64_t num_characters;
	auto &function_name = func.Function().GetName();
	if (function_name == "left") {
		D_ASSERT(children.size() == 2);
		auto &count = children[1]->Cast<BoundConstantExpression>().GetValue();
		if (count.IsNull()) {
			return RewriteAsNull(comparison);
		}
		num_characters = count.GetValue<int64_t>();
		// Left counts from the end for negative values. Larger values must preserve the error.
		if (num_characters < 0 || num_characters > NumericLimits<uint32_t>::Maximum()) {
			return nullptr;
		}
	} else if (function_name == "substring" || function_name == "substr") {
		D_ASSERT(children.size() == 2 || children.size() == 3);
		auto &start = children[1]->Cast<BoundConstantExpression>().GetValue();
		if (start.IsNull()) {
			return RewriteAsNull(comparison);
		}
		if (start.GetValue<int64_t>() != 1) {
			return nullptr;
		}
		if (children.size() == 2) {
			// substr(s, 1) is the same as comparing the entire string
			return RewriteAsEquality(std::move(children[0]), constant.GetValue());
		}
		if (children[2]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return nullptr;
		}
		auto &length = children[2]->Cast<BoundConstantExpression>().GetValue();
		if (length.IsNull()) {
			return RewriteAsNull(comparison);
		}
		num_characters = length.GetValue<int64_t>();
		if (num_characters < 0 || num_characters > NumericLimits<uint32_t>::Maximum()) {
			return nullptr;
		}
	} else {
		// s[x:y] -> array_slice(s, x, y)
		D_ASSERT(function_name == "array_slice" || function_name == "list_slice");
		D_ASSERT(children.size() == 3);
		if (children[2]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return nullptr;
		}

		auto &begin = children[1]->Cast<BoundConstantExpression>().GetValue();
		if (begin.IsNull()) {
			return RewriteAsNull(comparison);
		}
		if (begin.type().id() != LogicalTypeId::LIST) {
			// s[0:] == s[1:]
			const auto begin_index = begin.GetValue<int64_t>();
			if (begin_index != 0 && begin_index != 1) {
				return nullptr;
			}
		}

		auto &end = children[2]->Cast<BoundConstantExpression>().GetValue();
		if (end.IsNull()) {
			return RewriteAsNull(comparison);
		}
		if (end.type().id() == LogicalTypeId::LIST) {
			return RewriteAsEquality(std::move(children[0]), constant.GetValue());
		}
		num_characters = end.GetValue<int64_t>();
		if (num_characters < 0) {
			return nullptr;
		}
	}

	const auto constant_str = StringValue::Get(constant.GetValue());
	const auto constant_length =
	    Length<string_t, int64_t>(string_t(constant_str.c_str(), NumericCast<uint32_t>(constant_str.size())));

	// The constant is longer than the extracted prefix, so the comparison can only be FALSE
	if (constant_length > num_characters) {
		return ExpressionRewriter::ConstantOrNull(GetContext(), std::move(children[0]), Value::BOOLEAN(false));
	}

	// A prefix extraction can only return fewer than n characters if it returns the entire string.
	if (constant_length < num_characters) {
		return RewriteAsEquality(std::move(children[0]), constant.GetValue());
	}

	// Convert to prefix comparison
	vector<unique_ptr<Expression>> prefix_children;
	prefix_children.emplace_back(std::move(children[0]));
	prefix_children.emplace_back(make_uniq<BoundConstantExpression>(constant.GetValue()));
	return BindBuiltinScalarFunction(GetContext(), PrefixFun::Name, std::move(prefix_children));
}

unique_ptr<Expression> InstrPrefixRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                              bool &changes_made, bool is_root) {
	auto &func = bindings[1].get().Cast<BoundFunctionExpression>();
	auto &needle = bindings[3].get().Cast<BoundConstantExpression>();
	auto &position = bindings[4].get().Cast<BoundConstantExpression>();

	// If requested position is 1, rewrite as prefix comparison.
	if (position.GetValue().IsNull() || position.GetValue().GetValue<int64_t>() != 1) {
		return nullptr;
	}

	auto &children = func.GetChildrenMutable();
	D_ASSERT(children.size() == 2);
	vector<unique_ptr<Expression>> prefix_children;
	prefix_children.emplace_back(std::move(children[0]));
	prefix_children.emplace_back(make_uniq<BoundConstantExpression>(needle.GetValue()));
	return BindBuiltinScalarFunction(GetContext(), PrefixFun::Name, std::move(prefix_children));
}

} // namespace duckdb
