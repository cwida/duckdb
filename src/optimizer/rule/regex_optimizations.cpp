#include "duckdb/optimizer/rule/regex_optimizations.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/regexp.hpp"
#include "utf8proc_wrapper.hpp"

#include "re2/re2.h"
#include "re2/regexp.h"

namespace duckdb {

RegexOptimizationRule::RegexOptimizationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function = make_uniq<SpecificFunctionMatcher>("regexp_matches");
	func->policy = SetMatcher::Policy::SOME_ORDERED;
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());

	root = std::move(func);
}

struct LikeString {
	bool exists = true;
	bool escaped = false;
	string like_string = "";
};

static void AddCharacter(char chr, LikeString &ret, bool contains) {
	// if we are not converting into a contains, and the string has LIKE special characters
	// then don't return a possible LIKE match
	// same if the character is a control character
	if (iscntrl(static_cast<unsigned char>(chr)) || (!contains && (chr == '%' || chr == '_'))) {
		ret.exists = false;
		return;
	}
	auto run_as_str {chr};
	ret.like_string += run_as_str;
}

static void AddCodepoint(int32_t codepoint, LikeString &ret, bool contains) {
	int sz = 0;
	char utf8_str[4];
	if (!Utf8Proc::CodepointToUtf8(codepoint, sz, utf8_str)) {
		// invalid codepoint
		ret.exists = false;
		return;
	}
	for (idx_t i = 0; i < idx_t(sz); i++) {
		AddCharacter(utf8_str[i], ret, contains);
	}
}

static LikeString GetLikeStringEscaped(duckdb_re2::Regexp *regexp, bool contains = false) {
	D_ASSERT(regexp->op() == duckdb_re2::kRegexpLiteralString || regexp->op() == duckdb_re2::kRegexpLiteral);
	LikeString ret;

	if (regexp->parse_flags() & duckdb_re2::Regexp::FoldCase ||
	    !(regexp->parse_flags() & duckdb_re2::Regexp::OneLine)) {
		// parse flags can turn on and off within a regex match, return no optimization
		// For now, we just don't optimize if these every turn on.
		// TODO: logic to attempt the optimization, then if the parse flags change, then abort
		ret.exists = false;
		return ret;
	}

	// case insensitivity may be on now, but it can also turn off.
	if (regexp->op() == duckdb_re2::kRegexpLiteralString) {
		auto nrunes = (idx_t)regexp->nrunes();
		auto runes = regexp->runes();
		for (idx_t i = 0; i < nrunes; i++) {
			AddCodepoint(runes[i], ret, contains);
			if (!ret.exists) {
				return ret;
			}
		}
	} else {
		auto rune = regexp->rune();
		AddCodepoint(rune, ret, contains);
	}
	D_ASSERT(ret.like_string.size() >= 1 || !ret.exists);
	return ret;
}

static LikeString LikeMatchFromRegex(duckdb_re2::RE2 &pattern) {
	LikeString ret = LikeString();
	auto num_subs = pattern.Regexp()->nsub();
	auto subs = pattern.Regexp()->sub();
	auto cur_sub_index = 0;
	while (cur_sub_index < num_subs) {
		switch (subs[cur_sub_index]->op()) {
		case duckdb_re2::kRegexpAnyChar:
			if (cur_sub_index == 0) {
				ret.like_string += "%";
			}
			ret.like_string += "_";
			if (cur_sub_index + 1 == num_subs) {
				ret.like_string += "%";
			}
			break;
		case duckdb_re2::kRegexpStar:
			// .* is a Star operator is a anyChar operator as a child.
			// any other child operator would represent a pattern LIKE cannot match.
			if (subs[cur_sub_index]->nsub() == 1 && subs[cur_sub_index]->sub()[0]->op() == duckdb_re2::kRegexpAnyChar) {
				ret.like_string += "%";
				break;
			}
			ret.exists = false;
			return ret;
		case duckdb_re2::kRegexpLiteralString:
		case duckdb_re2::kRegexpLiteral: {
			// if this is the only matching op, we should have directly called
			// GetEscapedLikeString
			D_ASSERT(!(cur_sub_index == 0 && cur_sub_index + 1 == num_subs));
			if (cur_sub_index == 0) {
				ret.like_string += "%";
			}
			// if the kRegexpLiteral or kRegexpLiteralString is the only op to match
			// the string can directly be converted into a contains
			LikeString escaped_like_string = GetLikeStringEscaped(subs[cur_sub_index], false);
			if (!escaped_like_string.exists) {
				return escaped_like_string;
			}
			ret.like_string += escaped_like_string.like_string;
			ret.escaped = escaped_like_string.escaped;
			if (cur_sub_index + 1 == num_subs) {
				ret.like_string += "%";
			}
			break;
		}
		case duckdb_re2::kRegexpBeginText:
			// anchors are represented by the absence of a leading / trailing '%', so they must be at the edges
			if (cur_sub_index != 0) {
				ret.exists = false;
				return ret;
			}
			break;
		case duckdb_re2::kRegexpEndText:
			if (cur_sub_index + 1 != num_subs) {
				ret.exists = false;
				return ret;
			}
			break;
		case duckdb_re2::kRegexpEmptyMatch:
			break;
		default:
			// some other regexp op that doesn't have an equivalent to a like string
			// return false;
			ret.exists = false;
			return ret;
		}
		cur_sub_index += 1;
	}
	return ret;
}

