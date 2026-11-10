#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/common/string.hpp"
#include <stdint.h>
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

// Forward declaration to allow conversion between hugeint and uhugeint
struct uhugeint_t; // NOLINT: use numeric casing

struct hugeint_t { // NOLINT: use numeric casing
public:
	uint64_t lower;
	int64_t upper;

public:
	hugeint_t() = default;
	//! constexpr so that arithmetic against a constant operand can be folded at compile
	//! time; the body is at the bottom of this header
	constexpr hugeint_t(int64_t value); // NOLINT: Allow implicit conversion from `int64_t`
	constexpr hugeint_t(int64_t upper, uint64_t lower) : lower(lower), upper(upper) {
	}
	constexpr hugeint_t(const hugeint_t &rhs) = default;
	constexpr hugeint_t(hugeint_t &&rhs) = default;
	hugeint_t &operator=(const hugeint_t &rhs) = default;
	hugeint_t &operator=(hugeint_t &&rhs) = default;

	DUCKDB_API string ToString() const;

	// comparison operators
	//! constexpr so that comparisons fold at compile time
	constexpr bool operator==(const hugeint_t &rhs) const;
	constexpr bool operator!=(const hugeint_t &rhs) const;
	constexpr bool operator<=(const hugeint_t &rhs) const;
	constexpr bool operator<(const hugeint_t &rhs) const;
	constexpr bool operator>(const hugeint_t &rhs) const;
	constexpr bool operator>=(const hugeint_t &rhs) const;

