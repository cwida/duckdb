#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"

namespace duckdb {

namespace {

void TryRethrowFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	for (auto entry : args.data[0].Values<bool>()) {
		if (entry.IsValid() && entry.GetValue()) {
			throw InvalidInputException(
			    "An error was raised in a nested subquery under TRY (the original message is not preserved)");
		}
	}
	result.Reference(args.data[1]);
}

unique_ptr<FunctionData> TryRethrowBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	bound_function.SetReturnType(arguments[1]->GetReturnType());
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

} // namespace

ScalarFunction TryRethrowFun::GetFunction() {
	ScalarFunction fun(Name, {}, LogicalType::ANY, TryRethrowFunction);
	fun.GetSignature().AddParameter("error", LogicalType::BOOLEAN);
	fun.GetSignature().AddParameter("value", LogicalType::ANY);
	fun.SetBindCallback(TryRethrowBind);
	// a NULL flag (from an empty subquery) must reach the function unchanged, and the value must not
	// be rewritten around it: the raise on flagged rows is the entire point of the function
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// deterministic in its inputs, so it does not poison surrounding expressions with volatility, but
	// fallible, so nothing evaluates it on rows the plan would not evaluate it on
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
