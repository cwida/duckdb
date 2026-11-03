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

TEST_CASE("RoundToNearestAwayFromZero rounds halfway cases away from zero, and values just below a half down",
          "[numeric_utils]") {
	struct RoundingCase {
		double input;
		double expected;
	};
	// 0.49999999999999994 is the largest double below 0.5, which C runtimes that add 0.5 and floor round to 1
	const RoundingCase cases[] = {{0.0, 0.0},
	                              {0.5, 1.0},
	                              {1.5, 2.0},
	                              {2.5, 3.0},
	                              {-0.5, -1.0},
	                              {-2.5, -3.0},
	                              {2.25, 2.0},
	                              {-2.75, -3.0},
	                              {0.49999999999999994, 0.0},
	                              {-0.49999999999999994, 0.0},
	                              {4503599627370497.0, 4503599627370497.0}};

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
			REQUIRE(RoundToNearestAwayFromZero(static_cast<double>(input)) == test_case.expected);
		}
	}
	volatile float below_half = 0.49999997f;
	REQUIRE(RoundToNearestAwayFromZero(static_cast<float>(below_half)) == 0.0f);
	REQUIRE(RoundToNearestAwayFromZero(2.5f) == 3.0f);
}

TEST_CASE("RoundToNearestAwayFromZero preserves the sign of zero and non-finite values", "[numeric_utils]") {
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-0.0)));
	REQUIRE(IsNegativeZero(RoundToNearestAwayFromZero(-0.49999999999999994)));
	REQUIRE(!IsNegativeZero(RoundToNearestAwayFromZero(0.4)));

	const auto inf = std::numeric_limits<double>::infinity();
	REQUIRE(RoundToNearestAwayFromZero(inf) == inf);
	REQUIRE(RoundToNearestAwayFromZero(-inf) == -inf);
	REQUIRE(std::isnan(RoundToNearestAwayFromZero(std::numeric_limits<double>::quiet_NaN())));
}
