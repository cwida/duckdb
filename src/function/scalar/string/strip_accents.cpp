#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/swar.hpp"

#include "utf8proc.hpp"

namespace duckdb {

idx_t FirstNonAscii(const char *input, idx_t n) {
	// Check 8 bytes at a time
	idx_t i = 0;
	for (; i + SwarWord::SIZE <= n; i += SwarWord::SIZE) {
		if (!SwarWord::IsAscii(Load<uint64_t>(const_data_ptr_cast(input + i)))) {
			// non-ascii character in the next 8 bytes
			break;
		}
	}

	// Less than 8 bytes remain, or a non-ascii byte was found
	for (; i < n; i++) {
		if (input[i] & 0x80) {
			return i;
		}
	}
	return n;
}

bool IsAscii(const char *input, idx_t n) {
	return FirstNonAscii(input, n) == n;
}

idx_t DecodeCodepoint(const char *input, idx_t size, int32_t &codepoint) {
	D_ASSERT(size > 0);
	auto bytes = utf8proc_iterate(const_uchar_ptr_cast(input), UnsafeNumericCast<utf8proc_ssize_t>(size), &codepoint);
	if (bytes <= 0) {
		throw InternalException("Invalid UTF-8 encountered in DecodeCodepoint");
	}
	return UnsafeNumericCast<idx_t>(bytes);
}

namespace {

struct StripAccentsOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		if (IsAscii(input.GetData(), input.GetSize())) {
			return input;
		}

		// non-ascii, perform collation
		auto stripped = utf8proc_remove_accents((const utf8proc_uint8_t *)input.GetData(),
		                                        UnsafeNumericCast<utf8proc_ssize_t>(input.GetSize()));
		if (!stripped) {
			// invalid UTF-8
			return input;
		}
		auto result_str = heap.AddString(const_char_ptr_cast(stripped));
		free(stripped);
		return result_str;
	}
};

void StripAccentsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);

	UnaryExecutor::ExecuteString<string_t, string_t, StripAccentsOperator>(args.data[0], result);
	StringVector::AddHeapReference(result, args.data[0]);
}

} // namespace

ScalarFunction StripAccentsFun::GetFunction() {
	ScalarFunction fun("strip_accents", {}, LogicalType::VARCHAR, StripAccentsFunction);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
