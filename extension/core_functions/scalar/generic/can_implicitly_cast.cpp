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
	auto &context = state.GetContext();
	bool can_cast_implicitly = CanCastImplicitly(context, args.data[0].GetType(), args.data[1].GetType());
	auto v = Value::BOOLEAN(can_cast_implicitly);
	result.Reference(v, count_t(args.size()));
}

unique_ptr<Expression> BindCanCastImplicitlyExpression(FunctionBindExpressionInput &input) {
	auto &source_type = input.children[0]->GetReturnType();
	auto &target_type = input.children[1]->GetReturnType();
	if (source_type.id() == LogicalTypeId::UNKNOWN || source_type.id() == LogicalTypeId::SQLNULL ||
	    target_type.id() == LogicalTypeId::UNKNOWN || target_type.id() == LogicalTypeId::SQLNULL) {
		// parameter - unknown return type
		return nullptr;
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
