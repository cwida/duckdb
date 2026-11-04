#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "utf8proc.hpp"

#include <string.h>

namespace duckdb {

template <bool LTRIM, bool RTRIM>
struct TrimOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto data = input.GetData();
		auto size = input.GetSize();

		int32_t codepoint;

		// Find the first character that is not left trimmed
		idx_t begin = 0;
		if (LTRIM) {
			while (begin < size) {
				auto bytes = DecodeCodepoint(data + begin, size - begin, codepoint);
				if (utf8proc_category(codepoint) != UTF8PROC_CATEGORY_ZS) {
					break;
				}
				begin += bytes;
			}
		}

		// Find the last character that is not right trimmed
		idx_t end;
		if (RTRIM) {
			end = begin;
			for (auto next = begin; next < size;) {
				next += DecodeCodepoint(data + next, size - next, codepoint);
				if (utf8proc_category(codepoint) != UTF8PROC_CATEGORY_ZS) {
					end = next;
				}
			}
		} else {
			end = size;
		}

		// Copy the trimmed string
		auto target = heap.EmptyString(end - begin);
		auto output = target.GetDataWriteable();
		memcpy(output, data + begin, end - begin);

		target.Finalize();
		return target;
	}
};

template <bool LTRIM, bool RTRIM>
static void UnaryTrimFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, TrimOperator<LTRIM, RTRIM>>(args.data[0], result);
}

static void GetIgnoredCodepoints(string_t ignored, unordered_set<int32_t> &ignored_codepoints) {
	auto dataptr = ignored.GetData();
	auto size = ignored.GetSize();
	idx_t pos = 0;
	while (pos < size) {
		int32_t codepoint;
		pos += DecodeCodepoint(dataptr + pos, size - pos, codepoint);
		ignored_codepoints.insert(codepoint);
	}
}

template <bool LTRIM, bool RTRIM>
static void BinaryTrimFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    input.data[0], input.data[1], result, [&](string_t input, string_t ignored) {
		    auto data = input.GetData();
		    auto size = input.GetSize();

		    unordered_set<int32_t> ignored_codepoints;
		    GetIgnoredCodepoints(ignored, ignored_codepoints);

		    int32_t codepoint;

		    // Find the first character that is not left trimmed
		    idx_t begin = 0;
		    if (LTRIM) {
			    while (begin < size) {
				    auto bytes = DecodeCodepoint(data + begin, size - begin, codepoint);
				    if (ignored_codepoints.find(codepoint) == ignored_codepoints.end()) {
					    break;
				    }
				    begin += bytes;
			    }
		    }

		    // Find the last character that is not right trimmed
		    idx_t end;
		    if (RTRIM) {
			    end = begin;
			    for (auto next = begin; next < size;) {
				    next += DecodeCodepoint(data + next, size - next, codepoint);
				    if (ignored_codepoints.find(codepoint) == ignored_codepoints.end()) {
					    end = next;
				    }
			    }
		    } else {
			    end = size;
		    }

		    // Copy the trimmed string
		    auto target = StringVector::EmptyString(result, end - begin);
		    auto output = target.GetDataWriteable();
		    memcpy(output, data + begin, end - begin);

		    target.Finalize();
		    return target;
	    });
}

ScalarFunctionSet TrimFun::GetFunctions() {
	ScalarFunctionSet trim;
	ScalarFunction unary({}, LogicalType::VARCHAR, UnaryTrimFunction<true, true>);
	unary.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	trim.AddFunction(unary);

	ScalarFunction binary({}, LogicalType::VARCHAR, BinaryTrimFunction<true, true>);
	binary.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("characters", LogicalType::VARCHAR);
	trim.AddFunction(binary);
	return trim;
}

ScalarFunctionSet LtrimFun::GetFunctions() {
	ScalarFunctionSet ltrim;
	ScalarFunction unary({}, LogicalType::VARCHAR, UnaryTrimFunction<true, false>);
	unary.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	ltrim.AddFunction(unary);

	ScalarFunction binary({}, LogicalType::VARCHAR, BinaryTrimFunction<true, false>);
	binary.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("characters", LogicalType::VARCHAR);
	ltrim.AddFunction(binary);
	return ltrim;
}

ScalarFunctionSet RtrimFun::GetFunctions() {
	ScalarFunctionSet rtrim;
	ScalarFunction unary({}, LogicalType::VARCHAR, UnaryTrimFunction<false, true>);
	unary.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	rtrim.AddFunction(unary);

	ScalarFunction binary({}, LogicalType::VARCHAR, BinaryTrimFunction<false, true>);
	binary.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("characters", LogicalType::VARCHAR);
	rtrim.AddFunction(binary);
	return rtrim;
}

} // namespace duckdb
