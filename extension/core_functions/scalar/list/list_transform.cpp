#include "core_functions/scalar/list_functions.hpp"

#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"

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
	// the lambda expression that is executed for every element can throw
	fun.SetFallible();

	return fun;
}

} // namespace duckdb
