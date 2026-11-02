#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/string_characters.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

// Using Lowrance-Wagner (LW) algorithm: https://doi.org/10.1145%2F321879.321880
// Can't calculate as trivial modification to levenshtein algorithm
// as we need to potentially know about earlier in the string
template <class T>
static idx_t DamerauLevenshteinDistance(ClientContext &context, const T *source_str, idx_t source_len,
                                        const T *target_str, idx_t target_len) {
	// costs associated with each type of edit, to aid readability
	constexpr uint8_t COST_SUBSTITUTION = 1;
	constexpr uint8_t COST_INSERTION = 1;
	constexpr uint8_t COST_DELETION = 1;
	constexpr uint8_t COST_TRANSPOSITION = 1;

	// If one string is empty, the distance equals the length of the other string
	// either through target_len insertions
	// or source_len deletions
	if (source_len == 0) {
		return target_len * COST_INSERTION;
	} else if (target_len == 0) {
		return source_len * COST_DELETION;
	}

	// larger than the largest possible value:
	const auto inf = source_len * COST_DELETION + target_len * COST_INSERTION + 1;
	// minimum edit distance from prefix of source string to prefix of target string
	// same object as H in LW paper (with indices offset by 1)
	vector<vector<idx_t>> distance(source_len + 2, vector<idx_t>(target_len + 2, inf));
	// keeps track of the largest string indices of source string matching each character
	// same as DA in LW paper
	map<T, idx_t> largest_source_chr_matching;

	// initialise row/column corresponding to zero-length strings
	// partial string -> empty requires a deletion for each character
	for (idx_t source_idx = 0; source_idx <= source_len; source_idx++) {
		distance[source_idx + 1][1] = source_idx * COST_DELETION;
	}
	// and empty -> partial string means simply inserting characters
	for (idx_t target_idx = 1; target_idx <= target_len; target_idx++) {
		distance[1][target_idx + 1] = target_idx * COST_INSERTION;
	}
	// loop through string indices - these are offset by 2 from distance indices
	for (idx_t source_idx = 0; source_idx < source_len; source_idx++) {
		context.InterruptCheck();
		// keeps track of the largest string indices of target string matching current source character
		// same as DB in LW paper
		idx_t largest_target_chr_matching;
		largest_target_chr_matching = 0;
		for (idx_t target_idx = 0; target_idx < target_len; target_idx++) {
			// correspond to i1 and j1 in LW paper respectively
			idx_t largest_source_chr_matching_target;
			idx_t largest_target_chr_matching_source;
			// cost associated to diagnanl shift in distance matrix
			// corresponds to d in LW paper
			uint8_t cost_diagonal_shift;
			largest_source_chr_matching_target = largest_source_chr_matching[target_str[target_idx]];
			largest_target_chr_matching_source = largest_target_chr_matching;
			// if characters match, diagonal move costs nothing and we update our largest target index
			// otherwise move is substitution and costs as such
			if (source_str[source_idx] == target_str[target_idx]) {
				cost_diagonal_shift = 0;
				largest_target_chr_matching = target_idx + 1;
			} else {
				cost_diagonal_shift = COST_SUBSTITUTION;
			}
			distance[source_idx + 2][target_idx + 2] = MinValue(
			    distance[source_idx + 1][target_idx + 1] + cost_diagonal_shift,
			    MinValue(distance[source_idx + 2][target_idx + 1] + COST_INSERTION,
			             MinValue(distance[source_idx + 1][target_idx + 2] + COST_DELETION,
			                      distance[largest_source_chr_matching_target][largest_target_chr_matching_source] +
			                          (source_idx - largest_source_chr_matching_target) * COST_DELETION +
			                          COST_TRANSPOSITION +
			                          (target_idx - largest_target_chr_matching_source) * COST_INSERTION)));
		}
		largest_source_chr_matching[source_str[source_idx]] = source_idx + 1;
	}
	return distance[source_len + 1][target_len + 1];
}

//! The distance between the characters of the strings - not their bytes
static idx_t DamerauLevenshteinDistance(ClientContext &context, const string_t &source, const string_t &target) {
	if (StringIsAscii(source) && StringIsAscii(target)) {
		return DamerauLevenshteinDistance(context, source.GetData(), source.GetSize(), target.GetData(),
		                                  target.GetSize());
	}
	auto source_chars = GetStringCharacters(source);
	auto target_chars = GetStringCharacters(target);
	return DamerauLevenshteinDistance(context, source_chars.data(), source_chars.size(), target_chars.data(),
	                                  target_chars.size());
}

static int64_t DamerauLevenshteinScalarFunction(ClientContext &context, const string_t source, const string_t target) {
	return (int64_t)DamerauLevenshteinDistance(context, source, target);
}

static void DamerauLevenshteinFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &source_vec = args.data[0];
	const auto &target_vec = args.data[1];

	auto &context = state.GetContext();
	BinaryExecutor::Execute<string_t, string_t, int64_t>(
	    source_vec, target_vec, result,
	    [&](string_t source, string_t target) { return DamerauLevenshteinScalarFunction(context, source, target); });
}

ScalarFunction DamerauLevenshteinFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::BIGINT, DamerauLevenshteinFunction);
	fun.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
