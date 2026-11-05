#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast_rules.hpp"

namespace duckdb {

namespace {

bool CanCastImplicitly(ClientContext &context, const LogicalType &source, const LogicalType &target) {
	return CastFunctionSet::ImplicitCastCost(context, source, target) >= 0;
}

void CanCastImplicitlyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	throw InternalException("can_cast_implicitly cannot be executed directly");
}

unique_ptr<Expression> BindCanCastImplicitlyExpression(FunctionBindExpressionInput &input) {
	auto &source_type = input.children[0]->GetReturnType();
	auto &target_type = input.children[1]->GetReturnType();
	if (source_type.id() == LogicalTypeId::UNKNOWN || target_type.id() == LogicalTypeId::UNKNOWN) {
		// parameter - unknown return type
		throw ParameterNotResolvedException();
	}
	// emit a constant expression
	return make_uniq<BoundConstantExpression>(
	    Value::BOOLEAN(CanCastImplicitly(input.context, source_type, target_type)));
}

} // namespace

ScalarFunction CanCastImplicitlyFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::BOOLEAN, CanCastImplicitlyFunction);
	fun.GetSignature().AddParameter("source_type", LogicalType::ANY).AddParameter("target_type", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindCanCastImplicitlyExpression);
	return fun;
}

} // namespace duckdb
