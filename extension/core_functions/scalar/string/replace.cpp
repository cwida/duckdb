#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"

#include <string.h>
#include <ctype.h>
#include <unordered_map>

namespace duckdb {

static idx_t NextNeedle(const char *input_haystack, idx_t size_haystack, const char *input_needle,
                        const idx_t size_needle) {
	// Needle needs something to proceed
	if (size_needle > 0 && size_haystack > 0) {
		auto position = FindStrInStr(const_uchar_ptr_cast(input_haystack), size_haystack,
		                             const_uchar_ptr_cast(input_needle), size_needle);
		if (position != DConstants::INVALID_INDEX) {
			return position;
		}
	}
	// Did not find the needle
	return size_haystack;
}

static string_t ReplaceScalarFunction(const string_t &haystack, const string_t &needle, const string_t &thread,
                                      vector<char> &result) {
	// Get information about the needle, the haystack and the "thread"
	auto input_haystack = haystack.GetData();
	auto size_haystack = haystack.GetSize();

	auto input_needle = needle.GetData();
	auto size_needle = needle.GetSize();

	auto input_thread = thread.GetData();
	auto size_thread = thread.GetSize();

	//  Reuse the buffer
	result.clear();

	for (;;) {
		//  Append the non-matching characters
		auto string_position = NextNeedle(input_haystack, size_haystack, input_needle, size_needle);
		result.insert(result.end(), input_haystack, input_haystack + string_position);
		input_haystack += string_position;
		size_haystack -= string_position;

		//  Stop when we have read the entire haystack
		if (size_haystack == 0) {
			break;
		}

		//  Replace the matching characters
		result.insert(result.end(), input_thread, input_thread + size_thread);
		input_haystack += size_needle;
		size_haystack -= size_needle;
	}

	return string_t(result.data(), UnsafeNumericCast<uint32_t>(result.size()));
}

static void ReplaceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &haystack_vector = args.data[0];
	const auto &needle_vector = args.data[1];
	const auto &thread_vector = args.data[2];

	vector<char> buffer;
	auto &heap = StringVector::GetStringHeap(result);
	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    haystack_vector, needle_vector, thread_vector, result,
	    [&](string_t input_string, string_t needle_string, string_t thread_string) {
		    return heap.AddString(ReplaceScalarFunction(input_string, needle_string, thread_string, buffer));
	    });
}

ScalarFunction ReplaceFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, ReplaceFunction);
	fun.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("source", LogicalType::VARCHAR)
	    .AddParameter("target", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