	// arithmetic operators
	//! constexpr so the compiler can fold these when an operand is a compile-time constant
	constexpr hugeint_t operator+(const hugeint_t &rhs) const;
	constexpr hugeint_t operator-(const hugeint_t &rhs) const;
	constexpr hugeint_t operator*(const hugeint_t &rhs) const;
	constexpr hugeint_t operator/(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator%(const hugeint_t &rhs) const;
	constexpr hugeint_t operator-() const;

	// bitwise operators
	DUCKDB_API hugeint_t operator>>(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator<<(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator&(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator|(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator^(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator~() const;

	// in-place operators
	DUCKDB_API hugeint_t &operator+=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator-=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator*=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator/=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator%=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator>>=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator<<=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator&=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator|=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator^=(const hugeint_t &rhs);

	// boolean operators
	DUCKDB_API explicit operator bool() const;
	DUCKDB_API bool operator!() const;

	// cast operators -- doesn't check bounds/overflow/underflow
	DUCKDB_API explicit operator uint8_t() const;
	DUCKDB_API explicit operator uint16_t() const;
	DUCKDB_API explicit operator uint32_t() const;
	DUCKDB_API explicit operator uint64_t() const;
	DUCKDB_API explicit operator int8_t() const;
	DUCKDB_API explicit operator int16_t() const;
	DUCKDB_API explicit operator int32_t() const;
	DUCKDB_API explicit operator int64_t() const;
	DUCKDB_API explicit operator uhugeint_t() const;
};

//===--------------------------------------------------------------------===//
// constexpr 128-bit arithmetic
//
// These live here rather than next to the rest of Hugeint because
// duckdb/common/types/hugeint.hpp includes duckdb/common/limits.hpp, which
// includes this file back. Everything the digit-parsing loops need has to be
// reachable from here, and the bodies are at the bottom of the file because a
// member function body defined inside the class is looked up at the class's
// closing brace -- above the helpers below.
//===--------------------------------------------------------------------===//

//! NumericLimits<hugeint_t>::Minimum()/Maximum(), spelled out because limits.hpp
//! includes this file back so the traits cannot be used here
constexpr hugeint_t HugeintMinimum() {
	return hugeint_t(INT64_MIN, 0);
}

constexpr hugeint_t HugeintMaximum() {
	return hugeint_t(INT64_MAX, UINT64_MAX);
}

constexpr bool HugeintEquals(hugeint_t lhs, hugeint_t rhs) {
	return lhs.upper == rhs.upper && lhs.lower == rhs.lower;
}

constexpr bool HugeintNotEquals(hugeint_t lhs, hugeint_t rhs) {
	return !HugeintEquals(lhs, rhs);
}

constexpr bool HugeintGreaterThan(hugeint_t lhs, hugeint_t rhs) {
	return lhs.upper > rhs.upper || (lhs.upper == rhs.upper && lhs.lower > rhs.lower);
}

constexpr bool HugeintGreaterThanEquals(hugeint_t lhs, hugeint_t rhs) {
	return lhs.upper > rhs.upper || (lhs.upper == rhs.upper && lhs.lower >= rhs.lower);
}

constexpr bool HugeintLessThan(hugeint_t lhs, hugeint_t rhs) {
	return lhs.upper < rhs.upper || (lhs.upper == rhs.upper && lhs.lower < rhs.lower);
}

constexpr bool HugeintLessThanEquals(hugeint_t lhs, hugeint_t rhs) {
	return lhs.upper < rhs.upper || (lhs.upper == rhs.upper && lhs.lower <= rhs.lower);
}

//! Two's-complement negation, written out because the operators below cannot call
//! operator-() on themselves without the fold failing
constexpr hugeint_t HugeintNegate(hugeint_t n) {
	return hugeint_t(static_cast<int64_t>(~static_cast<uint64_t>(n.upper) + (n.lower == 0)), 0 - n.lower);
}

constexpr int HugeintSign(hugeint_t n) {
	// the sign is not decided by upper alone: upper == 0 with a non-zero lower is positive
	return int(HugeintGreaterThan(n, hugeint_t(0))) - int(HugeintLessThan(n, hugeint_t(0)));
}

constexpr hugeint_t HugeintAbs(hugeint_t n) {
	return n.upper < 0 ? HugeintNegate(n) : n;
}

constexpr hugeint_t HugeintAddOne(hugeint_t n) {
	const uint64_t carry = (n.lower == UINT64_MAX) ? 1 : 0;
	return hugeint_t(static_cast<int64_t>(static_cast<uint64_t>(n.upper) + carry), n.lower + 1);
}

constexpr hugeint_t HugeintSubtractOne(hugeint_t n) {
	// lower wraps to UINT64_MAX, so a borrow is taken out of upper
	const uint64_t borrow = (n.lower == 0) ? 1 : 0;
	return hugeint_t(static_cast<int64_t>(static_cast<uint64_t>(n.upper) - borrow), n.lower - 1);
}

//! Helpers for the bit-at-a-time division, from the same uint128_t reference the
//! runtime implementation uses
constexpr uint8_t HugeintHighestBit(hugeint_t bits) {
	uint8_t out = 0;
	if (bits.upper) {
		// bit index 64 is the lowest bit of upper
		out = 64;
		uint64_t up = static_cast<uint64_t>(bits.upper);
		while (up) {
			up >>= 1;
			out++;
		}
	} else {
		uint64_t low = bits.lower;
		while (low) {
			low >>= 1;
			out++;
		}
	}
	return out;
}

constexpr bool HugeintIsBitSet(hugeint_t lhs, uint8_t bit_position) {
	if (bit_position < 64) {
		return lhs.lower & (uint64_t(1) << uint64_t(bit_position));
	}
	return static_cast<uint64_t>(lhs.upper) & (uint64_t(1) << uint64_t(bit_position - 64));
}

//! Shift left by 1..63 bits. Called by the division loop below with amount == 1.
//! Takes no D_ASSERT: in assertion builds that macro is an unconditional function call, which
//! would make this function unusable in a constant expression.
constexpr hugeint_t HugeintLeftShift(hugeint_t lhs, uint32_t amount) {
	hugeint_t result {0, 0};
	result.lower = lhs.lower << amount;
	result.upper = static_cast<int64_t>((static_cast<uint64_t>(lhs.upper) << amount) + (lhs.lower >> (64 - amount)));
	return result;
}

constexpr hugeint_t HugeintMultiplyUnchecked(hugeint_t lhs, hugeint_t rhs) {
	// Unchecked multiplication uses the low 128 bits, including for negative operands.
#if ((__GNUC__ >= 5) || defined(__clang__)) && defined(__SIZEOF_INT128__)
	const auto left = (static_cast<__uint128_t>(static_cast<uint64_t>(lhs.upper)) << 64) | lhs.lower;
	const auto right = (static_cast<__uint128_t>(static_cast<uint64_t>(rhs.upper)) << 64) | rhs.lower;
	const auto result = left * right;
	return hugeint_t(static_cast<int64_t>(result >> 64), static_cast<uint64_t>(result));
#else
	// Multiply code adapted from:
	// https://github.com/calccrypto/uint128_t/blob/master/uint128_t.cpp

	// split values into 4 32-bit parts
	uint64_t top[4] {uint64_t(lhs.upper) >> 32, uint64_t(lhs.upper) & 0xffffffff, lhs.lower >> 32,
	                 lhs.lower & 0xffffffff};
	uint64_t bottom[4] {uint64_t(rhs.upper) >> 32, uint64_t(rhs.upper) & 0xffffffff, rhs.lower >> 32,
	                    rhs.lower & 0xffffffff};
	uint64_t products[4][4] {};

	// multiply each component of the values
	for (idx_t x = 0; x < 4; x++) {
		for (idx_t y = 0; y < 4; y++) {
			products[x][y] = top[x] * bottom[y];
		}
	}

	// first row
	uint64_t fourth32 = (products[3][3] & 0xffffffff);
	uint64_t third32 = (products[3][2] & 0xffffffff) + (products[3][3] >> 32);
	uint64_t second32 = (products[3][1] & 0xffffffff) + (products[3][2] >> 32);
	uint64_t first32 = (products[3][0] & 0xffffffff) + (products[3][1] >> 32);

	// second row
	third32 += (products[2][3] & 0xffffffff);
	second32 += (products[2][2] & 0xffffffff) + (products[2][3] >> 32);
	first32 += (products[2][1] & 0xffffffff) + (products[2][2] >> 32);

	// third row
	second32 += (products[1][3] & 0xffffffff);
	first32 += (products[1][2] & 0xffffffff) + (products[1][3] >> 32);

	// fourth row
	first32 += (products[0][3] & 0xffffffff);

	// move carry to next digit
	third32 += fourth32 >> 32;
	second32 += third32 >> 32;
	first32 += second32 >> 32;

	// remove carry from current digit
	fourth32 &= 0xffffffff;
	third32 &= 0xffffffff;
	second32 &= 0xffffffff;
	first32 &= 0xffffffff;

	return hugeint_t(static_cast<int64_t>((first32 << 32) + second32), (third32 << 32) + fourth32);
#endif
}

//! Result of a 128-bit division. Hugeint::DivMod's out-parameter form is declared in
//! types/hugeint.hpp, which is not visible from this header
struct HugeintDivModResult {
	hugeint_t quotient;
	hugeint_t remainder;
};

constexpr HugeintDivModResult HugeintDivMod(hugeint_t lhs, hugeint_t rhs);

//! Precondition: lhs or rhs equals HugeintMinimum(). Only reached through HugeintDivMod.
//! Takes no D_ASSERT for the same reason as HugeintLeftShift.
constexpr HugeintDivModResult HugeintDivModMinimum(hugeint_t lhs, hugeint_t rhs) {
	// rhs == MINIMUM: the quotient is 1 with remainder 0 only when lhs == MINIMUM;
	// every other lhs has |lhs| < |MINIMUM|, so the quotient is 0 and the remainder is lhs
	if (rhs.upper == INT64_MIN && rhs.lower == 0) {
		if (lhs.upper == INT64_MIN && lhs.lower == 0) {
			return HugeintDivModResult {hugeint_t(1), hugeint_t(0)};
		}
		return HugeintDivModResult {hugeint_t(0), lhs};
	}

	auto shifted = HugeintDivMod(HugeintAddOne(lhs), rhs);
	auto quotient = shifted.quotient;
	auto remainder = shifted.remainder;

	// If the 1 mattered we need to adjust the result, otherwise the remainder
	if (HugeintEquals(HugeintAddOne(HugeintAbs(remainder)), HugeintAbs(rhs))) {
		quotient = HugeintSign(rhs) > 0 ? HugeintSubtractOne(quotient) : HugeintAddOne(quotient);
		remainder = hugeint_t(0);
	} else {
		remainder = HugeintSubtractOne(remainder);
	}
	return HugeintDivModResult {quotient, remainder};
}

//! Bit-at-a-time 128-bit division, adapted from
//! https://github.com/calccrypto/uint128_t/blob/master/uint128_t.cpp
constexpr HugeintDivModResult HugeintDivMod(hugeint_t lhs, hugeint_t rhs) {
	if (HugeintEquals(rhs, hugeint_t(0))) {
		return HugeintDivModResult {hugeint_t(0), lhs};
	}

	// MINIMUM cannot be negated, so it has to be handled before any sign is stripped
	if (HugeintEquals(lhs, HugeintMinimum()) || HugeintEquals(rhs, HugeintMinimum())) {
		return HugeintDivModMinimum(lhs, rhs);
	}

	bool lhs_negative = lhs.upper < 0;
	bool rhs_negative = rhs.upper < 0;
	if (lhs_negative) {
		lhs = HugeintNegate(lhs);
	}
	if (rhs_negative) {
		rhs = HugeintNegate(rhs);
	}

	hugeint_t quotient {0, 0};
	hugeint_t remainder {0, 0};

	const uint8_t highest_bit_set = HugeintHighestBit(lhs);
	for (uint8_t x = highest_bit_set; x > 0; x--) {
		// left-shift the current quotient and remainder by 1
		quotient = HugeintLeftShift(quotient, 1);
		remainder = HugeintLeftShift(remainder, 1);

		// we get the value of the bit at position X, where position 0 is the least-significant bit
		if (HugeintIsBitSet(lhs, x - 1)) {
			remainder.lower += 1;
			if (remainder.lower == 0) {
				remainder.upper += 1;
			}
		}
		if (HugeintGreaterThanEquals(remainder, rhs)) {
			// the remainder has passed the division multiplier: subtract the divisor,
			// carrying a borrow out of lower, and add one to the quotient
			const int64_t borrow = remainder.lower < rhs.lower ? 1 : 0;
			remainder.lower -= rhs.lower;
			remainder.upper -= rhs.upper + borrow;
			quotient.lower += 1;
			if (quotient.lower == 0) {
				quotient.upper += 1;
			}
		}
	}
	if (lhs_negative ^ rhs_negative) {
		quotient = HugeintNegate(quotient);
	}
	if (lhs_negative) {
		remainder = HugeintNegate(remainder);
	}
	return HugeintDivModResult {quotient, remainder};
}

constexpr hugeint_t::hugeint_t(int64_t value) // NOLINT: Allow implicit conversion from `int64_t`
    : lower(static_cast<uint64_t>(value)), upper(value < 0 ? -1 : 0) {
}

constexpr bool hugeint_t::operator==(const hugeint_t &rhs) const {
	return HugeintEquals(*this, rhs);
}

constexpr bool hugeint_t::operator!=(const hugeint_t &rhs) const {
	return HugeintNotEquals(*this, rhs);
}

constexpr bool hugeint_t::operator<(const hugeint_t &rhs) const {
	return HugeintLessThan(*this, rhs);
}

constexpr bool hugeint_t::operator<=(const hugeint_t &rhs) const {
	return HugeintLessThanEquals(*this, rhs);
}

constexpr bool hugeint_t::operator>(const hugeint_t &rhs) const {
	return HugeintGreaterThan(*this, rhs);
}

constexpr bool hugeint_t::operator>=(const hugeint_t &rhs) const {
	return HugeintGreaterThanEquals(*this, rhs);
}

constexpr hugeint_t hugeint_t::operator+(const hugeint_t &rhs) const {
	return hugeint_t(static_cast<int64_t>(static_cast<uint64_t>(upper) + static_cast<uint64_t>(rhs.upper) +
	                                      ((lower + rhs.lower) < lower)),
	                 lower + rhs.lower);
}

constexpr hugeint_t hugeint_t::operator-(const hugeint_t &rhs) const {
	return hugeint_t(static_cast<int64_t>(static_cast<uint64_t>(upper) - static_cast<uint64_t>(rhs.upper) -
	                                      ((lower - rhs.lower) > lower)),
	                 lower - rhs.lower);
}

constexpr hugeint_t hugeint_t::operator*(const hugeint_t &rhs) const {
	return HugeintMultiplyUnchecked(*this, rhs);
}

constexpr hugeint_t hugeint_t::operator/(const hugeint_t &rhs) const {
	// this is the unchecked division: dividing by zero yields 0 and MINIMUM / -1 wraps,
	// matching Hugeint::Divide<false>. Callers that need overflow checks use TryDivMod.
	return HugeintDivMod(*this, rhs).quotient;
}

constexpr hugeint_t hugeint_t::operator-() const {
	return HugeintNegate(*this);
}

} // namespace duckdb

namespace std {
template <>
struct hash<duckdb::hugeint_t> {
	size_t operator()(const duckdb::hugeint_t &val) const {
		using std::hash;
		return hash<int64_t> {}(val.upper) ^ hash<uint64_t> {}(val.lower);
	}
};
} // namespace std
