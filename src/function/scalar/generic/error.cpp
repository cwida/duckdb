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

ScalarFunction ErrorFun::GetFunction() {
	auto fun = ScalarFunction("error", {}, LogicalType::SQLNULL, ErrorFunction);
	fun.GetSignature().AddParameter("message", LogicalType::VARCHAR);
	// Set the function with side effects to avoid the optimization.
	fun.SetVolatile();
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
