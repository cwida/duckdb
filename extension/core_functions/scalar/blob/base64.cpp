#include "core_functions/scalar/blob_functions.hpp"
#include "duckdb/common/types/blob.hpp"

namespace duckdb {

namespace {
struct Base64EncodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto result_str = heap.EmptyString(Blob::ToBase64Size(input));
		Blob::ToBase64(input, result_str.GetDataWriteable());
		result_str.Finalize();
		return result_str;
	}
};

struct Base64DecodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto result_size = Blob::FromBase64Size(input);
		auto result_blob = heap.EmptyString(result_size);
		Blob::FromBase64(input, data_ptr_cast(result_blob.GetDataWriteable()), result_size);
		result_blob.Finalize();
		return result_blob;
	}
};

void Base64EncodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// decode is also a nop cast, but requires verification if the provided string is actually
	UnaryExecutor::ExecuteString<string_t, string_t, Base64EncodeOperator>(args.data[0], result);
}

void Base64DecodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// decode is also a nop cast, but requires verification if the provided string is actually
	UnaryExecutor::ExecuteString<string_t, string_t, Base64DecodeOperator>(args.data[0], result);
}

} // namespace

ScalarFunction ToBase64Fun::GetFunction() {
	ScalarFunction func({}, LogicalType::VARCHAR, Base64EncodeFunction);
	func.GetSignature().AddParameter("blob", LogicalType::BLOB);
	return func;
}

ScalarFunction FromBase64Fun::GetFunction() {
	ScalarFunction function({}, LogicalType::BLOB, Base64DecodeFunction);
	function.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	function.SetFallible();
	return function;
}

} // namespace duckdb
