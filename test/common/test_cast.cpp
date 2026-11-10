#include "catch.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector.hpp"

#include <cmath>
#include <limits>

using namespace duckdb; // NOLINT

template <class SRC, class DST>
struct ExpectedNumericCast {
	static inline DST Operation(SRC value) {
		return (DST)value;
	}
};

template <class DST>
struct ExpectedNumericCast<double, DST> {
	static inline DST Operation(double value) {
		return (DST)nearbyint(value);
	}
};

template <class DST>
struct ExpectedNumericCast<float, DST> {
	static inline DST Operation(float value) {
		return (DST)nearbyintf(value);
	}
};

template <class SRC, class DST>
static void TestNumericCast(duckdb::vector<SRC> &working_values, duckdb::vector<SRC> &broken_values) {
	DST result;
	for (auto value : working_values) {
		REQUIRE_NOTHROW(Cast::Operation<SRC, DST>(value) == (DST)value);
		REQUIRE(TryCast::Operation<SRC, DST>(value, result));
		REQUIRE(result == ExpectedNumericCast<SRC, DST>::Operation(value));
	}
	for (auto value : broken_values) {
		REQUIRE_THROWS(Cast::Operation<SRC, DST>(value));
		REQUIRE(!TryCast::Operation<SRC, DST>(value, result));
	}
}

template <class DST>
static void TestStringCast(duckdb::vector<string> &working_values, duckdb::vector<DST> &expected_values,
                           duckdb::vector<string> &broken_values) {
	DST result;
	for (idx_t i = 0; i < working_values.size(); i++) {
		auto &value = working_values[i];
		auto expected_value = expected_values[i];
		REQUIRE_NOTHROW(Cast::Operation<string_t, DST>(string_t(value)) == expected_value);
		REQUIRE(TryCast::Operation<string_t, DST>(string_t(value), result));
		REQUIRE(result == expected_value);

		StringUtil::Trim(value);
		duckdb::vector<string> splits;
		splits = StringUtil::Split(value, 'e');
		if (splits.size() > 1 || value[0] == '+') {
			continue;
		}
		splits = StringUtil::Split(value, '.');
		REQUIRE(ConvertToString::Operation<DST>(result) == splits[0]);
	}
	for (auto &value : broken_values) {
		REQUIRE_THROWS(Cast::Operation<string_t, DST>(string_t(value)));
		REQUIRE(!TryCast::Operation<string_t, DST>(string_t(value), result));
	}
}

template <class T>
static void TestExponent() {
	T parse_result;
	string str;
	double value = 1;
	T expected_value = 1;
	for (idx_t exponent = 0; exponent < 100; exponent++) {
		if (value < (double)NumericLimits<T>::Maximum()) {
			// expect success
			str = "1e" + to_string(exponent);
			REQUIRE(TryCast::Operation<string_t, T>(string_t(str), parse_result));
			REQUIRE(parse_result == expected_value);
			str = "-1e" + to_string(exponent);
			REQUIRE(TryCast::Operation<string_t, T>(string_t(str), parse_result));
			REQUIRE(parse_result == -expected_value);
			value *= 10;
			// check again because otherwise this overflows
			if (value < (double)NumericLimits<T>::Maximum()) {
				expected_value *= 10;
			}
		} else {
			// expect failure
			str = "1e" + to_string(exponent);
			REQUIRE(!TryCast::Operation<string_t, T>(string_t(str), parse_result));
			str = "-1e" + to_string(exponent);
			REQUIRE(!TryCast::Operation<string_t, T>(string_t(str), parse_result));
		}
	}
}

