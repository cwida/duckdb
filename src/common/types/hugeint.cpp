#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/limits.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

//===--------------------------------------------------------------------===//
// String Conversion
//===--------------------------------------------------------------------===//
const hugeint_t Hugeint::POWERS_OF_TEN[] {
    hugeint_t(1),
    hugeint_t(10),
    hugeint_t(100),
    hugeint_t(1000),
    hugeint_t(10000),
    hugeint_t(100000),
    hugeint_t(1000000),
    hugeint_t(10000000),
    hugeint_t(100000000),
    hugeint_t(1000000000),
    hugeint_t(10000000000),
    hugeint_t(100000000000),
    hugeint_t(1000000000000),
    hugeint_t(10000000000000),
    hugeint_t(100000000000000),
    hugeint_t(1000000000000000),
    hugeint_t(10000000000000000),
    hugeint_t(100000000000000000),
    hugeint_t(1000000000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(10),
    hugeint_t(1000000000000000000) * hugeint_t(100),
    hugeint_t(1000000000000000000) * hugeint_t(1000),
    hugeint_t(1000000000000000000) * hugeint_t(10000),
    hugeint_t(1000000000000000000) * hugeint_t(100000),
    hugeint_t(1000000000000000000) * hugeint_t(1000000),
    hugeint_t(1000000000000000000) * hugeint_t(10000000),
    hugeint_t(1000000000000000000) * hugeint_t(100000000),
    hugeint_t(1000000000000000000) * hugeint_t(1000000000),
    hugeint_t(1000000000000000000) * hugeint_t(10000000000),
    hugeint_t(1000000000000000000) * hugeint_t(100000000000),
    hugeint_t(1000000000000000000) * hugeint_t(1000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(10000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(100000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(1000000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(10000000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(100000000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(1000000000000000000),
    hugeint_t(1000000000000000000) * hugeint_t(1000000000000000000) * hugeint_t(10),
    hugeint_t(1000000000000000000) * hugeint_t(1000000000000000000) * hugeint_t(100)};

static uint8_t PositiveHugeintHighestBit(hugeint_t bits) {
	uint8_t out = 0;
	if (bits.upper) {
		out = 64;
		uint64_t up = bits.upper;
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

static bool PositiveHugeintIsBitSet(hugeint_t lhs, uint8_t bit_position) {
	if (bit_position < 64) {
		return lhs.lower & (uint64_t(1) << uint64_t(bit_position));
	} else {
		return lhs.upper & (uint64_t(1) << uint64_t(bit_position - 64));
	}
}

hugeint_t PositiveHugeintLeftShift(hugeint_t lhs, uint32_t amount) {
	D_ASSERT(amount > 0 && amount < 64);
	hugeint_t result;
	result.lower = lhs.lower << amount;
	result.upper = (lhs.upper << amount) + (lhs.lower >> (64 - amount));
	return result;
}

hugeint_t Hugeint::DivModPositive(hugeint_t lhs, uint64_t rhs, uint64_t &remainder) {
	D_ASSERT(lhs.upper >= 0);
	// DivMod code adapted from:
	// https://github.com/calccrypto/uint128_t/blob/master/uint128_t.cpp

	// initialize the result and remainder to 0
	hugeint_t div_result;
	div_result.lower = 0;
	div_result.upper = 0;
	remainder = 0;

	uint8_t highest_bit_set = PositiveHugeintHighestBit(lhs);
	// now iterate over the amount of bits that are set in the LHS
	for (uint8_t x = highest_bit_set; x > 0; x--) {
		// left-shift the current result and remainder by 1
		div_result = PositiveHugeintLeftShift(div_result, 1);
		remainder <<= 1;
		// we get the value of the bit at position X, where position 0 is the least-significant bit
		if (PositiveHugeintIsBitSet(lhs, x - 1)) {
			// increment the remainder
			remainder++;
		}
		if (remainder >= rhs) {
			// the remainder has passed the division multiplier: add one to the divide result
			remainder -= rhs;
			div_result.lower++;
			if (div_result.lower == 0) {
				// overflow
				div_result.upper++;
			}
		}
	}
	return div_result;
}

string Hugeint::ToString(hugeint_t input) {
	uint64_t remainder;
	string result;
	bool negative = input.upper < 0;
	if (negative) {
		NegateInPlace(input);
	}
	while (true) {
		if (!input.lower && !input.upper) {
			break;
		}
		input = Hugeint::DivModPositive(input, 10, remainder);
		result = string(1, '0' + remainder) + result; // NOLINT
	}
	if (result.empty()) {
		// value is zero
		return "0";
	}
	return negative ? "-" + result : result;
}

//===--------------------------------------------------------------------===//
// Multiply
//===--------------------------------------------------------------------===//
bool Hugeint::TryMultiply(hugeint_t lhs, hugeint_t rhs, hugeint_t &result) {
	bool lhs_negative = lhs.upper < 0;
	bool rhs_negative = rhs.upper < 0;
	if (lhs_negative) {
		NegateInPlace(lhs);
	}
	if (rhs_negative) {
		NegateInPlace(rhs);
	}
#if ((__GNUC__ >= 5) || defined(__clang__)) && defined(__SIZEOF_INT128__)
	__uint128_t left = __uint128_t(lhs.lower) + (__uint128_t(lhs.upper) << 64);
	__uint128_t right = __uint128_t(rhs.lower) + (__uint128_t(rhs.upper) << 64);
	__uint128_t result_i128;
	if (__builtin_mul_overflow(left, right, &result_i128)) {
		return false;
	}
	uint64_t upper = uint64_t(result_i128 >> 64);
	if (upper & 0x8000000000000000) {
		return false;
	}
	result.upper = int64_t(upper);
	result.lower = uint64_t(result_i128 & 0xffffffffffffffff);
#else
	// Multiply code adapted from:
	// https://github.com/calccrypto/uint128_t/blob/master/uint128_t.cpp

	// split values into 4 32-bit parts
	uint64_t top[4] = {uint64_t(lhs.upper) >> 32, uint64_t(lhs.upper) & 0xffffffff, lhs.lower >> 32,
	                   lhs.lower & 0xffffffff};
	uint64_t bottom[4] = {uint64_t(rhs.upper) >> 32, uint64_t(rhs.upper) & 0xffffffff, rhs.lower >> 32,
	                      rhs.lower & 0xffffffff};
	uint64_t products[4][4];

	// multiply each component of the values
	for (auto x = 0; x < 4; x++) {
		for (auto y = 0; y < 4; y++) {
			products[x][y] = top[x] * bottom[y];
		}
	}

	// if any of these products are set to a non-zero value, there is always an overflow
	if (products[0][0] || products[0][1] || products[0][2] || products[1][0] || products[2][0] || products[1][1]) {
		return false;
	}
	// if the high bits of any of these are set, there is always an overflow
	if ((products[0][3] & 0xffffffff80000000) || (products[1][2] & 0xffffffff80000000) ||
	    (products[2][1] & 0xffffffff80000000) || (products[3][0] & 0xffffffff80000000)) {
		return false;
	}

	// otherwise we merge the result of the different products together in-order

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

	// check if the combination of the different products resulted in an overflow
	if (first32 & 0xffffff80000000) {
		return false;
	}

	// remove carry from current digit
	fourth32 &= 0xffffffff;
	third32 &= 0xffffffff;
	second32 &= 0xffffffff;
	first32 &= 0xffffffff;

	// combine components
	result.lower = (third32 << 32) | fourth32;
	result.upper = (first32 << 32) | second32;
#endif
	if (lhs_negative ^ rhs_negative) {
		NegateInPlace(result);
	}
	return true;
}

hugeint_t Hugeint::Multiply(hugeint_t lhs, hugeint_t rhs) {
	hugeint_t result;
	if (!TryMultiply(lhs, rhs, result)) {
		throw OutOfRangeException("Overflow in HUGEINT multiplication!");
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Divide
//===--------------------------------------------------------------------===//
hugeint_t Hugeint::DivMod(hugeint_t lhs, hugeint_t rhs, hugeint_t &remainder) {
	// division by zero not allowed
	D_ASSERT(!(rhs.upper == 0 && rhs.lower == 0));

	bool lhs_negative = lhs.upper < 0;
	bool rhs_negative = rhs.upper < 0;
	if (lhs_negative) {
		Hugeint::NegateInPlace(lhs);
	}
	if (rhs_negative) {
		Hugeint::NegateInPlace(rhs);
	}
	// DivMod code adapted from:
	// https://github.com/calccrypto/uint128_t/blob/master/uint128_t.cpp

	// initialize the result and remainder to 0
	hugeint_t div_result;
	div_result.lower = 0;
	div_result.upper = 0;
	remainder.lower = 0;
	remainder.upper = 0;

	uint8_t highest_bit_set = PositiveHugeintHighestBit(lhs);
	// now iterate over the amount of bits that are set in the LHS
	for (uint8_t x = highest_bit_set; x > 0; x--) {
		// left-shift the current result and remainder by 1
		div_result = PositiveHugeintLeftShift(div_result, 1);
		remainder = PositiveHugeintLeftShift(remainder, 1);

		// we get the value of the bit at position X, where position 0 is the least-significant bit
		if (PositiveHugeintIsBitSet(lhs, x - 1)) {
			// increment the remainder
			Hugeint::AddInPlace(remainder, 1);
		}
		if (Hugeint::GreaterThanEquals(remainder, rhs)) {
			// the remainder has passed the division multiplier: add one to the divide result
			remainder = Hugeint::Subtract(remainder, rhs);
			Hugeint::AddInPlace(div_result, 1);
		}
	}
	if (lhs_negative ^ rhs_negative) {
		Hugeint::NegateInPlace(div_result);
	}
	if (lhs_negative) {
		Hugeint::NegateInPlace(remainder);
	}
	return div_result;
}

hugeint_t Hugeint::Divide(hugeint_t lhs, hugeint_t rhs) {
	hugeint_t remainder;
	return Hugeint::DivMod(lhs, rhs, remainder);
}

hugeint_t Hugeint::Modulo(hugeint_t lhs, hugeint_t rhs) {
	hugeint_t remainder;
	Hugeint::DivMod(lhs, rhs, remainder);
	return remainder;
}

//===--------------------------------------------------------------------===//
// Add/Subtract
//===--------------------------------------------------------------------===//
bool Hugeint::AddInPlace(hugeint_t &lhs, hugeint_t rhs) {
	int overflow = lhs.lower + rhs.lower < lhs.lower;
	if (rhs.upper >= 0) {
		// RHS is positive: check for overflow
		if (lhs.upper > (std::numeric_limits<int64_t>::max() - rhs.upper - overflow)) {
			return false;
		}
		lhs.upper = lhs.upper + overflow + rhs.upper;
	} else {
		// RHS is negative: check for underflow
		if (lhs.upper < std::numeric_limits<int64_t>::min() - rhs.upper - overflow) {
			return false;
		}
		lhs.upper = lhs.upper + (overflow + rhs.upper);
	}
	lhs.lower += rhs.lower;
	if (lhs.upper == std::numeric_limits<int64_t>::min() && lhs.lower == 0) {
		return false;
	}
	return true;
}

bool Hugeint::SubtractInPlace(hugeint_t &lhs, hugeint_t rhs) {
	// underflow
	int underflow = lhs.lower - rhs.lower > lhs.lower;
	if (rhs.upper >= 0) {
		// RHS is positive: check for underflow
		if (lhs.upper < (std::numeric_limits<int64_t>::min() + rhs.upper + underflow)) {
			return false;
		}
		lhs.upper = (lhs.upper - rhs.upper) - underflow;
	} else {
		// RHS is negative: check for overflow
		if (lhs.upper > std::numeric_limits<int64_t>::min() &&
		    lhs.upper - 1 >= (std::numeric_limits<int64_t>::max() + rhs.upper + underflow)) {
			return false;
		}
		lhs.upper = lhs.upper - (rhs.upper + underflow);
	}
	lhs.lower -= rhs.lower;
	if (lhs.upper == std::numeric_limits<int64_t>::min() && lhs.lower == 0) {
		return false;
	}
	return true;
}

hugeint_t Hugeint::Add(hugeint_t lhs, hugeint_t rhs) {
	if (!AddInPlace(lhs, rhs)) {
		throw OutOfRangeException("Overflow in HUGEINT addition");
	}
	return lhs;
}

hugeint_t Hugeint::Subtract(hugeint_t lhs, hugeint_t rhs) {
	if (!SubtractInPlace(lhs, rhs)) {
		throw OutOfRangeException("Underflow in HUGEINT addition");
	}
	return lhs;
}

//===--------------------------------------------------------------------===//
// Hugeint Cast/Conversion
//===--------------------------------------------------------------------===//
template <class DST, bool SIGNED = true>
bool HugeintTryCastInteger(hugeint_t input, DST &result) {
	switch (input.upper) {
	case 0:
		// positive number: check if the positive number is in range
		if (input.lower <= uint64_t(NumericLimits<DST>::Maximum())) {
			result = DST(input.lower);
			return true;
		}
		break;
	case -1:
		if (!SIGNED) {
			return false;
		}
		// negative number: check if the negative number is in range
		if (input.lower > NumericLimits<uint64_t>::Maximum() - uint64_t(NumericLimits<DST>::Maximum())) {
			result = -DST(NumericLimits<uint64_t>::Maximum() - input.lower + 1);
			return true;
		}
		break;
	default:
		break;
	}
	return false;
}

template <>
bool Hugeint::TryCast(hugeint_t input, int8_t &result) {
	return HugeintTryCastInteger<int8_t>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, int16_t &result) {
	return HugeintTryCastInteger<int16_t>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, int32_t &result) {
	return HugeintTryCastInteger<int32_t>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, int64_t &result) {
	return HugeintTryCastInteger<int64_t>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, uint8_t &result) {
	return HugeintTryCastInteger<uint8_t, false>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, uint16_t &result) {
	return HugeintTryCastInteger<uint16_t, false>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, uint32_t &result) {
	return HugeintTryCastInteger<uint32_t, false>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, uint64_t &result) {
	return HugeintTryCastInteger<uint64_t, false>(input, result);
}

template <>
bool Hugeint::TryCast(hugeint_t input, hugeint_t &result) {
	result = input;
	return true;
}

template <>
bool Hugeint::TryCast(hugeint_t input, float &result) {
	double dbl_result;
	Hugeint::TryCast(input, dbl_result);
	result = (float)dbl_result;
	return true;
}

template <>
bool Hugeint::TryCast(hugeint_t input, double &result) {
	switch (input.upper) {
	case -1:
		// special case for upper = -1 to avoid rounding issues in small negative numbers
		result = -double(NumericLimits<uint64_t>::Maximum() - input.lower) - 1;
		break;
	default:
		result = double(input.lower) + double(input.upper) * double(NumericLimits<uint64_t>::Maximum());
		break;
	}
	return true;
}

template <class DST>
hugeint_t HugeintConvertInteger(DST input) {
	hugeint_t result;
	result.lower = (uint64_t)input;
	result.upper = (input < 0) * -1;
	return result;
}

template <>
hugeint_t Hugeint::Convert(int8_t value) {
	return HugeintConvertInteger<int8_t>(value);
}

template <>
hugeint_t Hugeint::Convert(int16_t value) {
	return HugeintConvertInteger<int16_t>(value);
}

template <>
hugeint_t Hugeint::Convert(int32_t value) {
	return HugeintConvertInteger<int32_t>(value);
}

template <>
hugeint_t Hugeint::Convert(int64_t value) {
	return HugeintConvertInteger<int64_t>(value);
}
template <>
hugeint_t Hugeint::Convert(uint8_t value) {
	return HugeintConvertInteger<uint8_t>(value);
}
template <>
hugeint_t Hugeint::Convert(uint16_t value) {
	return HugeintConvertInteger<uint16_t>(value);
}
template <>
hugeint_t Hugeint::Convert(uint32_t value) {
	return HugeintConvertInteger<uint32_t>(value);
}
template <>
hugeint_t Hugeint::Convert(uint64_t value) {
	return HugeintConvertInteger<uint64_t>(value);
}

template <>
hugeint_t Hugeint::Convert(float value) {
	return Hugeint::Convert<double>(value);
}

template <>
hugeint_t Hugeint::Convert(double value) {
	if (value <= -170141183460469231731687303715884105728.0 || value >= 170141183460469231731687303715884105727.0) {
		throw OutOfRangeException("Double out of range of HUGEINT");
	}
	hugeint_t result;
	bool negative = value < 0;
	if (negative) {
		value = -value;
	}
	result.lower = (uint64_t)fmod(value, double(NumericLimits<uint64_t>::Maximum()));
	result.upper = (uint64_t)(value / double(NumericLimits<uint64_t>::Maximum()));
	if (negative) {
		NegateInPlace(result);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// hugeint_t operators
//===--------------------------------------------------------------------===//
hugeint_t::hugeint_t(int64_t value) {
	auto result = Hugeint::Convert(value);
	this->lower = result.lower;
	this->upper = result.upper;
}

bool hugeint_t::operator==(const hugeint_t &rhs) const {
	return Hugeint::Equals(*this, rhs);
}

bool hugeint_t::operator!=(const hugeint_t &rhs) const {
	return Hugeint::NotEquals(*this, rhs);
}

bool hugeint_t::operator<(const hugeint_t &rhs) const {
	return Hugeint::LessThan(*this, rhs);
}

bool hugeint_t::operator<=(const hugeint_t &rhs) const {
	return Hugeint::LessThanEquals(*this, rhs);
}

bool hugeint_t::operator>(const hugeint_t &rhs) const {
	return Hugeint::GreaterThan(*this, rhs);
}

bool hugeint_t::operator>=(const hugeint_t &rhs) const {
	return Hugeint::GreaterThanEquals(*this, rhs);
}

hugeint_t hugeint_t::operator+(const hugeint_t &rhs) const {
	return Hugeint::Add(*this, rhs);
}

hugeint_t hugeint_t::operator-(const hugeint_t &rhs) const {
	return Hugeint::Subtract(*this, rhs);
}

hugeint_t hugeint_t::operator*(const hugeint_t &rhs) const {
	return Hugeint::Multiply(*this, rhs);
}

hugeint_t hugeint_t::operator/(const hugeint_t &rhs) const {
	return Hugeint::Divide(*this, rhs);
}

hugeint_t hugeint_t::operator%(const hugeint_t &rhs) const {
	return Hugeint::Modulo(*this, rhs);
}

hugeint_t hugeint_t::operator-() const {
	return Hugeint::Negate(*this);
}

hugeint_t hugeint_t::operator>>(const hugeint_t &rhs) const {
	if (upper < 0) {
		return hugeint_t(0);
	}
	hugeint_t result;
	uint64_t shift = rhs.lower;
	if (rhs.upper != 0 || shift >= 128) {
		return hugeint_t(0);
	} else if (shift == 64) {
		result.upper = 0;
		result.lower = upper;
	} else if (shift == 0) {
		return *this;
	} else if (shift < 64) {
		// perform upper shift in unsigned integer, and mask away the most significant bit
		result.lower = (uint64_t(upper) << (64 - shift)) + (lower >> shift);
		result.upper = uint64_t(upper) >> shift;
	} else {
		D_ASSERT(shift < 128);
		result.lower = uint64_t(upper) >> (shift - 64);
		result.upper = 0;
	}
	return result;
}

hugeint_t hugeint_t::operator<<(const hugeint_t &rhs) const {
	if (upper < 0) {
		return hugeint_t(0);
	}
	hugeint_t result;
	uint64_t shift = rhs.lower;
	if (rhs.upper != 0 || shift >= 128) {
		return hugeint_t(0);
	} else if (shift == 64) {
		result.upper = lower;
		result.lower = 0;
	} else if (shift == 0) {
		return *this;
	} else if (shift < 64) {
		// perform upper shift in unsigned integer, and mask away the most significant bit
		uint64_t upper_shift = ((uint64_t(upper) << shift) + (lower >> (64 - shift))) & 0x7FFFFFFFFFFFFFFF;
		result.lower = lower << shift;
		result.upper = upper_shift;
	} else {
		D_ASSERT(shift < 128);
		result.lower = 0;
		result.upper = (lower << (shift - 64)) & 0x7FFFFFFFFFFFFFFF;
	}
	return result;
}

hugeint_t hugeint_t::operator&(const hugeint_t &rhs) const {
	hugeint_t result;
	result.lower = lower & rhs.lower;
	result.upper = upper & rhs.upper;
	return result;
}

hugeint_t hugeint_t::operator|(const hugeint_t &rhs) const {
	hugeint_t result;
	result.lower = lower | rhs.lower;
	result.upper = upper | rhs.upper;
	return result;
}

hugeint_t hugeint_t::operator^(const hugeint_t &rhs) const {
	hugeint_t result;
	result.lower = lower ^ rhs.lower;
	result.upper = upper ^ rhs.upper;
	return result;
}

hugeint_t hugeint_t::operator~() const {
	hugeint_t result;
	result.lower = ~lower;
	result.upper = ~upper;
	return result;
}

hugeint_t &hugeint_t::operator+=(const hugeint_t &rhs) {
	Hugeint::AddInPlace(*this, rhs);
	return *this;
}
hugeint_t &hugeint_t::operator-=(const hugeint_t &rhs) {
	Hugeint::SubtractInPlace(*this, rhs);
	return *this;
}
hugeint_t &hugeint_t::operator*=(const hugeint_t &rhs) {
	*this = Hugeint::Multiply(*this, rhs);
	return *this;
}
hugeint_t &hugeint_t::operator/=(const hugeint_t &rhs) {
	*this = Hugeint::Divide(*this, rhs);
	return *this;
}
hugeint_t &hugeint_t::operator%=(const hugeint_t &rhs) {
	*this = Hugeint::Modulo(*this, rhs);
	return *this;
}
hugeint_t &hugeint_t::operator>>=(const hugeint_t &rhs) {
	*this = *this >> rhs;
	return *this;
}
hugeint_t &hugeint_t::operator<<=(const hugeint_t &rhs) {
	*this = *this << rhs;
	return *this;
}
hugeint_t &hugeint_t::operator&=(const hugeint_t &rhs) {
	lower &= rhs.lower;
	upper &= rhs.upper;
	return *this;
}
hugeint_t &hugeint_t::operator|=(const hugeint_t &rhs) {
	lower |= rhs.lower;
	upper |= rhs.upper;
	return *this;
}
hugeint_t &hugeint_t::operator^=(const hugeint_t &rhs) {
	lower ^= rhs.lower;
	upper ^= rhs.upper;
	return *this;
}

string hugeint_t::ToString() const {
	return Hugeint::ToString(*this);
}

} // namespace duckdb
