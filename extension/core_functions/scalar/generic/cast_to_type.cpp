#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

namespace {

void CastToTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	throw InternalException("CastToType function cannot be executed directly");
}

unique_ptr<Expression> BindCastToTypeFunction(FunctionBindExpressionInput &input) {
	auto &return_type = input.children[1]->GetReturnType();
	if (return_type.id() == LogicalTypeId::UNKNOWN) {
		// parameter - unknown return type
		throw ParameterNotResolvedException();
	}
	if (return_type.id() == LogicalTypeId::SQLNULL) {
		throw InvalidInputException("cast_to_type cannot be used to cast to NULL");
	}
	auto result = BoundCastExpression::AddCastToType(input.context, std::move(input.children[0]), return_type);
	return Expression::PreserveReturnType(return_type, std::move(result));
}

} // namespace
ScalarFunction CastToTypeFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::ANY, CastToTypeFunction);
	fun.GetSignature().AddParameter("param", LogicalType::ANY).AddParameter("type", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindCastToTypeFunction);
	return fun;
}

} // namespace duckdb
