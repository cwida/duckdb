#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/string_characters.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"

#include <ctype.h>
#include <algorithm>

namespace duckdb {

// See: https://www.kdnuggets.com/2020/10/optimizing-levenshtein-distance-measuring-text-similarity.html
// And: Iterative 2-row algorithm: https://en.wikipedia.org/wiki/Levenshtein_distance
// Note: A first implementation using the array algorithm version resulted in an error raised by duckdb
// (too muach memory usage)

template <class T>
static idx_t LevenshteinDistance(ClientContext &context, const T *txt_str, idx_t txt_len, const T *tgt_str,
                                 idx_t tgt_len) {
	// If one string is empty, the distance equals the length of the other string
	if (txt_len == 0) {
		return tgt_len;
	} else if (tgt_len == 0) {
		return txt_len;
	}

	// Create two working vectors
	vector<idx_t> distances0(tgt_len + 1, 0);
	vector<idx_t> distances1(tgt_len + 1, 0);

	idx_t cost_substitution = 0;
	idx_t cost_insertion = 0;
	idx_t cost_deletion = 0;

	// initialize distances0 vector
	// edit distance for an empty txt string is just the number of characters to delete from tgt
	for (idx_t pos_tgt = 0; pos_tgt <= tgt_len; pos_tgt++) {
		distances0[pos_tgt] = pos_tgt;
	}

	for (idx_t pos_txt = 0; pos_txt < txt_len; pos_txt++) {
		context.InterruptCheck();
		// calculate distances1 (current raw distances) from the previous row

		distances1[0] = pos_txt + 1;

		for (idx_t pos_tgt = 0; pos_tgt < tgt_len; pos_tgt++) {
			cost_deletion = distances0[pos_tgt + 1] + 1;
			cost_insertion = distances1[pos_tgt] + 1;
			cost_substitution = distances0[pos_tgt];

			if (txt_str[pos_txt] != tgt_str[pos_tgt]) {
				cost_substitution += 1;
			}

			distances1[pos_tgt + 1] = MinValue(cost_deletion, MinValue(cost_substitution, cost_insertion));
		}
		// copy distances1 (current row) to distances0 (previous row) for next iteration
		// since data in distances1 is always invalidated, a swap without copy is more efficient
		distances0 = distances1;
	}

	return distances0[tgt_len];
}

//! The distance between the characters of the strings - not their bytes
static idx_t LevenshteinDistance(ClientContext &context, const string_t &txt, const string_t &tgt) {
	if (StringIsAscii(txt) && StringIsAscii(tgt)) {
		return LevenshteinDistance(context, txt.GetData(), txt.GetSize(), tgt.GetData(), tgt.GetSize());
	}
	auto txt_chars = GetStringCharacters(txt);
	auto tgt_chars = GetStringCharacters(tgt);
	return LevenshteinDistance(context, txt_chars.data(), txt_chars.size(), tgt_chars.data(), tgt_chars.size());
}

static int64_t LevenshteinScalarFunction(ClientContext &context, const string_t str, string_t tgt) {
	return (int64_t)LevenshteinDistance(context, str, tgt);
}

static void LevenshteinFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vec = args.data[0];
	const auto &tgt_vec = args.data[1];

	auto &context = state.GetContext();
	BinaryExecutor::Execute<string_t, string_t, int64_t>(str_vec, tgt_vec, result, [&](string_t str, string_t tgt) {
		return LevenshteinScalarFunction(context, str, tgt);
	});
}

ScalarFunction LevenshteinFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::BIGINT, LevenshteinFunction);
	fun.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
