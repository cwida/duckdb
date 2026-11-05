#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindExpression(BetweenExpression &expr, idx_t depth) {
	// first try to bind the children of the case expression
	ErrorData error;
	auto input = BindChild(expr.InputMutable(), depth, error);
	auto lower = BindChild(expr.LowerBoundMutable(), depth, error);
	auto upper = BindChild(expr.UpperBoundMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}

	auto input_sql_type = ExpressionBinder::GetExpressionReturnType(*input);
	auto lower_sql_type = ExpressionBinder::GetExpressionReturnType(*lower);
	auto upper_sql_type = ExpressionBinder::GetExpressionReturnType(*upper);

	// cast the input types to the same type
	// now obtain the result type of the input types
	LogicalType input_type;
	if (!BoundComparisonExpression::TryBindComparison(context, input_sql_type, lower_sql_type, input_type,
	                                                  expr.GetExpressionType())) {
		throw BinderException(expr,
		                      "Cannot mix values of type %s and %s in BETWEEN clause - an explicit cast is required",
		                      input_sql_type.ToString(), lower_sql_type.ToString());
	}
	if (!BoundComparisonExpression::TryBindComparison(context, input_type, upper_sql_type, input_type,
	                                                  expr.GetExpressionType())) {
		throw BinderException(expr,
		                      "Cannot mix values of type %s and %s in BETWEEN clause - an explicit cast is required",
		                      input_type.ToString(), upper_sql_type.ToString());
	}
	// add casts (if necessary)
	input = BoundCastExpression::AddCastToType(context, std::move(input), input_type);
	lower = BoundCastExpression::AddCastToType(context, std::move(lower), input_type);
	upper = BoundCastExpression::AddCastToType(context, std::move(upper), input_type);
	// handle collation
	PushCollation(context, input, input_type);
	PushCollation(context, lower, input_type);
	PushCollation(context, upper, input_type);

	if (input->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		// the input is a column reference and can be copied cheaply: create two comparisons
		// the reason we do this is that individual comparisons are easier to handle in optimizers
		// if both comparisons remain they will be folded together again into a single BETWEEN in the optimizer
		auto left_compare = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                                                      input->Copy(), std::move(lower));
		auto right_compare = BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                                       std::move(input), std::move(upper));
		return BindResult(make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
		                                                        std::move(left_compare), std::move(right_compare)));
	} else {
		// expression has side effects: we cannot duplicate it
		// create a bound_between directly
		return BindResult(
		    BoundBetweenExpression::Create(std::move(input), std::move(lower), std::move(upper), true, true));
	}
}

} // namespace duckdb
