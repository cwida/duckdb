#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/string_characters.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include <ctype.h>
#include <algorithm>

namespace duckdb {

template <class T>
static int64_t Mismatches(const T *str_str, idx_t str_len, const T *tgt_str, idx_t tgt_len) {
	if (str_len != tgt_len) {
		throw InvalidInputException("Mismatch Function: Strings must be of equal length!");
	}
	if (str_len < 1) {
		throw InvalidInputException("Mismatch Function: Strings must be of length > 0!");
	}

	idx_t mismatches = 0;

	for (idx_t idx = 0; idx < str_len; ++idx) {
		if (str_str[idx] != tgt_str[idx]) {
			mismatches++;
		}
	}
	return (int64_t)mismatches;
}

//! The number of positions at which the characters of the strings differ - not their bytes
static int64_t MismatchesScalarFunction(Vector &result, const string_t str, string_t tgt) {
	if (StringIsAscii(str) && StringIsAscii(tgt)) {
		return Mismatches(str.GetData(), str.GetSize(), tgt.GetData(), tgt.GetSize());
	}
	auto str_chars = GetStringCharacters(str);
	auto tgt_chars = GetStringCharacters(tgt);
	return Mismatches(str_chars.data(), str_chars.size(), tgt_chars.data(), tgt_chars.size());
}

static void MismatchesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vec = args.data[0];
	const auto &tgt_vec = args.data[1];

	BinaryExecutor::Execute<string_t, string_t, int64_t>(str_vec, tgt_vec, result, [&](string_t str, string_t tgt) {
		return MismatchesScalarFunction(result, str, tgt);
	});
}

ScalarFunction HammingFun::GetFunction() {
	ScalarFunction function({}, LogicalType::BIGINT, MismatchesFunction);
	function.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	// throws if the strings are empty or of unequal length
	function.SetFallible();
	return function;
}

} // namespace duckdb
