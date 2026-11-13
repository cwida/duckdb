#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/pair.hpp"

#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

static pair<idx_t, idx_t> PadCountChars(const idx_t len, const char *data, const idx_t size) {
	//  Count how much of str will fit in the output
	idx_t nbytes = 0;
	idx_t nchars = 0;
	for (; nchars < len && nbytes < size; ++nchars) {
		int32_t codepoint;
		nbytes += DecodeCodepoint(data + nbytes, size - nbytes, codepoint);
	}

	return pair<idx_t, idx_t>(nbytes, nchars);
}

//! The padding a call writes: whole copies of the pad, then the bytes of its first remainder characters. Decided from
//! the pad's geometry, counted once per call, so that the output is copied rather than decoded character by character.
struct PadPlan {
	idx_t pad_bytes = 0;
	idx_t pad_chars = 0;
	idx_t copies = 0;
	idx_t remainder_bytes = 0;

	idx_t Bytes() const {
		return copies * pad_bytes + remainder_bytes;
	}
};

static PadPlan PlanPadding(const idx_t len, const string_t &pad) {
	PadPlan plan;
	plan.pad_bytes = pad.GetSize();
	plan.pad_chars = PadCountChars(NumericLimits<idx_t>::Maximum(), pad.GetData(), plan.pad_bytes).second;
	if (len == 0 || plan.pad_chars == 0) {
		return plan;
	}
	plan.copies = len / plan.pad_chars;
	plan.remainder_bytes = PadCountChars(len % plan.pad_chars, pad.GetData(), plan.pad_bytes).first;
	return plan;
}

static void CheckPadResultSize(const idx_t written_bytes, const PadPlan &plan, const char *name) {
	auto result_size = written_bytes + plan.Bytes();
	if (result_size > string_t::MAX_STRING_SIZE) {
		throw OutOfRangeException(
		    "Cannot create a string of size: '%d' in %s, the maximum supported string size is: '%d'", result_size, name,
		    string_t::MAX_STRING_SIZE);
	}
}

static void InsertPadding(const string_t &pad, const PadPlan &plan, vector<char> &result) {
	auto data = pad.GetData();
	if (plan.pad_bytes == 1) {
		result.insert(result.end(), plan.copies, data[0]);
		return;
	}
	for (idx_t i = 0; i < plan.copies; i++) {
		result.insert(result.end(), data, data + plan.pad_bytes);
	}
	result.insert(result.end(), data, data + plan.remainder_bytes);
}

template <bool LEFT>
static string_t PadString(const string_t &str, const int32_t len, const string_t &pad, vector<char> &result,
                          const char *name) {
	//  Reuse the buffer
	result.clear();

	// Get information about the base string
	auto data_str = str.GetData();
	auto size_str = str.GetSize();

	//  Count how much of str will fit in the output, and pad by the number of characters still needed
	auto written = PadCountChars(UnsafeNumericCast<idx_t>(len), data_str, size_str);
	auto padding_chars = UnsafeNumericCast<idx_t>(len) - written.second;
	auto plan = PlanPadding(padding_chars, pad);
	CheckPadResultSize(written.first, plan, name);
	if (padding_chars > 0 && plan.pad_chars == 0) {
		throw InvalidInputException("Insufficient padding in %s.", name);
	}
	result.reserve(written.first + plan.Bytes());

	if (LEFT) {
		InsertPadding(pad, plan, result);
		result.insert(result.end(), data_str, data_str + written.first);
	} else {
		result.insert(result.end(), data_str, data_str + written.first);
		InsertPadding(pad, plan, result);
	}
	return string_t(result.data(), UnsafeNumericCast<uint32_t>(result.size()));
}

struct LeftPadOperator {
	static inline string_t Operation(const string_t &str, const int32_t len, const string_t &pad,
	                                 vector<char> &result) {
		return PadString<true>(str, len, pad, result, "LPAD");
	}
};

struct RightPadOperator {
	static inline string_t Operation(const string_t &str, const int32_t len, const string_t &pad,
	                                 vector<char> &result) {
		return PadString<false>(str, len, pad, result, "RPAD");
	}
};

template <class OP>
static void PadFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vector = args.data[0];
	const auto &len_vector = args.data[1];
	const auto &pad_vector = args.data[2];

	vector<char> buffer;
	auto &heap = StringVector::GetStringHeap(result);
	TernaryExecutor::Execute<string_t, int32_t, string_t, string_t>(
	    str_vector, len_vector, pad_vector, result, [&](string_t str, int32_t len, string_t pad) {
		    len = MaxValue<int32_t>(len, 0);
		    return heap.AddString(OP::Operation(str, len, pad, buffer));
	    });
}

ScalarFunction LpadFun::GetFunction() {
	ScalarFunction func({}, LogicalType::VARCHAR, PadFunction<LeftPadOperator>);
	func.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("count", LogicalType::INTEGER)
	    .AddParameter("character", LogicalType::VARCHAR);
	func.SetFallible();
	return func;
}

ScalarFunction RpadFun::GetFunction() {
	ScalarFunction func({}, LogicalType::VARCHAR, PadFunction<RightPadOperator>);
	func.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("count", LogicalType::INTEGER)
	    .AddParameter("character", LogicalType::VARCHAR);
	func.SetFallible();
	return func;
}

} // namespace duckdb
