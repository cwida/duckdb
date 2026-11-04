#include "core_functions/scalar/list_functions.hpp"

#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"

namespace duckdb {

static unique_ptr<FunctionData> ListFilterBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	// the list column and the bound lambda expression
	D_ASSERT(arguments.size() == 2);
	if (arguments[1]->GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
		throw BinderException("Invalid lambda expression!");
	}

	auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();

	// try to cast to boolean, if the return type of the lambda filter expression is not already boolean
	if (bound_lambda_expr.LambdaExpr()->GetReturnType() != LogicalType::BOOLEAN) {
		auto cast_lambda_expr = BoundCastExpression::AddCastToType(
		    context, std::move(bound_lambda_expr.LambdaExprMutable()), LogicalType::BOOLEAN);
		bound_lambda_expr.LambdaExprMutable() = std::move(cast_lambda_expr);
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));

	bound_function.SetReturnType(arguments[0]->GetReturnType());
	auto has_index = bound_lambda_expr.ParameterCount() == 2;
	return LambdaFunctions::ListLambdaBind(context, bound_function, arguments, has_index);
}

static LogicalType ListFilterBindLambda(ClientContext &context, const vector<LogicalType> &function_child_types,
                                        const idx_t parameter_idx,
                                        optional_ptr<BindLambdaContext> bind_lambda_context) {
	return LambdaFunctions::BindBinaryChildren(function_child_types, parameter_idx);
}

ScalarFunction ListFilterFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::LIST(LogicalType::ANY), LambdaFunctions::ListFilterFunction, ListFilterBind,
	                   nullptr, nullptr);
	fun.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("lambda", LogicalType::LAMBDA);

	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(ListLambdaBindData::Serialize);
	fun.SetDeserializeCallback(ListLambdaBindData::Deserialize);
	fun.SetBindLambdaCallback(ListFilterBindLambda);
	// the lambda expression that is executed for every element can throw
	fun.SetFallible();

	return fun;
}

} // namespace duckdb
