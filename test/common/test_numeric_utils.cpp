#include "catch.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "test_helpers.hpp"

#include <cfenv>
#include <cmath>
#include <limits>

using namespace duckdb;

namespace {

//! Restores the floating-point rounding mode on scope exit, also when an assertion fails
struct RoundingModeGuard {
	RoundingModeGuard() : mode(std::fegetround()) {
	}
	~RoundingModeGuard() {
		std::fesetround(mode);
	}
	int mode;
};

template <class T>
bool IsNegativeZero(T value) {
	return value == 0 && std::signbit(value);
}

} // namespace

TEST_CASE("Numeric cast checks", "[numeric_utils]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return;
#endif
	// unsigned-unsiged
	// can not fail upcasting unsigned type
	REQUIRE_NOTHROW(NumericCast<uint16_t, uint8_t>(NumericLimits<uint8_t>::Maximum()));
	REQUIRE_NOTHROW(NumericCast<uint16_t, uint8_t>(NumericLimits<uint8_t>::Minimum()));

	// we can down cast if value fits
	REQUIRE_NOTHROW(NumericCast<uint8_t, uint16_t>(NumericLimits<uint8_t>::Maximum()));

	// but not if it doesn't
	REQUIRE_THROWS(NumericCast<uint8_t, uint16_t>(NumericLimits<uint8_t>::Maximum() + 1));

	// signed-signed, same as above
	REQUIRE_NOTHROW(NumericCast<int16_t, int8_t>(NumericLimits<int8_t>::Maximum()));
	REQUIRE_NOTHROW(NumericCast<int16_t, int8_t>(NumericLimits<int8_t>::Minimum()));
	REQUIRE_NOTHROW(NumericCast<int8_t, int16_t>(NumericLimits<int8_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<int8_t, int16_t>(NumericLimits<int8_t>::Maximum() + 1));
	REQUIRE_THROWS(NumericCast<int8_t, int16_t>(NumericLimits<int8_t>::Minimum() - 1));

	// unsigned to signed
	REQUIRE_NOTHROW(NumericCast<int8_t, uint8_t>(NumericLimits<int8_t>::Maximum()));
	REQUIRE_NOTHROW(NumericCast<int8_t, uint8_t>(NumericLimits<uint8_t>::Minimum()));

	// uint8 max will not fit in int8
	REQUIRE_THROWS(NumericCast<int8_t, uint8_t>(NumericLimits<uint8_t>::Maximum()));

	// signed to unsigned
	// can cast int8 max to uint8
	REQUIRE_NOTHROW(NumericCast<uint8_t, int8_t>(NumericLimits<int8_t>::Maximum()));
	// cat cast int8 min to unit8
	REQUIRE_THROWS(NumericCast<uint8_t, int8_t>(NumericLimits<int8_t>::Minimum()));

	// can't cast anything negative to anything unsigned
	REQUIRE_THROWS(NumericCast<uint64_t, int8_t>(-1));
	REQUIRE_THROWS(NumericCast<uint64_t, int16_t>(-1));
	REQUIRE_THROWS(NumericCast<uint64_t, int32_t>(-1));
	REQUIRE_THROWS(NumericCast<uint64_t, int64_t>(-1));

	// can't downcast big number
	REQUIRE_THROWS(NumericCast<int64_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<int32_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<uint32_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<int16_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<uint16_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<int8_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));
	REQUIRE_THROWS(NumericCast<uint8_t, uint64_t>(NumericLimits<uint64_t>::Maximum()));

	// TODO this should throw but doesn't
	//	REQUIRE_THROWS(NumericCast<uint8_t, hugeint_t>(hugeint_t(-1)));
}

