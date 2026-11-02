#include "duckdb/function/scalar_function.hpp"

#include "core_functions/scalar/string_functions.hpp"

namespace duckdb {
static void ParseFormattedBytesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &arg0 = args.data[0];
	UnaryExecutor::Execute<string_t, idx_t>(arg0, result, [&](string_t str) {
		// Invalid input exceptions thrown from ParseFormattedBytes won't be handled but will be thrown as is
		return StringUtil::ParseFormattedBytes(str.GetString());
	});
}

ScalarFunction ParseFormattedBytesFun::GetFunction() {
	ScalarFunction function({}, LogicalType::UBIGINT, ParseFormattedBytesFunction);
	function.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	// throws if the input is not a valid formatted byte string
	function.SetFallible();
	return function;
}
} // namespace duckdb
