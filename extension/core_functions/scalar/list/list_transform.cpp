#include "core_functions/scalar/list_functions.hpp"

#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

namespace duckdb {

static unique_ptr<FunctionData> ListTransformBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	// the list column and the bound lambda expression
	D_ASSERT(arguments.size() == 2);
	if (arguments[1]->GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
		throw BinderException("Invalid lambda expression!");
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));

	auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();
	bound_function.SetReturnType(LogicalType::LIST(bound_lambda_expr.LambdaExpr()->GetReturnType()));
	auto has_index = bound_lambda_expr.ParameterCount() == 2;
	return LambdaFunctions::ListLambdaBind(context, bound_function, arguments, has_index);
}

static LogicalType ListTransformBindLambda(ClientContext &context, const vector<LogicalType> &function_child_types,
                                           const idx_t parameter_idx,
                                           optional_ptr<BindLambdaContext> bind_lambda_context) {
	return LambdaFunctions::BindBinaryChildren(function_child_types, parameter_idx);
}

//! Whether the expression is the element, or a monotone function of it whose bounds come only from those of the element
static bool IsMonotoneInElement(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &children = func.GetChildren();
	if (children.size() != 1 || func.Function().HasStatisticsCallback()) {
		return false;
	}
	auto monotonicity = func.Function().GetArgProperties(0).monotonicity;
	if (!IsMonotonicIncreasing(monotonicity) && !IsMonotonicDecreasing(monotonicity)) {
		return false;
	}
	return IsMonotoneInElement(*children[0]);
}

//! The elements of the result have the statistics of the lambda applied to the elements of the list
static unique_ptr<BaseStatistics> ListTransformStats(ClientContext &context, FunctionStatisticsInput &input) {
	// only a lambda that reads the element alone, without its index or other columns
	if (!input.bind_data || input.child_stats.size() != 2) {
		return nullptr;
	}
	auto &bind_data = input.bind_data->Cast<ListLambdaBindData>();
	auto &list_stats = input.child_stats[0];
	if (!bind_data.lambda_expr || bind_data.has_index || list_stats.GetStatsType() != StatisticsType::LIST_STATS) {
		return nullptr;
	}
	// statistics can let the optimizer skip the call, which must not hide errors or side effects of the list or lambda
	auto &list_expr = *input.expr.GetChildren()[0];
	// only the list of a scan column in a table filter: the statistics of any other list can stem from a producer
	// with side effects that the optimizer removes once the statistics fold its use
	if (list_expr.GetExpressionClass() != ExpressionClass::BOUND_REF) {
		return nullptr;
	}
	if (list_expr.IsVolatile() || list_expr.CanThrow() || bind_data.lambda_expr->IsVolatile() ||
	    bind_data.lambda_expr->CanThrow()) {
		return nullptr;
	}
	// the statistics of the list argument may not show that its producer has side effects or can throw, so derive
	// nothing that does not follow from the statistics of the elements
	if (!IsMonotoneInElement(*bind_data.lambda_expr)) {
		return nullptr;
	}
	auto &element_stats = ListStats::GetChildStats(list_stats);
	auto lambda_stats = ExpressionFilter::TryGetExpressionStatistics(
	    context, *bind_data.lambda_expr, array_ptr<const BaseStatistics>(&element_stats, 1));
	if (!lambda_stats) {
		return nullptr;
	}
	auto result = ListStats::CreateUnknown(input.expr.GetReturnType());
	result.CopyValidity(list_stats);
	ListStats::SetChildStats(result, std::move(lambda_stats));
	return result.ToUnique();
}

ScalarFunction ListTransformFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::LIST(LogicalType::ANY), LambdaFunctions::ListTransformFunction,
	                   ListTransformBind, nullptr, nullptr);
	fun.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("lambda", LogicalType::LAMBDA);

	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(ListLambdaBindData::Serialize);
	fun.SetDeserializeCallback(ListLambdaBindData::Deserialize);
	fun.SetBindLambdaCallback(ListTransformBindLambda);
	fun.SetStatisticsCallback(ListTransformStats);
	// the lambda expression that is executed for every element can throw
	fun.SetFallible();

	return fun;
}

} // namespace duckdb