TEST_CASE("RoundToNearestEven rounds halfway cases to the even neighbour regardless of rounding mode",
          "[numeric_utils]") {
	struct RoundingCase {
		double input;
		double expected;
	};
	// every input is exact in binary, so the float and the double case are the same value
	const RoundingCase cases[] = {{0.0, 0.0},  {1.0, 1.0},  {0.5, 0.0},   {1.5, 2.0},    {2.5, 2.0},   {3.5, 4.0},
	                              {4.5, 4.0},  {-0.5, 0.0}, {-1.5, -2.0}, {-2.5, -2.0},  {-3.5, -4.0}, {2.25, 2.0},
	                              {2.75, 3.0}, {3.25, 3.0}, {3.75, 4.0},  {-2.25, -2.0}, {-2.75, -3.0}};

	RoundingModeGuard guard;
	const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
	for (const auto mode : modes) {
		if (std::fesetround(mode) != 0) {
			// mode not supported on this platform
			continue;
		}
		REQUIRE(std::fegetround() == mode);
		for (const auto &test_case : cases) {
			// volatile stops the compiler from rounding these at compile time instead
			volatile double input = test_case.input;
			volatile float input_float = static_cast<float>(test_case.input);
			REQUIRE(RoundToNearestEven(static_cast<double>(input)) == test_case.expected);
			REQUIRE(RoundToNearestEven(static_cast<float>(input_float)) == static_cast<float>(test_case.expected));
		}
	}
}

TEST_CASE("RoundToNearestEven preserves the sign of zero", "[numeric_utils]") {
	REQUIRE(IsNegativeZero(RoundToNearestEven(-0.0)));
	REQUIRE(IsNegativeZero(RoundToNearestEven(-0.4)));
	REQUIRE(IsNegativeZero(RoundToNearestEven(-0.5)));
	REQUIRE(IsNegativeZero(RoundToNearestEven(-0.0f)));
	REQUIRE(IsNegativeZero(RoundToNearestEven(-0.5f)));

	REQUIRE(!IsNegativeZero(RoundToNearestEven(0.0)));
	REQUIRE(!IsNegativeZero(RoundToNearestEven(0.5)));
	REQUIRE(!IsNegativeZero(RoundToNearestEven(0.5f)));
}

TEST_CASE("RoundToNearestEven returns non-finite values unchanged", "[numeric_utils]") {
	const auto inf = std::numeric_limits<double>::infinity();
	REQUIRE(RoundToNearestEven(inf) == inf);
	REQUIRE(RoundToNearestEven(-inf) == -inf);
	REQUIRE(std::isnan(RoundToNearestEven(std::numeric_limits<double>::quiet_NaN())));

	const auto inf_float = std::numeric_limits<float>::infinity();
	REQUIRE(RoundToNearestEven(inf_float) == inf_float);
	REQUIRE(RoundToNearestEven(-inf_float) == -inf_float);
	REQUIRE(std::isnan(RoundToNearestEven(std::numeric_limits<float>::quiet_NaN())));
}

namespace {

template <class T>
bool SameFloat(T left, T right) {
	if (std::isnan(left) || std::isnan(right)) {
		return std::isnan(left) && std::isnan(right);
	}
	return left == right && std::signbit(left) == std::signbit(right);
}

template <class T>
void RequireRoundsAwayFromZero(T input, T expected) {
	INFO("input " << input);
	const T result = RoundToNearestAwayFromZero(input);
	REQUIRE(SameFloat(result, expected));
}

template <class T>
void RequireUnchangedByRounding(T input) {
	RequireRoundsAwayFromZero(input, input);
	RequireRoundsAwayFromZero(-input, -input);
}

//! Returns the first value for which the helper disagrees with the C runtime, or NaN if there is none
template <class T>
T FirstDisagreementWithRound(T start, T stop, T step) {
	for (T value = start; value < stop; value += step) {
		if (!SameFloat(RoundToNearestAwayFromZero(value), std::round(value))) {
			return value;
		}
		if (!SameFloat(RoundToNearestAwayFromZero(-value), std::round(-value))) {
			return -value;
		}
	}
	return std::numeric_limits<T>::quiet_NaN();
}

} // namespace