unique_ptr<Expression> RegexOptimizationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                    bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant_expr = bindings[2].get().Cast<BoundConstantExpression>();
	D_ASSERT(root.GetChildrenMutable().size() == 2 || root.GetChildrenMutable().size() == 3);
	for (idx_t i = 0; i < 2; i++) {
		const auto &type = root.GetChildren()[i]->GetReturnType();
		if (type.id() == LogicalTypeId::VARCHAR && !StringType::GetCollation(type).empty()) {
			return nullptr;
		}
	}
	auto regexp_bind_data = root.BindInfo().get()->Cast<RegexpMatchesBindData>();

	Value constant_value;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), constant_expr, constant_value)) {
		return nullptr;
	}
	D_ASSERT(constant_value.type() == constant_expr.GetReturnType());

	duckdb_re2::RE2::Options parsed_options = regexp_bind_data.options;

	if (constant_expr.GetValue().IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(root.GetReturnType()));
	}
	auto patt_str = StringValue::Get(constant_value);

	// the constant_expr is a scalar expression that we have to fold
	if (!constant_expr.IsFoldable()) {
		return nullptr;
	};

	duckdb_re2::RE2 pattern(patt_str, parsed_options);
	if (!pattern.ok()) {
		return nullptr; // this should fail somewhere else
	}

	LikeString like_string;
	// check for a like string. If we can convert it to a like string, the like string
	// optimizer will further optimize suffix and prefix things.
	if (pattern.Regexp()->op() == duckdb_re2::kRegexpLiteralString ||
	    pattern.Regexp()->op() == duckdb_re2::kRegexpLiteral) {
		// convert to contains.
		LikeString escaped_like_string = GetLikeStringEscaped(pattern.Regexp(), true);
		if (!escaped_like_string.exists) {
			return nullptr;
		}

		// if regexp had options, remove them so the new Contains Expression can be matched for other optimizers.
		if (root.GetChildrenMutable().size() == 3) {
			root.GetChildrenMutable().pop_back();
			D_ASSERT(root.GetChildrenMutable().size() == 2);
		}

		auto parameter = make_uniq<BoundConstantExpression>(Value(std::move(escaped_like_string.like_string)));
		auto contains = BindBuiltinScalarFunction(GetContext(), "contains", std::move(root.GetChildrenMutable()));

		contains->GetChildrenMutable()[1] = std::move(parameter);

		return std::move(contains);
	} else if (pattern.Regexp()->op() == duckdb_re2::kRegexpConcat) {
		like_string = LikeMatchFromRegex(pattern);
	} else {
		like_string.exists = false;
	}

	if (!like_string.exists) {
		return nullptr;
	}

	// if regexp had options, remove them so the new Like Expression can be matched for other optimizers.
	if (root.GetChildrenMutable().size() == 3) {
		root.GetChildrenMutable().pop_back();
		D_ASSERT(root.GetChildrenMutable().size() == 2);
	}

	auto like_expression = BindBuiltinScalarFunction(GetContext(), LikeFun::Name, std::move(root.GetChildrenMutable()));

	// Clear the bind info, as the LikeFun bind info is not valid for this new expression.
	like_expression->BindInfoMutable().reset();

	auto parameter = make_uniq<BoundConstantExpression>(Value(std::move(like_string.like_string)));
	like_expression->GetChildrenMutable()[1] = std::move(parameter);
	return std::move(like_expression);
}

