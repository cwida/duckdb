//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/hugeint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//! The Hugeint class contains static operations for the INT128 type
class Hugeint {
public:
	constexpr static const char *HUGEINT_MINIMUM_STRING = "-170141183460469231731687303715884105728";

	//! Convert a hugeint object to a string
	static string ToString(hugeint_t input);

	template <class T>
	DUCKDB_API static bool TryCast(hugeint_t input, T &result);

	template <class T>
	static T Cast(hugeint_t input) {
		T result = 0;
		TryCast(input, result);
		return result;
	}

	template <class T>
	static bool TryConvert(T value, hugeint_t &result);

	template <class T>
	static hugeint_t Convert(T value) {
		hugeint_t result;
		if (!TryConvert(value, result)) { // LCOV_EXCL_START
			OutOfRangeException::ThrowConversion(double(value), GetTypeId<T>(), GetTypeId<hugeint_t>());
		} // LCOV_EXCL_STOP
		return result;
	}

	static bool TryNegate(hugeint_t input, hugeint_t &result);

	template <bool CHECK_OVERFLOW = true>
	inline static void NegateInPlace(hugeint_t &input) {
		if (!TryNegate(input, input)) {
			throw OutOfRangeException("Negation of HUGEINT is out of range!");
		}
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Negate(hugeint_t input) {
		NegateInPlace<CHECK_OVERFLOW>(input);
		return input;
	}

	static bool TryMultiply(hugeint_t lhs, hugeint_t rhs, hugeint_t &result);

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Multiply(hugeint_t lhs, hugeint_t rhs) {
		hugeint_t result;
		if (!TryMultiply(lhs, rhs, result)) {
			throw OutOfRangeException("Overflow in HUGEINT multiplication: %s * %s", lhs.ToString(), rhs.ToString());
		}
		return result;
	}

	static bool TryDivMod(hugeint_t lhs, hugeint_t rhs, hugeint_t &result, hugeint_t &remainder);

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Divide(hugeint_t lhs, hugeint_t rhs) {
		// No division by zero
		if (rhs == 0) {
			throw OutOfRangeException("Division of HUGEINT by zero: %s / %s", lhs.ToString(), rhs.ToString());
		}

		// division only has one reason to overflow: MINIMUM / -1
		if (lhs == NumericLimits<hugeint_t>::Minimum() && rhs == -1) {
			throw OutOfRangeException("Overflow in HUGEINT division: %s / %s", lhs.ToString(), rhs.ToString());
		}
		return Divide<false>(lhs, rhs);
	}

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Modulo(hugeint_t lhs, hugeint_t rhs) {
		// No division by zero
		if (rhs == 0) {
			throw OutOfRangeException("Modulo of HUGEINT by zero: %s %% %s", lhs.ToString(), rhs.ToString());
		}

		// division only has one reason to overflow: MINIMUM / -1
		if (lhs == NumericLimits<hugeint_t>::Minimum() && rhs == -1) {
			throw OutOfRangeException("Overflow in HUGEINT modulo: %s %% %s", lhs.ToString(), rhs.ToString());
		}
		return Modulo<false>(lhs, rhs);
	}

	static bool TryAddInPlace(hugeint_t &lhs, hugeint_t rhs);

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Add(hugeint_t lhs, hugeint_t rhs) {
		if (!TryAddInPlace(lhs, rhs)) {
			throw OutOfRangeException("Overflow in HUGEINT addition: %s + %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	static bool TrySubtractInPlace(hugeint_t &lhs, hugeint_t rhs);

	template <bool CHECK_OVERFLOW = true>
	inline static hugeint_t Subtract(hugeint_t lhs, hugeint_t rhs) {
		if (!TrySubtractInPlace(lhs, rhs)) {
			throw OutOfRangeException("Underflow in HUGEINT subtraction: %s - %s", lhs.ToString(), rhs.ToString());
		}
		return lhs;
	}

	// DivMod -> returns the result of the division (lhs / rhs), and fills up the remainder
	static hugeint_t DivMod(hugeint_t lhs, hugeint_t rhs, hugeint_t &remainder);
	// DivMod but lhs MUST be positive, and rhs is a uint64_t
	static hugeint_t DivModPositive(hugeint_t lhs, uint64_t rhs, uint64_t &remainder);
	//! lhs / rhs rounded to the nearest double (ties to even), the same on every platform: it does not go through
	//! long double, which is a double on some of them. rhs must not be zero.
	DUCKDB_API static double DivideToDouble(hugeint_t lhs, uint64_t rhs);
	DUCKDB_API static double DivideToDouble(hugeint_t lhs, int64_t rhs);

	static int Sign(hugeint_t n);
	static hugeint_t Abs(hugeint_t n);
	// comparison operators
	static bool Equals(const hugeint_t &lhs, const hugeint_t &rhs) {
		bool lower_equals = lhs.lower == rhs.lower;
		bool upper_equals = lhs.upper == rhs.upper;
		return lower_equals && upper_equals;
	}

	static bool NotEquals(const hugeint_t &lhs, const hugeint_t &rhs) {
		return !Equals(lhs, rhs);
	}

	static bool GreaterThan(const hugeint_t &lhs, const hugeint_t &rhs) {
		bool upper_bigger = lhs.upper > rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_bigger = lhs.lower > rhs.lower;
		return upper_bigger || (upper_equal && lower_bigger);
	}

	static bool GreaterThanEquals(const hugeint_t &lhs, const hugeint_t &rhs) {
		bool upper_bigger = lhs.upper > rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_bigger_equals = lhs.lower >= rhs.lower;
		return upper_bigger || (upper_equal && lower_bigger_equals);
	}

	static bool LessThan(const hugeint_t &lhs, const hugeint_t &rhs) {
		bool upper_smaller = lhs.upper < rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_smaller = lhs.lower < rhs.lower;
		return upper_smaller || (upper_equal && lower_smaller);
	}

	static bool LessThanEquals(const hugeint_t &lhs, const hugeint_t &rhs) {
		bool upper_smaller = lhs.upper < rhs.upper;
		bool upper_equal = lhs.upper == rhs.upper;
		bool lower_smaller_equals = lhs.lower <= rhs.lower;
		return upper_smaller || (upper_equal && lower_smaller_equals);
	}

	static constexpr uint8_t CACHED_POWERS_OF_TEN = 39;
	static const hugeint_t POWERS_OF_TEN[CACHED_POWERS_OF_TEN];
};

template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, int8_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, int16_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, int32_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, int64_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, uint8_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, uint16_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, uint32_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, uint64_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, hugeint_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, uhugeint_t &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, float &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, double &result);
template <>
DUCKDB_API bool Hugeint::TryCast(hugeint_t input, long double &result);

template <>
bool Hugeint::TryConvert(int8_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(int16_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(int32_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(int64_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(uint8_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(uint16_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(uint32_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(uint64_t value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(float value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(double value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(long double value, hugeint_t &result);
template <>
bool Hugeint::TryConvert(const char *value, hugeint_t &result);

} // namespace duckdb