TEST_CASE("RoundToNearestAwayFromZero rounds halfway cases away from zero regardless of rounding mode",
          "[numeric_utils]") {
	struct RoundingCase {
		double input;
		double expected;
	};
	// every input is exact in binary, so the float and the double case are the same value
	const RoundingCase cases[] = {{0.0, 0.0},    {1.0, 1.0},    {0.5, 1.0},   {1.5, 2.0},   {2.5, 3.0},   {3.5, 4.0},
	                              {4.5, 5.0},    {-0.5, -1.0},  {-1.5, -2.0}, {-2.5, -3.0}, {-3.5, -4.0}, {0.25, 0.0},
	                              {0.75, 1.0},   {2.25, 2.0},   {2.75, 3.0},  {3.25, 3.0},  {3.75, 4.0},  {-0.25, -0.0},
	                              {-0.75, -1.0}, {-2.25, -2.0}, {-2.75, -3.0}};

	RoundingModeGuard guard;
	const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
	for (const auto mode : modes) {
		if (std::fesetround(mode) != 0) {
			// mode not supported on this platform
			continue;
		}
		REQUIRE(std::fegetround() == mode);
		for (const auto &test_case : cases) {
			// volatile stops the compiler from rounding these at compile time instead
			volatile double input = test_case.input;
			volatile float input_float = static_cast<float>(test_case.input);
			RequireRoundsAwayFromZero(static_cast<double>(input), test_case.expected);
			RequireRoundsAwayFromZero(static_cast<float>(input_float), static_cast<float>(test_case.expected));
		}
	}
}

TEST_CASE("RoundToNearestAwayFromZero rounds the neighbours of a half to the nearest integer", "[numeric_utils]") {
	// the double below 0.5 is the value the mingw-w64 round misrounds
	const double below_half = std::nextafter(0.5, 0.0);
	REQUIRE(below_half == 0.49999999999999994);
	RequireRoundsAwayFromZero(below_half, 0.0);
	RequireRoundsAwayFromZero(-below_half, -0.0);
	RequireRoundsAwayFromZero(std::nextafter(0.5, 1.0), 1.0);
	RequireRoundsAwayFromZero(std::nextafter(1.5, 0.0), 1.0);
	RequireRoundsAwayFromZero(std::nextafter(1.5, 2.0), 2.0);
	RequireRoundsAwayFromZero(std::nextafter(2.5, 0.0), 2.0);
	RequireRoundsAwayFromZero(-std::nextafter(2.5, 0.0), -2.0);

	const float below_half_float = std::nextafter(0.5f, 0.0f);
	REQUIRE(below_half_float == 0.49999997f);
	RequireRoundsAwayFromZero(below_half_float, 0.0f);
	RequireRoundsAwayFromZero(-below_half_float, -0.0f);
	RequireRoundsAwayFromZero(std::nextafter(0.5f, 1.0f), 1.0f);
	RequireRoundsAwayFromZero(std::nextafter(1.5f, 0.0f), 1.0f);
	RequireRoundsAwayFromZero(std::nextafter(1.5f, 2.0f), 2.0f);
	RequireRoundsAwayFromZero(-std::nextafter(2.5f, 0.0f), -2.0f);
}

TEST_CASE("RoundToNearestAwayFromZero preserves the sign of zero", "[numeric_utils]") {
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-0.0)));
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-0.25)));
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-std::numeric_limits<double>::denorm_min())));
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-0.0f)));
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-0.25f)));
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-std::numeric_limits<float>::denorm_min())));

	REQUIRE(!IsNegativeZero(RoundToNearestAwayFromZero(0.0)));
	REQUIRE(!IsNegativeZero(RoundToNearestAwayFromZero(0.25)));
	REQUIRE(!IsNegativeZero(RoundToNearestAwayFromZero(0.0f)));
	REQUIRE(!IsNegativeZero(RoundToNearestAwayFromZero(0.25f)));
	// a negative half rounds to minus one, not to a zero of either sign
	REQUIRE(RoundToNearestAwayFromZero(-0.5) == -1.0);
	REQUIRE(RoundToNearestAwayFromZero(-0.5f) == -1.0f);
}

