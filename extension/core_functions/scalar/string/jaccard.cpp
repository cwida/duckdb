#include "duckdb/common/map.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/string_characters.hpp"
#include "duckdb/common/unordered_set.hpp"

#include <bitset>
#include <ctype.h>

namespace duckdb {

namespace {
constexpr size_t MAX_SIZE = std::numeric_limits<unsigned char>::max() + 1;
}

static inline std::bitset<MAX_SIZE> GetSet(const string_t &str) {
	std::bitset<MAX_SIZE> array_set;

	idx_t str_len = str.GetSize();
	auto s = str.GetData();

	for (idx_t pos = 0; pos < str_len; pos++) {
		array_set.set(static_cast<unsigned char>(s[pos]));
	}
	return array_set;
}

static double JaccardSimilarity(const string_t &str, const string_t &txt) {
	if (str.GetSize() < 1 || txt.GetSize() < 1) {
		throw InvalidInputException("Jaccard Function: An argument too short!");
	}
	idx_t size_intersect;
	idx_t size_union;
	if (StringIsAscii(str) && StringIsAscii(txt)) {
		std::bitset<MAX_SIZE> m_str, m_txt;

		m_str = GetSet(str);
		m_txt = GetSet(txt);

		size_intersect = (m_str & m_txt).count();
		size_union = (m_str | m_txt).count();
	} else {
		// the sets of characters - not of bytes
		auto str_chars = GetStringCharacters(str);
		auto txt_chars = GetStringCharacters(txt);
		unordered_set<int32_t> str_set(str_chars.begin(), str_chars.end());
		unordered_set<int32_t> txt_set(txt_chars.begin(), txt_chars.end());
		size_intersect = 0;
		for (auto &character : str_set) {
			size_intersect += txt_set.count(character);
		}
		size_union = str_set.size() + txt_set.size() - size_intersect;
	}

	return static_cast<double>(size_intersect) / static_cast<double>(size_union);
}

static double JaccardScalarFunction(Vector &result, const string_t str, string_t tgt) {
	return (double)JaccardSimilarity(str, tgt);
}

static void JaccardFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vec = args.data[0];
	const auto &tgt_vec = args.data[1];

	BinaryExecutor::Execute<string_t, string_t, double>(
	    str_vec, tgt_vec, result, [&](string_t str, string_t tgt) { return JaccardScalarFunction(result, str, tgt); });
}

ScalarFunction JaccardFun::GetFunction() {
	ScalarFunction function({}, LogicalType::DOUBLE, JaccardFunction);
	function.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	// throws if one of the strings is empty
	function.SetFallible();
	return function;
}

} // namespace duckdb