TEST_CASE("Test casting to boolean", "[cast]") {
	duckdb::vector<string> working_values = {"true", "false", "TRUE", "FALSE", "T", "F", "1", "0", "False", "True"};
	duckdb::vector<bool> expected_values = {true, false, true, false, true, false, true, false, false, true};
	duckdb::vector<string> broken_values = {"304", "1002", "blabla", "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};

	bool result;
	for (idx_t i = 0; i < working_values.size(); i++) {
		auto &value = working_values[i];
		auto expected_value = expected_values[i];
		REQUIRE_NOTHROW(Cast::Operation<string_t, bool>(value) == expected_value);
		REQUIRE(TryCast::Operation<string_t, bool>(value, result));
		REQUIRE(result == expected_value);
	}
	for (auto &value : broken_values) {
		REQUIRE_THROWS(Cast::Operation<string_t, bool>(value));
		REQUIRE(!TryCast::Operation<string_t, bool>(value, result));
	}
}

TEST_CASE("Test casting to int8_t", "[cast]") {
	// int16_t -> int8_t
	duckdb::vector<int16_t> working_values_int16 = {10, -10, 127, -128};
	duckdb::vector<int16_t> broken_values_int16 = {128, -129, 1000, -1000};
	TestNumericCast<int16_t, int8_t>(working_values_int16, broken_values_int16);
	// int32_t -> int8_t
	duckdb::vector<int32_t> working_values_int32 = {10, -10, 127, -128};
	duckdb::vector<int32_t> broken_values_int32 = {128, -129, 1000000, -1000000};
	TestNumericCast<int32_t, int8_t>(working_values_int32, broken_values_int32);
	// int64_t -> int8_t
	duckdb::vector<int64_t> working_values_int64 = {10, -10, 127, -128};
	duckdb::vector<int64_t> broken_values_int64 = {128, -129, 10000000000LL, -10000000000LL};
	TestNumericCast<int64_t, int8_t>(working_values_int64, broken_values_int64);
	// float -> int8_t
	duckdb::vector<float> working_values_float = {10, -10, 127, -128, 1.3f, -2.7f};
	duckdb::vector<float> broken_values_float = {128, -129, 10000000000.0f, -10000000000.0f, 1e30f, -1e30f};
	TestNumericCast<float, int8_t>(working_values_float, broken_values_float);
	// double -> int8_t
	duckdb::vector<double> working_values_double = {10, -10, 127, -128, 1.3, -2.7};
	duckdb::vector<double> broken_values_double = {128, -129, 10000000000.0, -10000000000.0, 1e100, -1e100};
	TestNumericCast<double, int8_t>(working_values_double, broken_values_double);
	// string -> int8_t
	duckdb::vector<string> working_values_str = {"10",  "+10", "-10",   "127", "-128", "1.3",   "1e2",
	                                             "2e1", "2e0", "20e-1", "1.",  "  3",  " 3   ", "\t3 \t \n"};
	duckdb::vector<int8_t> expected_values_str = {10, 10, -10, 127, -128, 1, 100, 20, 2, 2, 1, 3, 3, 3};
	duckdb::vector<string> broken_values_str = {"128",
	                                            "-129",
	                                            "10000000000000000000000000000000000000000000000000000000000000",
	                                            "aaaa",
	                                            "19A",
	                                            "",
	                                            "1e3",
	                                            "1e",
	                                            "1e-",
	                                            "1e100",
	                                            "1e100000000",
	                                            "10000e-1",
	                                            " 3 2",
	                                            "+"};
	TestStringCast<int8_t>(working_values_str, expected_values_str, broken_values_str);
	TestExponent<int8_t>();
}

TEST_CASE("Test casting to int16_t", "[cast]") {
	// int32_t -> int16_t
	duckdb::vector<int32_t> working_values_int32 = {10, -10, 127, -127, 32767, -32768};
	duckdb::vector<int32_t> broken_values_int32 = {32768, -32769, 1000000, -1000000};
	TestNumericCast<int32_t, int16_t>(working_values_int32, broken_values_int32);
	// int64_t -> int16_t
	duckdb::vector<int64_t> working_values_int64 = {10, -10, 127, -127, 32767, -32768};
	duckdb::vector<int64_t> broken_values_int64 = {32768, -32769, 10000000000LL, -10000000000LL};
	TestNumericCast<int64_t, int16_t>(working_values_int64, broken_values_int64);
	// float -> int16_t
	duckdb::vector<float> working_values_float = {10.0f, -10.0f, 32767.0f, -32768.0f, 1.3f, -2.7f};
	duckdb::vector<float> broken_values_float = {32768.0f, -32769.0f, 10000000000.0f, -10000000000.0f, 1e30f, -1e30f};
	TestNumericCast<float, int16_t>(working_values_float, broken_values_float);
	// double -> int16_t
	duckdb::vector<double> working_values_double = {10, -10, 32767, -32768, 1.3, -2.7};
	duckdb::vector<double> broken_values_double = {32768, -32769, 10000000000.0, -10000000000.0, 1e100, -1e100};
	TestNumericCast<double, int16_t>(working_values_double, broken_values_double);
	// string -> int16_t
	duckdb::vector<string> working_values_str = {"10",  "-10",   "32767", "-32768", "1.3",
	                                             "3e4", "250e2", "3e+4",  "3e0",    "30e-1"};
	duckdb::vector<int16_t> expected_values_str = {10, -10, 32767, -32768, 1, 30000, 25000, 30000, 3, 3};
	duckdb::vector<string> broken_values_str = {
	    "32768", "-32769",      "10000000000000000000000000000000000000000000000000000000000000",
	    "aaaa",  "19A",         "",
	    "1.A",   "1e",          "1e-",
	    "1e100", "1e100000000", "+"};
	TestStringCast<int16_t>(working_values_str, expected_values_str, broken_values_str);
	TestExponent<int16_t>();
}

TEST_CASE("Test casting to int32_t", "[cast]") {
	// int64_t -> int32_t
	duckdb::vector<int64_t> working_values_int64 = {10, -10, 127, -127, 32767, -32768, 2147483647LL, -2147483648LL};
	duckdb::vector<int64_t> broken_values_int64 = {2147483648LL, -2147483649LL, 10000000000LL, -10000000000LL};
	TestNumericCast<int64_t, int32_t>(working_values_int64, broken_values_int64);
	// float -> int32_t
	duckdb::vector<float> working_values_float = {10.0f, -10.0f, 2000000000.0f, -2000000000.0f, 1.3f, -2.7f};
	duckdb::vector<float> broken_values_float = {3000000000.0f,   -3000000000.0f, 10000000000.0f,
	                                             -10000000000.0f, 1e30f,          -1e30f};
	TestNumericCast<float, int32_t>(working_values_float, broken_values_float);
	// double -> int32_t
	duckdb::vector<double> working_values_double = {10, -10, 32767.0, -32768.0, 1.3, -2.7, 2147483647.0, -2147483648.0};
	duckdb::vector<double> broken_values_double = {2147483648.0,   -2147483649.0, 10000000000.0,
	                                               -10000000000.0, 1e100,         -1e100};
	TestNumericCast<double, int32_t>(working_values_double, broken_values_double);
	// string -> int32_t
	duckdb::vector<string> working_values_str = {"10", "-10", "2147483647", "-2147483647", "1.3", "-1.3", "1e6"};
	duckdb::vector<int32_t> expected_values_str = {10, -10, 2147483647, -2147483647, 1, -1, 1000000};
	duckdb::vector<string> broken_values_str = {
	    "2147483648", "-2147483649", "10000000000000000000000000000000000000000000000000000000000000",
	    "aaaa",       "19A",         "",
	    "1.A",        "1e1e1e1"};
	TestStringCast<int32_t>(working_values_str, expected_values_str, broken_values_str);
	TestExponent<int32_t>();
}

TEST_CASE("Test casting to int64_t", "[cast]") {
	// float -> int64_t
	duckdb::vector<float> working_values_float = {10.0f,
	                                              -10.0f,
	                                              32767.0f,
	                                              -32768.0f,
	                                              1.3f,
	                                              -2.7f,
	                                              2000000000.0f,
	                                              -2000000000.0f,
	                                              4000000000000000000.0f,
	                                              -4000000000000000000.0f};
	duckdb::vector<float> broken_values_float = {20000000000000000000.0f, -20000000000000000000.0f, 1e30f, -1e30f};
	TestNumericCast<float, int64_t>(working_values_float, broken_values_float);
	// double -> int64_t
	duckdb::vector<double> working_values_double = {
	    10, -10, 32767, -32768, 1.3, -2.7, 2147483647, -2147483648.0, 4611686018427387904.0, -4611686018427387904.0};
	duckdb::vector<double> broken_values_double = {18446744073709551616.0, -18446744073709551617.0, 1e100, -1e100};
	TestNumericCast<double, int64_t>(working_values_double, broken_values_double);
	// string -> int64_t
	duckdb::vector<string> working_values_str = {
	    "10",    "-10", "9223372036854775807", "-9223372036854775807", "1.3", "-9223372036854775807.1293813", "1e18",
	    "1e+18", "1."};
	duckdb::vector<int64_t> expected_values_str = {10,
	                                               -10,
	                                               9223372036854775807LL,
	                                               -9223372036854775807LL,
	                                               1,
	                                               -9223372036854775807LL,
	                                               1000000000000000000LL,
	                                               1000000000000000000LL,
	                                               1};
	duckdb::vector<string> broken_values_str = {"9223372036854775808",
	                                            "-9223372036854775809",
	                                            "10000000000000000000000000000000000000000000000000000000000000",
	                                            "aaaa",
	                                            "19A",
	                                            "",
	                                            "1.A",
	                                            "1.2382398723A",
	                                            "1e++1",
	                                            "1e+1+1",
	                                            "1e+1-1",
	                                            "+"};
	TestStringCast<int64_t>(working_values_str, expected_values_str, broken_values_str);
	TestExponent<int64_t>();
}

template <class DST>
static void TestStringCastDouble(duckdb::vector<string> &working_values, duckdb::vector<DST> &expected_values,
                                 duckdb::vector<string> &broken_values) {
	DST result;
	for (idx_t i = 0; i < working_values.size(); i++) {
		auto &value = working_values[i];
		auto expected_value = expected_values[i];
		REQUIRE_NOTHROW(Cast::Operation<string_t, DST>(string_t(value)) == expected_value);
		REQUIRE(TryCast::Operation<string_t, DST>(string_t(value), result));
		REQUIRE(ApproxEqual(result, expected_value));

		auto to_str_and_back =
		    Cast::Operation<string_t, DST>(string_t(ConvertToString::Operation<DST>(expected_value)));
		REQUIRE(ApproxEqual(to_str_and_back, expected_value));
	}
	for (auto &value : broken_values) {
		REQUIRE_THROWS(Cast::Operation<string_t, DST>(string_t(value)));
		REQUIRE(!TryCast::Operation<string_t, DST>(string_t(value), result));
	}
}

TEST_CASE("Test casting to float", "[cast]") {
	// string -> float
	duckdb::vector<string> working_values = {
	    "1.3",         "1.34514", "1e10", "1e-2", "-1e-1", "1.1781237378938173987123987123981723981723981723987123",
	    "1.123456789", "1."};
	duckdb::vector<float> expected_values = {
	    1.3f,         1.34514f, 1e10f, 1e-2f, -1e-1f, 1.1781237378938173987123987123981723981723981723987123f,
	    1.123456789f, 1.0f};
	duckdb::vector<string> broken_values = {
	    "-",     "",        "aaa",
	    "12aaa", "1e10e10", "1e",
	    "1e-",   "1e10a",   "1.1781237378938173987123987123981723981723981723934834583490587123w",
	    "1.2.3"};
	TestStringCastDouble<float>(working_values, expected_values, broken_values);
}

TEST_CASE("Test casting to double", "[cast]") {
	// string -> double
	duckdb::vector<string> working_values = {"1.3",
	                                         "+1.3",
	                                         "1.34514",
	                                         "1e10",
	                                         "1e-2",
	                                         "-1e-1",
	                                         "1.1781237378938173987123987123981723981723981723987123",
	                                         "1.123456789",
	                                         "1.",
	                                         "-1.2",
	                                         "-1.2e1",
	                                         " 1.2 ",
	                                         "  1.2e2  ",
	                                         " \t 1.2e2 \t"};
	duckdb::vector<double> expected_values = {
	    1.3,         1.3, 1.34514, 1e10, 1e-2, -1e-1, 1.1781237378938173987123987123981723981723981723987123,
	    1.123456789, 1.0, -1.2,    -12,  1.2,  120,   120};
	duckdb::vector<string> broken_values = {
	    "-",     "",        "aaa",
	    "12aaa", "1e10e10", "1e",
	    "1e-",   "1e10a",   "1.1781237378938173987123987123981723981723981723934834583490587123w",
	    "1.2.3", "1.222.",  "1..",
	    "1 . 2", "1. 2",    "1.2 e20",
	    "+"};
	TestStringCastDouble<double>(working_values, expected_values, broken_values);
}

TEST_CASE("Test converting long double to HUGEINT and UHUGEINT", "[cast]") {
	const long double two_to_the_127 = std::ldexp(1.0L, 127);
	hugeint_t hugeint_result;
	REQUIRE(Hugeint::TryConvert(-two_to_the_127, hugeint_result));
	REQUIRE(hugeint_result == NumericLimits<hugeint_t>::Minimum());
	REQUIRE(!Hugeint::TryConvert(two_to_the_127, hugeint_result));
	REQUIRE(!Hugeint::TryConvert(-two_to_the_127 * 2, hugeint_result));

	uhugeint_t uhugeint_result;
	REQUIRE(Uhugeint::TryConvert(two_to_the_127, uhugeint_result));
	REQUIRE(uhugeint_result == uhugeint_t(uint64_t(1) << 63, 0));
	REQUIRE(!Uhugeint::TryConvert(two_to_the_127 * 2, uhugeint_result));

	if (std::numeric_limits<long double>::digits < 64) {
		return;
	}
	// Exact only in a long double with at least 64 mantissa bits, and lost by a conversion through double.
	const long double uint64_maximum = static_cast<long double>(NumericLimits<uint64_t>::Maximum());
	const long double above_uint64 = std::ldexp(1.0L, 66) + 8;
	REQUIRE(Hugeint::TryConvert(uint64_maximum, hugeint_result));
	REQUIRE(hugeint_result == hugeint_t(0, NumericLimits<uint64_t>::Maximum()));
	REQUIRE(Hugeint::TryConvert(-uint64_maximum, hugeint_result));
	REQUIRE(hugeint_result == -hugeint_t(0, NumericLimits<uint64_t>::Maximum()));
	REQUIRE(Hugeint::TryConvert(above_uint64, hugeint_result));
	REQUIRE(hugeint_result == hugeint_t(4, 8));
	REQUIRE(Hugeint::TryConvert(-above_uint64, hugeint_result));
	REQUIRE(hugeint_result == -hugeint_t(4, 8));
	REQUIRE(Uhugeint::TryConvert(uint64_maximum, uhugeint_result));
	REQUIRE(uhugeint_result == uhugeint_t(0, NumericLimits<uint64_t>::Maximum()));
	REQUIRE(Uhugeint::TryConvert(above_uint64, uhugeint_result));
	REQUIRE(uhugeint_result == uhugeint_t(4, 8));
}