RegexpReplaceExtractRule::RegexpReplaceExtractRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function = make_uniq<SpecificFunctionMatcher>("regexp_replace");
	func->policy = SetMatcher::Policy::SOME_ORDERED;
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	func->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	root = std::move(func);
}

static bool RegexpHasWholeTextAnchors(duckdb_re2::Regexp *regexp) {
	if (regexp->op() != duckdb_re2::kRegexpConcat || regexp->nsub() < 2) {
		return false;
	}
	auto subs = regexp->sub();
	return subs[0]->op() == duckdb_re2::kRegexpBeginText &&
	       subs[regexp->nsub() - 1]->op() == duckdb_re2::kRegexpEndText;
}

// Rewrites `regexp_replace(s, P, '\N')` into `regexp_extract(s, P, N, 'k')` when parsed RE2 anchors
// prove that any match spans the whole input. The original pattern is preserved; any regexp_extract
// pattern simplification belongs in a separate optimization.
unique_ptr<Expression> RegexpReplaceExtractRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                       bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	if (!root.BindInfo()) {
		return nullptr;
	}
	auto &bind_data = root.BindInfo()->Cast<RegexpReplaceBindData>();
	if (!bind_data.constant_pattern || bind_data.global_replace) {
		return nullptr;
	}
	// Literal mode treats `^`/`$` as literal chars, so the shape check below would be meaningless.
	if (bind_data.options.literal()) {
		return nullptr;
	}

	const auto &replace_const = bindings[3].get().Cast<BoundConstantExpression>();
	if (replace_const.GetValue().IsNull() || replace_const.GetValue().type().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	const auto &replace_str = StringValue::Get(replace_const.GetValue());
	int32_t group_index;
	// Only a bare backreference can be represented by regexp_extract. Replacement literals or
	// composites such as `x\1`, `\1x`, and `\1\2` must stay on regexp_replace.
	if (replace_str.size() != 2 || replace_str[0] != '\\' || replace_str[1] < '0' || replace_str[1] > '9') {
		return nullptr;
	}
	group_index = replace_str[1] - '0';

	const auto &pattern = bind_data.constant_string;
	duckdb_re2::RE2 compiled(duckdb_re2::StringPiece(pattern.c_str(), pattern.size()), bind_data.options);
	if (!compiled.ok() || !RegexpHasWholeTextAnchors(compiled.Regexp())) {
		return nullptr;
	}
	if (group_index > compiled.NumberOfCapturingGroups()) {
		// leave reporting the invalid back-reference to regexp_replace
		return nullptr;
	}

	string options_str = "k";
	if (root.GetChildrenMutable().size() == 4) {
		auto &options_const = root.GetChildrenMutable()[3]->Cast<BoundConstantExpression>();
		if (options_const.GetValue().IsNull() || options_const.GetValue().type().id() != LogicalTypeId::VARCHAR) {
			return nullptr;
		}
		options_str = StringValue::Get(options_const.GetValue()) + options_str;
	}

	vector<unique_ptr<Expression>> extract_children;
	extract_children.emplace_back(std::move(root.GetChildrenMutable()[0]));
	extract_children.emplace_back(make_uniq<BoundConstantExpression>(Value(pattern)));
	extract_children.emplace_back(make_uniq<BoundConstantExpression>(Value::INTEGER(group_index)));
	extract_children.emplace_back(make_uniq<BoundConstantExpression>(Value(std::move(options_str))));

	FunctionBinder binder(rewriter.context);
	ErrorData error;
	auto extract_expr =
	    binder.BindScalarFunction(Identifier::DefaultSchema(), "regexp_extract", std::move(extract_children), error);
	if (!extract_expr) {
		return nullptr;
	}
	return extract_expr;
}

} // namespace duckdb
