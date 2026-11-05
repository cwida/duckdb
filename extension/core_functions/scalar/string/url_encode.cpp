#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct URLEncodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_str = input.GetData();
		auto input_size = input.GetSize();
		idx_t result_length = StringUtil::URLEncodeSize(input_str, input_size);
		auto result_str = heap.EmptyString(result_length);
		StringUtil::URLEncodeBuffer(input_str, input_size, result_str.GetDataWriteable());
		result_str.Finalize();
		return result_str;
	}
};

static void URLEncodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, URLEncodeOperator>(args.data[0], result);
}

ScalarFunction UrlEncodeFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, URLEncodeFunction);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

struct URLDecodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_str = input.GetData();
		auto input_size = input.GetSize();
		idx_t result_length = StringUtil::URLDecodeSize(input_str, input_size);
		auto result_str = heap.EmptyString(result_length);
		StringUtil::URLDecodeBuffer(input_str, input_size, result_str.GetDataWriteable());
		result_str.Finalize();
		return result_str;
	}
};

static void URLDecodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, URLDecodeOperator>(args.data[0], result);
}

ScalarFunction UrlDecodeFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, URLDecodeFunction);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	// throws if the decoded value is not valid UTF8
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
