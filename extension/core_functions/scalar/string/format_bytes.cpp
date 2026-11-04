#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

template <int64_t MULTIPLIER>
static void FormatBytesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<int64_t, string_t>(args.data[0], result, [&](int64_t bytes) {
		bool is_negative = bytes < 0;
		idx_t unsigned_bytes;
		if (bytes < 0) {
			if (bytes == NumericLimits<int64_t>::Minimum()) {
				unsigned_bytes = idx_t(NumericLimits<int64_t>::Maximum()) + 1;
			} else {
				unsigned_bytes = idx_t(-bytes);
			}
		} else {
			unsigned_bytes = idx_t(bytes);
		}
		return heap.AddString((is_negative ? "-" : "") +
		                      StringUtil::BytesToHumanReadableString(unsigned_bytes, MULTIPLIER));
	});
}

ScalarFunction FormatBytesFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, FormatBytesFunction<1024>);
	fun.GetSignature().AddParameter("integer", LogicalType::BIGINT);
	return fun;
}

ScalarFunction FormatreadabledecimalsizeFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, FormatBytesFunction<1000>);
	fun.GetSignature().AddParameter("integer", LogicalType::BIGINT);
	return fun;
}

} // namespace duckdb
