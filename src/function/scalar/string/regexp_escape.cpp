#include "duckdb/function/scalar/string_functions.hpp"
#include "re2/re2.h"

namespace duckdb {

namespace {

struct EscapeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto escaped_pattern = RE2::QuoteMeta(input.GetString());
		return heap.AddString(escaped_pattern);
	}
};

void RegexpEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, EscapeOperator>(args.data[0], result);
}

} // namespace

ScalarFunction RegexpEscapeFun::GetFunction() {
	ScalarFunction fun("regexp_escape", {}, LogicalType::VARCHAR, RegexpEscapeFunction);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
