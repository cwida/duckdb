#include "duckdb/optimizer/rule/date_part_simplification.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/optimizer/matcher/type_matcher.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

DatePartSimplificationRule::DatePartSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function = make_uniq<SpecificFunctionMatcher>("date_part");
	auto specifier = make_uniq<ConstantExpressionMatcher>();
	// only the single-part overload is simplified - the struct overload takes a constant LIST of part names
	specifier->type = make_uniq<SpecificTypeMatcher>(LogicalType::VARCHAR);
	func->matchers.push_back(std::move(specifier));
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	root = std::move(func);
}

unique_ptr<Expression> DatePartSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                         bool &changes_made, bool is_root) {
	auto &date_part = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant_expr = bindings[1].get().Cast<BoundConstantExpression>();
	const auto &constant = constant_expr.GetValue();

	if (constant.IsNull()) {
		// NULL specifier: return constant NULL
		return make_uniq<BoundConstantExpression>(Value(date_part.GetReturnType()));
	}
	// otherwise check the specifier
	auto specifier = GetDatePartSpecifier(StringValue::Get(constant));
	Identifier new_function_name;
	switch (specifier) {
	case DatePartSpecifier::YEAR:
		new_function_name = "year";
		break;
	case DatePartSpecifier::MONTH:
		new_function_name = "month";
		break;
	case DatePartSpecifier::DAY:
		new_function_name = "day";
		break;
	case DatePartSpecifier::DECADE:
		new_function_name = "decade";
		break;
	case DatePartSpecifier::CENTURY:
		new_function_name = "century";
		break;
	case DatePartSpecifier::MILLENNIUM:
		new_function_name = "millennium";
		break;
	case DatePartSpecifier::QUARTER:
		new_function_name = "quarter";
		break;
	case DatePartSpecifier::WEEK:
		new_function_name = "week";
		break;
	case DatePartSpecifier::YEARWEEK:
		new_function_name = "yearweek";
		break;
	case DatePartSpecifier::DOW:
		new_function_name = "dayofweek";
		break;
	case DatePartSpecifier::ISODOW:
		new_function_name = "isodow";
		break;
	case DatePartSpecifier::DOY:
		new_function_name = "dayofyear";
		break;
	case DatePartSpecifier::MICROSECONDS:
		new_function_name = "microsecond";
		break;
	case DatePartSpecifier::MILLISECONDS:
		new_function_name = "millisecond";
		break;
	case DatePartSpecifier::SECOND:
		new_function_name = "second";
		break;
	case DatePartSpecifier::MINUTE:
		new_function_name = "minute";
		break;
	case DatePartSpecifier::HOUR:
		new_function_name = "hour";
		break;
	default:
		return nullptr;
	}
	// found a replacement function: bind it
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(date_part.GetChildrenMutable()[1]));

	FunctionBinder binder(rewriter.context);
	return binder.BindScalarFunction(Identifier::DefaultSchema(), new_function_name, std::move(children));
}

} // namespace duckdb