TEST_CASE("RoundToNearestAwayFromZero at the limit of representable fractions", "[numeric_utils]") {
	// doubles from 2^52 on are integers, the last half is 2^52 - 0.5
	const double two_52 = 4503599627370496.0;
	REQUIRE(two_52 - 0.5 != two_52);
	REQUIRE(two_52 + 0.5 == two_52);
	RequireRoundsAwayFromZero(two_52 - 1.5, two_52 - 1.0);
	RequireRoundsAwayFromZero(two_52 - 0.5, two_52);
	RequireRoundsAwayFromZero(-(two_52 - 1.5), -(two_52 - 1.0));
	RequireRoundsAwayFromZero(-(two_52 - 0.5), -two_52);
	RequireUnchangedByRounding(two_52 - 1.0);
	RequireUnchangedByRounding(two_52);
	RequireUnchangedByRounding(two_52 + 1.0);
	RequireUnchangedByRounding(two_52 * 2);
	RequireUnchangedByRounding(two_52 * 2 + 2.0);
	RequireUnchangedByRounding(two_52 * 4 + 4.0);
	RequireUnchangedByRounding(1e300);
	RequireUnchangedByRounding(std::numeric_limits<double>::max());

	// floats from 2^23 on are integers, the last half is 2^23 - 0.5
	const float two_23 = 8388608.0f;
	REQUIRE(two_23 - 0.5f != two_23);
	REQUIRE(two_23 + 0.5f == two_23);
	RequireRoundsAwayFromZero(two_23 - 1.5f, two_23 - 1.0f);
	RequireRoundsAwayFromZero(two_23 - 0.5f, two_23);
	RequireRoundsAwayFromZero(-(two_23 - 1.5f), -(two_23 - 1.0f));
	RequireRoundsAwayFromZero(-(two_23 - 0.5f), -two_23);
	RequireUnchangedByRounding(two_23 - 1.0f);
	RequireUnchangedByRounding(two_23);
	RequireUnchangedByRounding(two_23 + 1.0f);
	RequireUnchangedByRounding(two_23 * 2);
	RequireUnchangedByRounding(two_23 * 2 + 2.0f);
	RequireUnchangedByRounding(two_23 * 4 + 4.0f);
	RequireUnchangedByRounding(1e30f);
	RequireUnchangedByRounding(std::numeric_limits<float>::max());
}

TEST_CASE("RoundToNearestAwayFromZero returns non-finite values unchanged", "[numeric_utils]") {
	const auto inf = std::numeric_limits<double>::infinity();
	REQUIRE(RoundToNearestAwayFromZero(inf) == inf);
	REQUIRE(RoundToNearestAwayFromZero(-inf) == -inf);
	REQUIRE(std::isnan(RoundToNearestAwayFromZero(std::numeric_limits<double>::quiet_NaN())));
	REQUIRE(std::signbit(RoundToNearestAwayFromZero(-std::numeric_limits<double>::quiet_NaN())));

	const auto inf_float = std::numeric_limits<float>::infinity();
	REQUIRE(RoundToNearestAwayFromZero(inf_float) == inf_float);
	REQUIRE(RoundToNearestAwayFromZero(-inf_float) == -inf_float);
	REQUIRE(std::isnan(RoundToNearestAwayFromZero(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("RoundToNearestAwayFromZero agrees with the C runtime round", "[numeric_utils]") {
	// sixteenths cover the quarters and the halves, k + 0.5 covers every half below 2^20
	// the grids avoid the double below 0.5, which the mingw-w64 round misrounds
	const double sixteenths_stop = 4096.0;
	const double halves_stop = 1048576.0;
	REQUIRE(std::isnan(FirstDisagreementWithRound(0.0, sixteenths_stop, 0.0625)));
	REQUIRE(std::isnan(FirstDisagreementWithRound(0.5, halves_stop, 1.0)));
	REQUIRE(std::isnan(FirstDisagreementWithRound(0.0f, static_cast<float>(sixteenths_stop), 0.0625f)));
	REQUIRE(std::isnan(FirstDisagreementWithRound(0.5f, static_cast<float>(halves_stop), 1.0f)));
}
