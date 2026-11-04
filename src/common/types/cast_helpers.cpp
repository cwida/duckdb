#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"

namespace duckdb {

const int64_t NumericHelper::POWERS_OF_TEN[] {1,
                                              10,
                                              100,
                                              1000,
                                              10000,
                                              100000,
                                              1000000,
                                              10000000,
                                              100000000,
                                              1000000000,
                                              10000000000,
                                              100000000000,
                                              1000000000000,
                                              10000000000000,
                                              100000000000000,
                                              1000000000000000,
                                              10000000000000000,
                                              100000000000000000,
                                              1000000000000000000};

const double NumericHelper::DOUBLE_POWERS_OF_TEN[] {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                                                    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
                                                    1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29,
                                                    1e30, 1e31, 1e32, 1e33, 1e34, 1e35, 1e36, 1e37, 1e38, 1e39};

template <>
int NumericHelper::UnsignedLength(uint8_t value) {
	int length = 1;
	length += value >= 10;
	length += value >= 100;
	return length;
}

template <>
int NumericHelper::UnsignedLength(uint16_t value) {
	int length = 1;
	length += value >= 10;
	length += value >= 100;
	length += value >= 1000;
	length += value >= 10000;
	return length;
}

template <>
int NumericHelper::UnsignedLength(uint32_t value) {
	if (value >= 10000) {
		int length = 5;
		length += value >= 100000;
		length += value >= 1000000;
		length += value >= 10000000;
		length += value >= 100000000;
		length += value >= 1000000000;
		return length;
	} else {
		int length = 1;
		length += value >= 10;
		length += value >= 100;
		length += value >= 1000;
		return length;
	}
}

template <>
int NumericHelper::UnsignedLength(uint64_t value) {
	if (value >= 10000000000ULL) {
		if (value >= 1000000000000000ULL) {
			int length = 16;
			length += value >= 10000000000000000ULL;
			length += value >= 100000000000000000ULL;
			length += value >= 1000000000000000000ULL;
			length += value >= 10000000000000000000ULL;
			return length;
		} else {
			int length = 11;
			length += value >= 100000000000ULL;
			length += value >= 1000000000000ULL;
			length += value >= 10000000000000ULL;
			length += value >= 100000000000000ULL;
			return length;
		}
	} else {
		if (value >= 100000ULL) {
			int length = 6;
			length += value >= 1000000ULL;
			length += value >= 10000000ULL;
			length += value >= 100000000ULL;
			length += value >= 1000000000ULL;
			return length;
		} else {
			int length = 1;
			length += value >= 10ULL;
			length += value >= 100ULL;
			length += value >= 1000ULL;
			length += value >= 10000ULL;
			return length;
		}
	}
}

template <>
int NumericHelper::UnsignedLength(hugeint_t value) {
	D_ASSERT(value.upper >= 0);
	if (value.upper == 0) {
		return UnsignedLength<uint64_t>(value.lower);
	}
	// search the length using the POWERS_OF_TEN array
	// the length has to be between [17] and [38], because the hugeint is bigger than 2^63
	// we use the same approach as above, but split a bit more because comparisons for hugeints are more expensive
	if (value >= Hugeint::POWERS_OF_TEN[27]) {
		// [27..38]
		if (value >= Hugeint::POWERS_OF_TEN[32]) {
			if (value >= Hugeint::POWERS_OF_TEN[36]) {
				int length = 37;
				length += value >= Hugeint::POWERS_OF_TEN[37];
				length += value >= Hugeint::POWERS_OF_TEN[38];
				return length;
			} else {
				int length = 33;
				length += value >= Hugeint::POWERS_OF_TEN[33];
				length += value >= Hugeint::POWERS_OF_TEN[34];
				length += value >= Hugeint::POWERS_OF_TEN[35];
				return length;
			}
		} else {
			if (value >= Hugeint::POWERS_OF_TEN[30]) {
				int length = 31;
				length += value >= Hugeint::POWERS_OF_TEN[31];
				length += value >= Hugeint::POWERS_OF_TEN[32];
				return length;
			} else {
				int length = 28;
				length += value >= Hugeint::POWERS_OF_TEN[28];
				length += value >= Hugeint::POWERS_OF_TEN[29];
				return length;
			}
		}
	} else {
		// [17..27]
		if (value >= Hugeint::POWERS_OF_TEN[22]) {
			// [22..27]
			if (value >= Hugeint::POWERS_OF_TEN[25]) {
				int length = 26;
				length += value >= Hugeint::POWERS_OF_TEN[26];
				return length;
			} else {
				int length = 23;
				length += value >= Hugeint::POWERS_OF_TEN[23];
				length += value >= Hugeint::POWERS_OF_TEN[24];
				return length;
			}
		} else {
			// [17..22]
			if (value >= Hugeint::POWERS_OF_TEN[20]) {
				int length = 21;
				length += value >= Hugeint::POWERS_OF_TEN[21];
				return length;
			} else {
				int length = 18;
				length += value >= Hugeint::POWERS_OF_TEN[18];
				length += value >= Hugeint::POWERS_OF_TEN[19];
				return length;
			}
		}
	}
}

template <>
string_t NumericHelper::FormatSigned(hugeint_t value, StringHeap &heap) {
	int negative = value.upper < 0;
	if (negative) {
		if (value == NumericLimits<hugeint_t>::Minimum()) {
			string_t result = heap.AddString(Hugeint::HUGEINT_MINIMUM_STRING);
			return result;
		}
		Hugeint::NegateInPlace(value);
	}
	int length = UnsignedLength(value) + negative;
	string_t result = heap.EmptyString(NumericCast<size_t>(length));
	auto dataptr = result.GetDataWriteable();
	auto endptr = dataptr + length;
	if (value.upper == 0) {
		// small value: format as uint64_t
		endptr = NumericHelper::FormatUnsigned<uint64_t>(value.lower, endptr);
	} else {
		endptr = FormatUnsigned(value, endptr);
	}
	if (negative) {
		*--endptr = '-';
	}
	D_ASSERT(endptr == dataptr);
	result.Finalize();
	return result;
}

template <>
std::string NumericHelper::ToString(hugeint_t value) {
	return Hugeint::ToString(value);
}

template <>
std::string NumericHelper::ToString(uhugeint_t value) {
	return Uhugeint::ToString(value);
}

template <>
int DecimalToString::DecimalLength(hugeint_t value, uint32_t width, uint32_t scale) {
	D_ASSERT(value > NumericLimits<hugeint_t>::Minimum());
	int negative;

	if (value.upper < 0) {
		Hugeint::NegateInPlace(value);
		negative = 1;
	} else {
		negative = 0;
	}
	if (scale == 0) {
		// scale is 0: regular number
		return NumericHelper::UnsignedLength(value) + negative;
	}
	// length is max of either:
	// scale + 2 OR
	// integer length + 1
	// scale + 2 happens when the number is in the range of (-1, 1)
	// in that case we print "0.XXX", which is the scale, plus "0." (2 chars)
	// integer length + 1 happens when the number is outside of that range
	// in that case we print the integer number, but with one extra character ('.')
	auto extra_numbers = width > scale ? 2 : 1;
	return MaxValue(UnsafeNumericCast<int>(scale) + extra_numbers, NumericHelper::UnsignedLength(value) + 1) + negative;
}

template <>
string_t DecimalToString::Format(hugeint_t value, uint32_t width, uint32_t scale, StringHeap &heap) {
	int length = DecimalLength(value, width, scale);
	string_t result = heap.EmptyString(NumericCast<idx_t>(length));

	auto dst = result.GetDataWriteable();

	FormatDecimal(value, width, scale, dst, NumericCast<idx_t>(length));

	result.Finalize();
	return result;
}

template <>
char *NumericHelper::FormatUnsigned(hugeint_t value, char *ptr) {
	while (value.upper > 0) {
		// while integer division is slow, hugeint division is MEGA slow
		// we want to avoid doing as many divisions as possible
		// for that reason we start off doing a division by a large power of ten that uint64_t can hold
		// (100000000000000000) - this is the third largest
		// the reason we don't use the largest is because that can result in an overflow inside the division
		// function
		uint64_t remainder;
		value = Hugeint::DivModPositive(value, 100000000000000000ULL, remainder);

		auto startptr = ptr;
		// now we format the remainder: note that we need to pad with zero's in case
		// the remainder is small (i.e. less than 10000000000000000)
		ptr = NumericHelper::FormatUnsigned<uint64_t>(remainder, ptr);

		int format_length = UnsafeNumericCast<int>(startptr - ptr);
		// pad with zero
		for (int i = format_length; i < 17; i++) {
			*--ptr = '0';
		}
	}
	// once the value falls in the range of a uint64_t, fallback to formatting as uint64_t to avoid hugeint division
	return NumericHelper::FormatUnsigned<uint64_t>(value.lower, ptr);
}

template <>
void DecimalToString::FormatDecimal(hugeint_t value, uint32_t width, uint32_t scale, char *dst, idx_t len) {
	auto endptr = dst + len;

	int negative = value.upper < 0;
	if (negative) {
		Hugeint::NegateInPlace(value);
		*dst = '-';
		dst++;
	}
	if (scale == 0) {
		// with scale=0 we format the number as a regular number
		NumericHelper::FormatUnsigned(value, endptr);
		return;
	}

	// we write two numbers:
	// the numbers BEFORE the decimal (major)
	// and the numbers AFTER the decimal (minor)
	hugeint_t minor;
	hugeint_t major = Hugeint::DivMod(value, Hugeint::POWERS_OF_TEN[scale], minor);

	// write the number after the decimal
	dst = NumericHelper::FormatUnsigned(minor, endptr);
	// (optionally) pad with zeros and add the decimal point
	while (dst > (endptr - scale)) {
		*--dst = '0';
	}
	*--dst = '.';
	// now write the part before the decimal
	D_ASSERT(width > scale || major == 0);
	if (width > scale) {
		dst = NumericHelper::FormatUnsigned(major, dst);
	}
}

} // namespace duckdb
