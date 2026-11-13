#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

bool IsAscii(const char *input, idx_t n);
//! Returns the index of the first byte with the high bit set, or n if all bytes are ASCII
idx_t FirstNonAscii(const char *input, idx_t n);
//! Decodes one codepoint and returns its length - throws an InternalException on invalid UTF-8
idx_t DecodeCodepoint(const char *input, idx_t size, int32_t &codepoint);
idx_t LowerLength(const char *input_data, idx_t input_length);
void LowerCase(const char *input_data, idx_t input_length, char *result_data);
idx_t FindStrInStr(const string_t &haystack_s, const string_t &needle_s);
idx_t FindStrInStr(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle, idx_t needle_size);
string_t SubstringASCII(Vector &result, string_t input, int64_t offset, int64_t length);
string_t SubstringUnicode(Vector &result, string_t input, int64_t offset, int64_t length);
string_t SubstringGrapheme(Vector &result, string_t input, int64_t offset, int64_t length);
//! Whether the offset and length are within the range supported by substring - it throws for values outside of it
bool SubstringInSupportedRange(int64_t offset, int64_t length);
unique_ptr<BaseStatistics> PropagateStringSliceStats(FunctionStatisticsInput &input, idx_t start_character_index,
                                                     optional_idx character_count);
//! Common util zonemap pruning for `prefix(s, constant)`.
FilterPropagateResult PrefixFilterPrune(const FunctionStatisticsPruneInput &input);

ScalarFunction GetStringContains();
DUCKDB_API bool Glob(const char *s, idx_t slen, const char *pattern, idx_t plen, bool allow_question_mark = true);

static inline bool IsCharacter(char c) {
	return (c & 0xc0) != 0x80;
}

template <class TA, class TR>
static inline TR Length(TA input) {
	auto input_data = input.GetData();
	auto input_length = input.GetSize();
	// ASCII bytes are one code point each, so only the remainder needs to be counted
	const auto ascii_end = FirstNonAscii(input_data, input_length);
	if (ascii_end == input_length) {
		return UnsafeNumericCast<TR>(input_length);
	}
	TR length = UnsafeNumericCast<TR>(ascii_end);
	for (idx_t i = ascii_end; i < input_length; i++) {
		length += IsCharacter(input_data[i]);
	}
	return length;
}

template <class TA, class TR>
static inline TR GraphemeCount(TA input) {
	auto input_data = input.GetData();
	auto input_length = input.GetSize();
	if (FirstNonAscii(input_data, input_length) == input_length) {
		// ASCII: every byte is a grapheme cluster
		return UnsafeNumericCast<TR>(input_length);
	}
	return UnsafeNumericCast<TR>(Utf8Proc::GraphemeCount(input_data, input_length));
}

} // namespace duckdb
