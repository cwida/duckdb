#include "catch.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/vector.hpp"

using namespace duckdb;

namespace {

// Deterministic 64-bit generator (splitmix64)
struct SplitMix64 {
	uint64_t state;
	uint64_t Next() {
		uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
};

const hugeint_t HUGE_MIN(NumericLimits<int64_t>::Minimum(), 0);
const hugeint_t HUGE_MAX(NumericLimits<int64_t>::Maximum(), NumericLimits<uint64_t>::Maximum());

vector<hugeint_t> EdgeValues() {
	const uint64_t two_63 = 1ULL << 63;
	vector<hugeint_t> base {hugeint_t(1),
	                        hugeint_t(2),
	                        hugeint_t(3),
	                        hugeint_t(7),
	                        hugeint_t(10),
	                        hugeint_t(1000000007),
	                        hugeint_t(100000000000000000LL),
	                        hugeint_t(0, two_63 - 1),
	                        hugeint_t(0, two_63),
	                        hugeint_t(0, two_63 + 1),
	                        hugeint_t(0, NumericLimits<uint64_t>::Maximum()),
	                        hugeint_t(1, 0),
	                        hugeint_t(1, 1),
	                        hugeint_t(0, 10000000000000000000ULL),
	                        hugeint_t(5421010862427522170LL, 687399551400673280ULL), // 10^38
	                        hugeint_t(542101086242752217LL, 68739955140067328ULL),   // 10^37
	                        hugeint_t(NumericLimits<int64_t>::Maximum(), 0),
	                        HUGE_MAX - 1,
	                        HUGE_MAX};
	vector<hugeint_t> result {hugeint_t(0)};
	for (auto &v : base) {
		result.push_back(v);
		result.push_back(-v);
	}
	return result;
}

hugeint_t RandomValue(SplitMix64 &rng) {
	const auto shape = rng.Next() % 4;
	hugeint_t value;
	switch (shape) {
	case 0: // full 128 bits
		value = hugeint_t(static_cast<int64_t>(rng.Next()), rng.Next());
		break;
	case 1: // just above 64 bits
		value = hugeint_t(static_cast<int64_t>(rng.Next() % 1000), rng.Next());
		break;
	case 2: // 64 bits
		value = hugeint_t(0, rng.Next());
		break;
	default: // small
		value = hugeint_t(0, rng.Next() % 100000);
		break;
	}
	if (value == HUGE_MIN) {
		value = HUGE_MAX;
	}
	return rng.Next() % 2 ? -value : value;
}

void CheckDivMod(hugeint_t lhs, hugeint_t rhs) {
	if (rhs == hugeint_t(0) || lhs == HUGE_MIN || rhs == HUGE_MIN) {
		return; // handled before either path is taken
	}
	const auto native = HugeintDivMod(lhs, rhs);
	const auto fallback = HugeintDivModFallback(lhs, rhs);
	INFO("lhs " << lhs.ToString() << " rhs " << rhs.ToString());
	REQUIRE(native.quotient == fallback.quotient);
	REQUIRE(native.remainder == fallback.remainder);
	// quotient * rhs + remainder == lhs, |remainder| < |rhs|, remainder has the sign of lhs
	REQUIRE(native.quotient * rhs + native.remainder == lhs);
	const auto abs_rem = native.remainder < 0 ? -native.remainder : native.remainder;
	const auto abs_rhs = rhs < 0 ? -rhs : rhs;
	REQUIRE(abs_rem < abs_rhs);
	REQUIRE((native.remainder == hugeint_t(0) || (native.remainder < 0) == (lhs < 0)));
}

void CheckDivModPositive(hugeint_t lhs, uint64_t rhs) {
	if (lhs < 0 || rhs == 0) {
		return;
	}
	uint64_t native_rem = 0;
	uint64_t fallback_rem = 0;
	const auto native = Hugeint::DivModPositive(lhs, rhs, native_rem);
	const auto fallback = Hugeint::DivModPositiveFallback(lhs, rhs, fallback_rem);
	INFO("lhs " << lhs.ToString() << " rhs " << rhs);
	REQUIRE(native == fallback);
	REQUIRE(native_rem == fallback_rem);
	REQUIRE(native_rem < rhs);
	REQUIRE(native * hugeint_t(0, rhs) + hugeint_t(0, native_rem) == lhs);
}

} // namespace

TEST_CASE("Native and fallback 128-bit division agree", "[hugeint]") {
	const auto edges = EdgeValues();
	for (auto &lhs : edges) {
		for (auto &rhs : edges) {
			CheckDivMod(lhs, rhs);
			CheckDivModPositive(lhs, rhs.lower);
		}
	}
	SplitMix64 rng {42};
	for (idx_t i = 0; i < 20000; i++) {
		const auto lhs = RandomValue(rng);
		const auto rhs = RandomValue(rng);
		CheckDivMod(lhs, rhs);
		CheckDivModPositive(lhs < 0 ? -lhs : lhs, rhs.lower);
		// divisors of 2^63 and above
		CheckDivModPositive(lhs < 0 ? -lhs : lhs, rhs.lower | (1ULL << 63));
	}
}
