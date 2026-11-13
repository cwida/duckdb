#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

#include "utf8proc_wrapper.hpp"

#include <string.h>

namespace duckdb {

template <bool IS_UPPER>
static string_t ASCIICaseConvert(StringHeap &heap, const char *input_data, idx_t input_length) {
	idx_t output_length = input_length;
	auto result_str = heap.EmptyString(output_length);
	auto result_data = result_str.GetDataWriteable();
	for (idx_t i = 0; i < input_length; i++) {
		result_data[i] = UnsafeNumericCast<char>(IS_UPPER ? StringUtil::ASCII_TO_UPPER_MAP[uint8_t(input_data[i])]
		                                                  : StringUtil::ASCII_TO_LOWER_MAP[uint8_t(input_data[i])]);
	}
	result_str.Finalize();
	return result_str;
}

template <bool IS_UPPER>
static idx_t GetResultLength(const char *input_data, idx_t input_length) {
	idx_t output_length = 0;
	for (idx_t i = 0; i < input_length;) {
		if (!(input_data[i] & 0x80)) {
			// ASCII.
			output_length++;
			i++;
			continue;
		}

		// UTF-8.
		int sz = 0;
		auto codepoint = Utf8Proc::UTF8ToCodepoint(input_data + i, sz, input_length - i);
		auto converted = IS_UPPER ? Utf8Proc::CodepointToUpper(codepoint) : Utf8Proc::CodepointToLower(codepoint);
		auto new_sz = Utf8Proc::CodepointLength(converted);
		output_length += UnsafeNumericCast<idx_t>(new_sz);
		D_ASSERT(sz != 0);
		i += UnsafeNumericCast<idx_t>(sz);
	}
	return output_length;
}

template <bool IS_UPPER>
static void CaseConvert(const char *input_data, idx_t input_length, char *result_data) {
	for (idx_t i = 0; i < input_length;) {
		if (input_data[i] & 0x80) {
			// non-ascii character
			int sz = 0, new_sz = 0;
			auto codepoint = Utf8Proc::UTF8ToCodepoint(input_data + i, sz, input_length - i);
			auto converted_codepoint =
			    IS_UPPER ? Utf8Proc::CodepointToUpper(codepoint) : Utf8Proc::CodepointToLower(codepoint);
			auto success = Utf8Proc::CodepointToUtf8(converted_codepoint, new_sz, result_data);
			D_ASSERT(success);
			(void)success;
			result_data += new_sz;
			i += UnsafeNumericCast<idx_t>(sz);
		} else {
			// ascii
			*result_data = UnsafeNumericCast<char>(IS_UPPER ? StringUtil::ASCII_TO_UPPER_MAP[uint8_t(input_data[i])]
			                                                : StringUtil::ASCII_TO_LOWER_MAP[uint8_t(input_data[i])]);
			result_data++;
			i++;
		}
	}
}

idx_t LowerLength(const char *input_data, idx_t input_length) {
	return GetResultLength<false>(input_data, input_length);
}

void LowerCase(const char *input_data, idx_t input_length, char *result_data) {
	CaseConvert<false>(input_data, input_length, result_data);
}

template <bool IS_UPPER>
static string_t UnicodeCaseConvert(StringHeap &heap, const char *input_data, idx_t input_length) {
	const auto ascii_end = FirstNonAscii(input_data, input_length);
	if (ascii_end == input_length) {
		// ASCII: code points and bytes coincide
		return ASCIICaseConvert<IS_UPPER>(heap, input_data, input_length);
	}
	// first figure out the output length: the ASCII prefix is one byte per code point
	idx_t output_length = ascii_end + GetResultLength<IS_UPPER>(input_data + ascii_end, input_length - ascii_end);
	auto result_str = heap.EmptyString(output_length);
	auto result_data = result_str.GetDataWriteable();

	CaseConvert<IS_UPPER>(input_data, input_length, result_data);
	result_str.Finalize();
	return result_str;
}

namespace {
template <bool IS_UPPER>
struct CaseConvertOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();
		return UnicodeCaseConvert<IS_UPPER>(heap, input_data, input_length);
	}
};
} // namespace

template <bool IS_UPPER>
static void CaseConvertFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, CaseConvertOperator<IS_UPPER>>(args.data[0], result);
}

namespace {
template <bool IS_UPPER>
struct CaseConvertOperatorASCII {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();
		return ASCIICaseConvert<IS_UPPER>(heap, input_data, input_length);
	}
};
} // namespace

template <bool IS_UPPER>
static void CaseConvertFunctionASCII(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, CaseConvertOperatorASCII<IS_UPPER>>(args.data[0], result);
}

template <bool IS_UPPER>
static unique_ptr<BaseStatistics> CaseConvertPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 1);
	// can only propagate stats if the children have stats
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(CaseConvertFunctionASCII<IS_UPPER>);
	}
	// case conversion is not order- or length-preserving, but it never turns a valid string into NULL
	auto result = StringStats::CreateUnknown(expr.GetReturnType());
	result.CopyValidity(child_stats[0]);
	if (!StringStats::HasMinMax(child_stats[0])) {
		return result.ToUnique();
	}
	// All values share the common prefix of min and max; case conversion preserves this property.
	auto min = StringStats::Min(child_stats[0]);
	auto max = StringStats::Max(child_stats[0]);
	const bool is_exact = min == max && StringStats::GetMinType(child_stats[0]) == StringStatsType::EXACT_STATS &&
	                      StringStats::GetMaxType(child_stats[0]) == StringStatsType::EXACT_STATS;
	if (!is_exact) {
		min.resize(StringUtil::GetCommonPrefixSize(min, max));
		// truncated stats can end in the middle of a character - only complete ones can be converted
		size_t invalid_pos = 0;
		if (Utf8Proc::Analyze(min.c_str(), min.size(), nullptr, &invalid_pos) == UnicodeType::INVALID) {
			min.resize(invalid_pos);
		}
		if (min.empty()) {
			return result.ToUnique();
		}
	}
	string converted;
	converted.resize(GetResultLength<IS_UPPER>(min.c_str(), min.size()));
	CaseConvert<IS_UPPER>(min.c_str(), min.size(), &converted[0]);
	auto stats_type = is_exact ? StringStatsType::EXACT_STATS : StringStatsType::TRUNCATED_STATS;
	// case conversion can lengthen a string beyond what the stats can store
	if (converted.size() > StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE) {
		converted.resize(StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
		stats_type = StringStatsType::TRUNCATED_STATS;
	}
	StringStats::SetMin(result, string_t(converted), stats_type);
	StringStats::SetMax(result, string_t(converted), stats_type);
	return result.ToUnique();
}

ScalarFunction LowerFun::GetFunction() {
	ScalarFunction fun("lower", {}, LogicalType::VARCHAR, CaseConvertFunction<false>, nullptr,
	                   CaseConvertPropagateStats<false>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

ScalarFunction UpperFun::GetFunction() {
	ScalarFunction fun("upper", {}, LogicalType::VARCHAR, CaseConvertFunction<true>, nullptr,
	                   CaseConvertPropagateStats<true>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
