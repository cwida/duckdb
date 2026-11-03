#include "duckdb/function/scalar/generic_functions.hpp"

namespace duckdb {

namespace {

void HasErrorFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	throw InternalException("has_error is bound as the HAS_ERROR operator and never executes as a function");
}

//! A plain function call receives its argument already evaluated, so a direct binding cannot capture
//! the errors raised while evaluating it. The expression binder rewrites calls that resolve to this
//! entry into the HAS_ERROR operator before the children are bound; any other binding path is refused.
unique_ptr<FunctionData> HasErrorBind(BindScalarFunctionInput &input) {
	throw BinderException("has_error must be bound as the HAS_ERROR operator over its argument");
}

} // namespace

ScalarFunction HasErrorFun::GetFunction() {
	ScalarFunction fun(Name, {}, LogicalType::BOOLEAN, HasErrorFunction);
	fun.GetSignature().AddParameter("expr", LogicalType::ANY);
	fun.SetBindCallback(HasErrorBind);
	return fun;
}

} // namespace duckdb
