#include "jaro_winkler.hpp"

#include "core_functions/scalar/string_functions.hpp"
#include "core_functions/scalar/string_characters.hpp"

namespace duckdb {

static inline double JaroScalarFunction(const string_t &s1, const string_t &s2, const double_t &score_cutoff = 0.0) {
	if (!StringIsAscii(s1) || !StringIsAscii(s2)) {
		// compare the characters - not the bytes
		auto s1_chars = GetStringCharacters(s1);
		auto s2_chars = GetStringCharacters(s2);
		return duckdb_jaro_winkler::jaro_similarity(s1_chars.begin(), s1_chars.end(), s2_chars.begin(), s2_chars.end(),
		                                            score_cutoff);
	}
	auto s1_begin = s1.GetData();
	auto s2_begin = s2.GetData();
	return duckdb_jaro_winkler::jaro_similarity(s1_begin, s1_begin + s1.GetSize(), s2_begin, s2_begin + s2.GetSize(),
	                                            score_cutoff);
}

static inline double JaroWinklerScalarFunction(const string_t &s1, const string_t &s2,
                                               const double_t &score_cutoff = 0.0) {
	if (!StringIsAscii(s1) || !StringIsAscii(s2)) {
		// compare the characters - not the bytes
		auto s1_chars = GetStringCharacters(s1);
		auto s2_chars = GetStringCharacters(s2);
		return duckdb_jaro_winkler::jaro_winkler_similarity(s1_chars.begin(), s1_chars.end(), s2_chars.begin(),
		                                                    s2_chars.end(), 0.1, score_cutoff);
	}
	auto s1_begin = s1.GetData();
	auto s2_begin = s2.GetData();
	return duckdb_jaro_winkler::jaro_winkler_similarity(s1_begin, s1_begin + s1.GetSize(), s2_begin,
	                                                    s2_begin + s2.GetSize(), 0.1, score_cutoff);
}

template <class CACHED_SIMILARITY, class SIMILARITY_FUNCTION>
static void CachedFunction(Vector &constant, Vector &other, Vector &result, DataChunk &args, SIMILARITY_FUNCTION fun) {
	auto val = constant.GetValue(0);
	if (val.IsNull()) {
		auto &result_validity = FlatVector::ValidityMutable(result);
		result_validity.SetAllInvalid(other.size());
		return;
	}

	auto str_val = StringValue::Get(val);
	auto cached = CACHED_SIMILARITY(str_val);
	const string_t constant_str(str_val);

	D_ASSERT(args.ColumnCount() == 2 || args.ColumnCount() == 3);
	if (args.ColumnCount() == 2) {
		UnaryExecutor::Execute<string_t, double>(other, result, [&](const string_t &other_str) {
			if (!StringIsAscii(other_str)) {
				return fun(constant_str, other_str, 0.0);
			}
			auto other_str_begin = other_str.GetData();
			return cached.similarity(other_str_begin, other_str_begin + other_str.GetSize());
		});
	} else {
		const auto &score_cutoff = args.data[2];
		BinaryExecutor::Execute<string_t, double_t, double>(
		    other, score_cutoff, result, [&](const string_t &other_str, const double_t score_cutoff) {
			    if (!StringIsAscii(other_str)) {
				    return fun(constant_str, other_str, score_cutoff);
			    }
			    auto other_str_begin = other_str.GetData();
			    return cached.similarity(other_str_begin, other_str_begin + other_str.GetSize(), score_cutoff);
		    });
	}
}

template <class CACHED_SIMILARITY, class SIMILARITY_FUNCTION>
static void TemplatedJaroWinklerFunction(DataChunk &args, Vector &result, SIMILARITY_FUNCTION fun) {
	bool arg0_constant = args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR;
	bool arg1_constant = args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR;
	// the cached similarity compares bytes, which only works for a constant that consists of ASCII characters
	bool can_cache = arg0_constant ^ arg1_constant;
	if (can_cache) {
		auto constant_value = (arg0_constant ? args.data[0] : args.data[1]).GetValue(0);
		if (!constant_value.IsNull()) {
			auto &constant_str = StringValue::Get(constant_value);
			can_cache = StringIsAscii(string_t(constant_str));
		}
	}
	if (!can_cache) {
		// We can't optimize by caching one of the two strings
		D_ASSERT(args.ColumnCount() == 2 || args.ColumnCount() == 3);
		if (args.ColumnCount() == 2) {
			BinaryExecutor::Execute<string_t, string_t, double>(
			    args.data[0], args.data[1], result,
			    [&](const string_t &s1, const string_t &s2) { return fun(s1, s2, 0.0); });
			return;
		} else {
			TernaryExecutor::Execute<string_t, string_t, double_t, double>(args.data[0], args.data[1], args.data[2],
			                                                               result, fun);
			return;
		}
	}

	if (arg0_constant) {
		CachedFunction<CACHED_SIMILARITY>(args.data[0], args.data[1], result, args, fun);
	} else {
		CachedFunction<CACHED_SIMILARITY>(args.data[1], args.data[0], result, args, fun);
	}
}

static void JaroFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	TemplatedJaroWinklerFunction<duckdb_jaro_winkler::CachedJaroSimilarity<char>>(args, result, JaroScalarFunction);
}

static void JaroWinklerFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	TemplatedJaroWinklerFunction<duckdb_jaro_winkler::CachedJaroWinklerSimilarity<char>>(args, result,
	                                                                                     JaroWinklerScalarFunction);
}

ScalarFunctionSet JaroSimilarityFun::GetFunctions() {
	ScalarFunctionSet jaro;

	auto fun = ScalarFunction({}, LogicalType::DOUBLE, JaroFunction);
	fun.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	jaro.AddFunction(fun);

	fun = ScalarFunction({}, LogicalType::DOUBLE, JaroFunction);
	fun.GetSignature()
	    .AddParameter("s1", LogicalType::VARCHAR)
	    .AddParameter("s2", LogicalType::VARCHAR)
	    .AddParameter("score_cutoff", LogicalType::DOUBLE);
	jaro.AddFunction(fun);
	return jaro;
}

ScalarFunctionSet JaroWinklerSimilarityFun::GetFunctions() {
	ScalarFunctionSet jaroWinkler;

	auto fun = ScalarFunction({}, LogicalType::DOUBLE, JaroWinklerFunction);
	fun.GetSignature().AddParameter("s1", LogicalType::VARCHAR).AddParameter("s2", LogicalType::VARCHAR);
	jaroWinkler.AddFunction(fun);

	fun = ScalarFunction({}, LogicalType::DOUBLE, JaroWinklerFunction);
	fun.GetSignature()
	    .AddParameter("s1", LogicalType::VARCHAR)
	    .AddParameter("s2", LogicalType::VARCHAR)
	    .AddParameter("score_cutoff", LogicalType::DOUBLE);
	jaroWinkler.AddFunction(fun);
	return jaroWinkler;
}

} // namespace duckdb
