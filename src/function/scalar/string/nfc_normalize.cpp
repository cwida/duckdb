#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

namespace {

struct NFCNormalizeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();
		if (IsAscii(input_data, input_length)) {
			return input;
		}
		auto normalized_str = Utf8Proc::Normalize(input_data, input_length);
		if (!normalized_str) {
			// invalid UTF-8
			return input;
		}
		auto result_str = heap.AddString(normalized_str);
		free(normalized_str);
		return result_str;
	}
};

void NFCNormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);

	UnaryExecutor::ExecuteString<string_t, string_t, NFCNormalizeOperator>(args.data[0], result);
	StringVector::AddHeapReference(result, args.data[0]);
}

} // namespace

ScalarFunction NFCNormalizeFun::GetFunction() {
	ScalarFunction fun("nfc_normalize", {}, LogicalType::VARCHAR, NFCNormalizeFunction);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
