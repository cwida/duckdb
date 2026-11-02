//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/decimal_cast_operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Decimal Casts
//===--------------------------------------------------------------------===//
struct TryCastToDecimal {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
		NotImplementedException::Throw("Unimplemented type for TryCastToDecimal!");
	}
};

struct TryCastToDecimalCommaSeparated {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
		NotImplementedException::Throw("Unimplemented type for TryCastToDecimal!");
	}
};

struct TryCastFromDecimal {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
		NotImplementedException::Throw("Unimplemented type for TryCastFromDecimal!");
	}
};

//===--------------------------------------------------------------------===//
// Cast Decimal <-> bool
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(bool input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int8_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int8_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int16_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int16_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int32_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int32_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> int64_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(int64_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> hugeint_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(hugeint_t input, hugeint_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uhugeint_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, int16_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, int32_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, int64_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uhugeint_t input, hugeint_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint8_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint8_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint16_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint16_t input, hugeint_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint32_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint32_t input, hugeint_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> uint64_t
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(uint64_t input, hugeint_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> float
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(float input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> double
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(double input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);

template <>
bool TryCastFromDecimal::Operation(int16_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int32_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(int64_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale);

//===--------------------------------------------------------------------===//
// Cast Decimal <-> VARCHAR
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                            uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimal::Operation(string_t input, hugeint_t &result, CastParameters &parameters,
                                            uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, int16_t &result, CastParameters &parameters,
                                                          uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, int32_t &result, CastParameters &parameters,
                                                          uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, int64_t &result, CastParameters &parameters,
                                                          uint8_t width, uint8_t scale);
template <>
DUCKDB_API bool TryCastToDecimalCommaSeparated::Operation(string_t input, hugeint_t &result, CastParameters &parameters,
                                                          uint8_t width, uint8_t scale);

struct StringCastFromDecimal {
	template <class SRC>
	static inline string_t Operation(SRC input, uint8_t width, uint8_t scale, StringHeap &heap) {
		NotImplementedException::Throw("Unimplemented type for string cast!");
	}
};

template <>
string_t StringCastFromDecimal::Operation(int16_t input, uint8_t width, uint8_t scale, StringHeap &heap);
template <>
string_t StringCastFromDecimal::Operation(int32_t input, uint8_t width, uint8_t scale, StringHeap &heap);
template <>
string_t StringCastFromDecimal::Operation(int64_t input, uint8_t width, uint8_t scale, StringHeap &heap);
template <>
string_t StringCastFromDecimal::Operation(hugeint_t input, uint8_t width, uint8_t scale, StringHeap &heap);

//===--------------------------------------------------------------------===//
// Cast VARCHAR <-> Decimal
//===--------------------------------------------------------------------===//
enum class ExponentType : uint8_t { NONE, POSITIVE, NEGATIVE };

template <typename T>
struct DecimalCastTraits {
	using POWERS_OF_TEN_CLASS = NumericHelper;
};

template <>
struct DecimalCastTraits<hugeint_t> {
	using POWERS_OF_TEN_CLASS = Hugeint;
};

template <>
struct DecimalCastTraits<uhugeint_t> {
	using POWERS_OF_TEN_CLASS = Uhugeint;
};

template <class T>
struct DecimalCastData {
	using StoreType = T;
	StoreType result;
	uint8_t width;
	uint8_t scale;
	uint8_t digit_count;
	uint8_t decimal_count;
	//! Whether we have determined if the result should be rounded
	bool round_set;
	//! If the result should be rounded
	bool should_round;
	//! Only set when ALLOW_EXPONENT is enabled
	uint8_t excessive_decimals;
	ExponentType exponent_type;
	StoreType limit;
};

//! StoreType max/min divided by 10, for the overflow check in DecimalCastOperation::HandleDigit.
//! These are forced to be compile-time constants so the division is folded away: with a hugeint_t
//! store type it is a 128-bit division that would otherwise run for every digit. hugeint_t::operator/
//! is constexpr, but it is too large to be inlined at the call site, so the compiler does not fold it
//! unless constant evaluation is actually required.
template <class T>
struct DecimalCastLimits {
	static constexpr T MaximumDiv10 = NumericLimits<T>::Maximum() / 10;
	static constexpr T MinimumDiv10 = NumericLimits<T>::Minimum() / 10;
};

struct DecimalCastOperation {
	template <class T, bool NEGATIVE>
	static bool HandleDigit(T &state, uint8_t digit) {
		if (state.result == 0 && digit == 0) {
			// leading zero's don't count towards the digit count
			return true;
		}
		if (state.digit_count == state.width - state.scale) {
			// width of decimal type is exceeded!
			return false;
		}
		state.digit_count++;
		if (NEGATIVE) {
			if (state.result < DecimalCastLimits<typename T::StoreType>::MinimumDiv10) {
				return false;
			}
			state.result = state.result * 10 - digit;
		} else {
			if (state.result > DecimalCastLimits<typename T::StoreType>::MaximumDiv10) {
				return false;
			}
			state.result = state.result * 10 + digit;
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleHexDigit(T &state, uint8_t digit) {
		return false;
	}

	template <class T, bool NEGATIVE>
	static bool HandleBinaryDigit(T &state, uint8_t digit) {
		return false;
	}

	template <class T, bool NEGATIVE>
	static void RoundUpResult(T &state) {
		if (NEGATIVE) {
			state.result -= 1;
		} else {
			state.result += 1;
		}
	}

	template <class T, bool NEGATIVE>
	static bool HandleExponent(T &state, int32_t exponent) {
		auto decimal_excess = (state.decimal_count > state.scale) ? state.decimal_count - state.scale : 0;
		if (exponent > 0) {
			state.exponent_type = ExponentType::POSITIVE;
			// Positive exponents need up to 'exponent' amount of digits
			// Everything beyond that amount needs to be truncated
			if (decimal_excess > exponent) {
				// We've allowed too many decimals
				state.excessive_decimals = UnsafeNumericCast<uint8_t>(decimal_excess - exponent);
				exponent = 0;
			} else {
				exponent -= decimal_excess;
			}
			D_ASSERT(exponent >= 0);
		} else if (exponent < 0) {
			state.exponent_type = ExponentType::NEGATIVE;
		}
		if (!Finalize<T, NEGATIVE>(state)) {
			return false;
		}
		if (exponent < 0) {
			bool round_up = false;
			for (idx_t i = 0; i < idx_t(-int64_t(exponent)); i++) {
				auto mod = state.result % 10;
				round_up = NEGATIVE ? mod <= -5 : mod >= 5;
				state.result /= 10;
				if (state.result == 0 && mod == 0) {
					break;
				}
			}
			if (round_up) {
				RoundUpResult<T, NEGATIVE>(state);
			}
			return true;
		} else {
			// positive exponent: append 0's
			for (idx_t i = 0; i < idx_t(exponent); i++) {
				if (!HandleDigit<T, NEGATIVE>(state, 0)) {
					return false;
				}
			}
			// decimals moved before the decimal point by the exponent do not count towards the digit count
			return WithinLimit<T, NEGATIVE>(state);
		}
	}

	template <class T, bool NEGATIVE, bool ALLOW_EXPONENT>
	static bool HandleDecimal(T &state, uint8_t digit) {
		if (state.decimal_count == state.scale && !state.round_set) {
			// Determine whether the last registered decimal should be rounded or not
			state.round_set = true;
			state.should_round = digit >= 5;
		}
		if (!ALLOW_EXPONENT && state.decimal_count == state.scale) {
			// we exceeded the amount of supported decimals
			// however, we don't throw an error here
			// we just truncate the decimal
			return true;
		}
		//! If we expect an exponent, we need to preserve the decimals
		//! But we don't want to overflow, so we prevent overflowing the result with this check
		if (state.digit_count + state.decimal_count >= DecimalWidth<decltype(state.result)>::max) {
			return true;
		}
		state.decimal_count++;
		if (NEGATIVE) {
			state.result = state.result * 10 - digit;
		} else {
			state.result = state.result * 10 + digit;
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool TruncateExcessiveDecimals(T &state) {
		D_ASSERT(state.excessive_decimals);
		bool round_up = false;
		for (idx_t i = 0; i < state.excessive_decimals; i++) {
			auto mod = state.result % 10;
			round_up = NEGATIVE ? mod <= -5 : mod >= 5;
			state.result /= static_cast<typename T::StoreType>(10.0);
		}
		//! Only round up when exponents are involved
		if (state.exponent_type == ExponentType::POSITIVE && round_up) {
			RoundUpResult<T, NEGATIVE>(state);
		}
		D_ASSERT(state.decimal_count > state.scale);
		state.decimal_count = state.scale;
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool Finalize(T &state) {
		if (state.exponent_type != ExponentType::POSITIVE && state.decimal_count > state.scale) {
			//! Did not encounter an exponent, but ALLOW_EXPONENT was on
			state.excessive_decimals = state.decimal_count - state.scale;
		}
		if (state.excessive_decimals && !TruncateExcessiveDecimals<T, NEGATIVE>(state)) {
			return false;
		}
		if (state.exponent_type == ExponentType::NONE && state.round_set && state.should_round) {
			RoundUpResult<T, NEGATIVE>(state);
		}
		//  if we have not gotten exactly "scale" decimals, we need to multiply the result
		//  e.g. if we have a string "1.0" that is cast to a DECIMAL(9,3), the value needs to be 1000
		//  but we have only gotten the value "10" so far, so we multiply by 1000
		for (uint8_t i = state.decimal_count; i < state.scale; i++) {
			state.result *= 10;
		}
		return WithinLimit<T, NEGATIVE>(state);
	}

	template <class T, bool NEGATIVE>
	static bool WithinLimit(T &state) {
		if (NEGATIVE) {
			return state.result > -state.limit;
		} else {
			return state.result < state.limit;
		}
	}
};

template <class T, char decimal_separator = '.'>
bool TryDecimalStringCast(string_t input, T &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	return TryDecimalStringCast<T, decimal_separator>(input.GetData(), input.GetSize(), result, parameters, width,
	                                                  scale);
}

template <class T, char decimal_separator = '.'>
bool TryDecimalStringCast(const char *string_ptr, idx_t string_size, T &result, CastParameters &parameters,
                          uint8_t width, uint8_t scale) {
	DecimalCastData<T> state;
	state.result = 0;
	state.width = width;
	state.scale = scale;
	state.digit_count = 0;
	state.decimal_count = 0;
	state.excessive_decimals = 0;
	state.exponent_type = ExponentType::NONE;
	state.round_set = false;
	state.should_round = false;
	state.limit = UnsafeNumericCast<T>(DecimalCastTraits<T>::POWERS_OF_TEN_CLASS::POWERS_OF_TEN[width]);
	if (!TryIntegerCast<DecimalCastData<T>, true, true, DecimalCastOperation, false, decimal_separator>(
	        string_ptr, string_size, state, false)) {
		string_t value(string_ptr, (uint32_t)string_size);
		string error = StringUtil::Format("Could not convert string \"%s\" to DECIMAL(%d,%d)", value.GetString(),
		                                  (int)width, (int)scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = state.result;
	return true;
}

template <class T, char decimal_separator = '.'>
bool TryDecimalStringCast(const char *string_ptr, idx_t string_size, T &result, uint8_t width, uint8_t scale) {
	DecimalCastData<T> state;
	state.result = 0;
	state.width = width;
	state.scale = scale;
	state.digit_count = 0;
	state.decimal_count = 0;
	state.excessive_decimals = 0;
	state.exponent_type = ExponentType::NONE;
	state.round_set = false;
	state.should_round = false;
	state.limit = UnsafeNumericCast<T>(DecimalCastTraits<T>::POWERS_OF_TEN_CLASS::POWERS_OF_TEN[width]);
	if (!TryIntegerCast<DecimalCastData<T>, true, true, DecimalCastOperation, false, decimal_separator>(
	        string_ptr, string_size, state, false)) {
		return false;
	}
	result = state.result;
	return true;
}

} // namespace duckdb
