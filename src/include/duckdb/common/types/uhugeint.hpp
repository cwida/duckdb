//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/uhugeint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/uhugeint.hpp"

namespace duckdb {

//! The uhugeint class contains static operations for the UINT128 type
class Uhugeint {
public:
	//! Convert a uhugeint object to a string
	static string ToString(uhugeint_t input);

	template <class T>
	DUCKDB_API static bool TryCast(uhugeint_t input, T &result);

	template <class T>
	static T Cast(uhugeint_t input) {
		T result = 0;
		TryCast(input, result);
		return result;
	}

	template <class T>
	static bool TryConvert(T value, uhugeint_t &result);

	template <class T>
	static uhugeint_t Convert(T value) {
		uhugeint_t result;
		if (!TryConvert(value, result)) { // LCOV_EXCL_START
			OutOfRangeException::ThrowConversion(double(value), GetTypeId<T>(), GetTypeId<uhugeint_t>());
		} // LCOV_EXCL_STOP
		return result;
	}

	static bool TryNegate(uhugeint_t input, uhugeint_t &result);

	// "The negative of an unsigned quantity is computed by subtracting its value from 2^n, where n is the number of
	// bits in the promoted operand."
	template <bool CHECK_OVERFLOW = true>
	inline static void NegateInPlace(uhugeint_t &input) {
		if (!TryNegate(input, input)) {
			throw OutOfRangeException("UHUGEINT is out of range");
		}
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Negate(uhugeint_t input) {
		NegateInPlace<CHECK_OVERFLOW>(input);
		return input;
	}

	static bool TryMultiply(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &result);

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Multiply(uhugeint_t lhs, uhugeint_t rhs) {
		uhugeint_t result;
		if (!TryMultiply(lhs, rhs, result)) {
			throw OutOfRangeException("Overflow in UHUGEINT multiplication: %s * %s", lhs.ToString(), rhs.ToString());
		}
		return result;
	}

	static bool TryDivMod(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &result, uhugeint_t &remainder);

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Divide(uhugeint_t lhs, uhugeint_t rhs) {
		// division between two same-size unsigned integers can only go wrong with division by zero
		if (rhs == 0) {
			throw OutOfRangeException("Division of UHUGEINT by zero: %s / %s", lhs.ToString(), rhs.ToString());
		}
		return Divide<false>(lhs, rhs);
	}

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Modulo(uhugeint_t lhs, uhugeint_t rhs) {
		if (rhs == 0) {
			throw OutOfRangeException("Modulo of UHUGEINT by zero: %s %% %s", lhs.ToString(), rhs.ToString());
		}
		return Modulo<false>(lhs, rhs);
	}

	static bool TryAddInPlace(uhugeint_t &lhs, uhugeint_t rhs);

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Add(uhugeint_t lhs, uhugeint_t rhs) {
		if (!TryAddInPlace(lhs, rhs)) {
			throw OutOfRangeException("Overflow in UHUGEINT addition: %s + %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	static bool TrySubtractInPlace(uhugeint_t &lhs, uhugeint_t rhs);

	template <bool CHECK_OVERFLOW = true>
	inline static uhugeint_t Subtract(uhugeint_t lhs, uhugeint_t rhs) {
		if (!TrySubtractInPlace(lhs, rhs)) {
			throw OutOfRangeException("Underflow in HUGEINT subtraction: %s - %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	// DivMod -> returns the result of the division (lhs / rhs), and fills up the remainder
	static uhugeint_t DivMod(uhugeint_t lhs, uhugeint_t rhs, uhugeint_t &remainder);

	static int Sign(hugeint_t n);
	static hugeint_t Abs(hugeint_t n);

	// comparison operators
	static bool Equals(uhugeint_t lhs, uhugeint_t rhs) {
		bool lower_equals = lhs.lower == rhs.lower;
		bool upper_equals = lhs.upper == rhs.upper;
		return lower_equals && upper_equals;
	}

	static bool NotEquals(uhugeint_t lhs, uhugeint_t rhs) {
		return !Equals(lhs, rhs);
	}

	static bool GreaterThan(uhugeint_t lhs, uhugeint_t rhs) {
		bool upper_bigger = lhs.upper > rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_bigger = lhs.lower > rhs.lower;
		return upper_bigger || (upper_equal && lower_bigger);
	}

	static bool GreaterThanEquals(uhugeint_t lhs, uhugeint_t rhs) {
		bool upper_bigger = lhs.upper > rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_bigger_equals = lhs.lower >= rhs.lower;
		return upper_bigger || (upper_equal && lower_bigger_equals);
	}

	static bool LessThan(uhugeint_t lhs, uhugeint_t rhs) {
		bool upper_smaller = lhs.upper < rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_smaller = lhs.lower < rhs.lower;
		return upper_smaller || (upper_equal && lower_smaller);
	}

	static bool LessThanEquals(uhugeint_t lhs, uhugeint_t rhs) {
		bool upper_smaller = lhs.upper < rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_smaller_equals = lhs.lower <= rhs.lower;
		return upper_smaller || (upper_equal && lower_smaller_equals);
	}

	static constexpr uint8_t CACHED_POWERS_OF_TEN = 39;
	static const uhugeint_t POWERS_OF_TEN[CACHED_POWERS_OF_TEN];
};

template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, int8_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, int16_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, int32_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, int64_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, uint8_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, uint16_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, uint32_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, uint64_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, hugeint_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, uhugeint_t &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, float &result);
template <>
DUCKDB_API bool Uhugeint::TryCast(uhugeint_t input, double &result);

template <>
bool Uhugeint::TryConvert(int8_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(int16_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(int32_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(int64_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(uint8_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(uint16_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(uint32_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(uint64_t value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(float value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(double value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(long double value, uhugeint_t &result);
template <>
bool Uhugeint::TryConvert(const char *value, uhugeint_t &result);

} // namespace duckdb
