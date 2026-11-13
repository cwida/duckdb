#include "catch.hpp"
#include "duckdb/common/types/hugeint.hpp"

using namespace duckdb;

namespace hugeint_constexpr_test {

constexpr hugeint_t minimum(INT64_MIN, 0);
constexpr hugeint_t maximum(INT64_MAX, UINT64_MAX);
constexpr hugeint_t limb(1, 0);

static_assert(minimum * 1 == minimum, "minimum times one is representable");
static_assert(hugeint_t(1) * minimum == minimum, "one times minimum is representable");
static_assert(minimum * 0 == 0, "minimum times zero");
static_assert(minimum * -1 == minimum, "unchecked multiplication wraps");
static_assert(-minimum == minimum, "unchecked negation wraps");
static_assert(maximum + 1 == minimum, "unchecked addition wraps");
static_assert(minimum - 1 == maximum, "unchecked subtraction wraps");
static_assert(hugeint_t(0, UINT64_MAX) + 1 == limb, "addition carries between limbs");
static_assert(limb - 1 == hugeint_t(0, UINT64_MAX), "subtraction borrows between limbs");
static_assert(limb * limb == 0, "unchecked multiplication discards high bits");
static_assert(hugeint_t(0, UINT64_MAX) * hugeint_t(0, UINT64_MAX) == hugeint_t(-2, 1),
              "multiplication carries between limbs");
static_assert(minimum + maximum == -1, "addition with opposite signs");
static_assert(minimum < -1 && hugeint_t(-1) < 0 && hugeint_t(0) < maximum, "signed ordering");
static_assert(minimum / 10 == hugeint_t(-922337203685477581LL, 3689348814741910324ULL),
              "negative decimal parsing limit");
static_assert(maximum / 10 == hugeint_t(922337203685477580LL, 14757395258967641292ULL),
              "positive decimal parsing limit");

struct DivisionCase {
	hugeint_t lhs;
	hugeint_t rhs;
	hugeint_t quotient;
	hugeint_t remainder;
};

constexpr DivisionCase divisions[] = {
    {7, 3, 2, 1},
    {-7, 3, -2, -1},
    {7, -3, -2, 1},
    {-7, -3, 2, -1},
    {minimum, 1, minimum, 0},
    {minimum, -1, minimum, 0},
    {minimum, 2, hugeint_t(INT64_MIN / 2, 0), 0},
    {minimum, 3, hugeint_t(-3074457345618258603LL, 6148914691236517206ULL), -2},
    {minimum, maximum, -1, -1},
    {minimum, minimum, 1, 0},
    {maximum, minimum, 0, maximum},
    {maximum, maximum, 1, 0},
    {minimum, 0, 0, minimum},
    {limb, 3, hugeint_t(0, 6148914691236517205ULL), 1},
};

constexpr bool CheckDivisions() {
	for (const auto &test : divisions) {
		const auto result = HugeintDivMod(test.lhs, test.rhs);
		if (result.quotient != test.quotient || result.remainder != test.remainder ||
		    test.lhs / test.rhs != test.quotient) {
			return false;
		}
	}
	return true;
}

static_assert(CheckDivisions(), "constant division agrees with signed truncation and unchecked overflow rules");

hugeint_t RuntimeValue(hugeint_t value) {
	volatile int64_t upper = value.upper;
	volatile uint64_t lower = value.lower;
	return hugeint_t(upper, lower);
}

} // namespace hugeint_constexpr_test

TEST_CASE("HUGEINT constexpr and runtime arithmetic agree at boundaries", "[hugeint]") {
	using namespace hugeint_constexpr_test;
	for (const auto &test : divisions) {
		const auto lhs = RuntimeValue(test.lhs);
		const auto rhs = RuntimeValue(test.rhs);
		hugeint_t remainder;
		REQUIRE(lhs / rhs == test.quotient);
		REQUIRE(Hugeint::DivMod(lhs, rhs, remainder) == test.quotient);
		REQUIRE(remainder == test.remainder);
		REQUIRE(lhs % rhs == test.remainder);
	}
	const auto min = RuntimeValue(minimum);
	const auto max = RuntimeValue(maximum);
	REQUIRE(min * 1 == min);
	REQUIRE(min * -1 == min);
	REQUIRE(-min == min);
	REQUIRE(max + 1 == min);
	REQUIRE(min - 1 == max);

	const hugeint_t operands[] = {minimum, minimum + 1, -limb, -1, 0, 1, limb, maximum};
	for (auto lhs : operands) {
		for (auto rhs : operands) {
			lhs = RuntimeValue(lhs);
			rhs = RuntimeValue(rhs);
			auto product = lhs;
			product *= rhs;
			REQUIRE(product == lhs * rhs);
		}
	}
}
