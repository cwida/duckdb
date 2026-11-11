#include "duckdb/function/scalar/generic_functions.hpp"

#include <iostream>

namespace duckdb {

namespace {

static void ErrorFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	for (auto entry : args.data[0].Values<string_t>()) {
		if (!entry.IsValid()) {
			FlatVector::SetNull(result, entry.GetIndex(), true);
			continue;
		}
		throw InvalidInputException(entry.GetValue().GetString());
	}
}

} // namespace

bool ErrorFun::IsError(const BoundScalarFunction &function) {
	using error_callback_t = void (*)(DataChunk &, ExpressionState &, Vector &);
	auto &callback = function.GetDefinition()->GetCallbacks().function;
	optional_ptr<const error_callback_t> target = callback.target<error_callback_t>();
	return target && *target == ErrorFunction;
}

ScalarFunction ErrorFun::GetFunction() {
	auto fun = ScalarFunction("error", {}, LogicalType::SQLNULL, ErrorFunction);
	fun.GetSignature().AddParameter("message", LogicalType::VARCHAR);
	// Set the function with side effects to avoid the optimization.
	fun.SetVolatile();
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
