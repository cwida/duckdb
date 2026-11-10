//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/decimal_helper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/bignum_decimal.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace duckdb {
class Vector;

//! The operations on the unscaled value of a fixed-size decimal tier (int16_t, int32_t, int64_t or hugeint_t), the
//! interface DecimalHelper<bignum_t> offers for the decimals wider than 38 digits
template <class T, class POWERS_OF_TEN_CLASS>
struct FixedSizeDecimalHelper {
	//! An owned result is the value itself in these tiers (a BIGNUM blob in the bignum_t tier)
	using OWNED = T;
	static constexpr bool IS_WIDE = false;
	static constexpr uint32_t MAX_WIDTH = DecimalWidth<T>::max;

	//! The scratch of TryRound, nothing in these tiers
	struct RoundState {
		RoundState(Vector &result, optional_ptr<ClientContext> context) {
		}
	};

	//! 10^exponent, which is at most 10^MAX_WIDTH
	static T PowerOfTen(idx_t exponent) {
		D_ASSERT(exponent <= MAX_WIDTH);
		return UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[exponent]);
	}
	//! Whether |value| < 10^width, i.e. the unscaled value fits a DECIMAL of the given width
	static bool FitsWidth(T value, uint32_t width) {
		if (width > MAX_WIDTH) {
			return true;
		}
		const T limit = PowerOfTen(width);
		return value > -limit && value < limit;
	}
	static bool IsZero(T value) {
		return value == 0;
	}
	static bool IsNegative(T value) {
		return value < 0;
	}
	//! The value as this tier stores it, and the value stored in a vector: the integer itself in these tiers
	static T Load(const OWNED &owned) {
		return owned;
	}
	static T Store(Vector &result, const OWNED &owned) {
		return owned;
	}
	//! The unscaled value of an integer that fits the tier
	static OWNED FromInteger(hugeint_t value) {
		if constexpr (std::is_same<T, hugeint_t>::value) {
			return value;
		} else {
			return Hugeint::Cast<T>(value);
		}
	}
	static bool TryToHugeint(T value, hugeint_t &result) {
		result = FromIntegerToHugeint(value);
		return true;
	}

	//! Formats an unscaled value with the given width and scale, e.g. 12345 with scale 2 -> "123.45"
	static string ToString(T value, uint32_t width, uint32_t scale, optional_ptr<ClientContext> context = nullptr) {
		return Decimal::ToString(value, width, scale);
	}
	//! Parses a decimal string into the unscaled value of a DECIMAL(width, scale), rounding half away from zero
	DUCKDB_API static bool TryParse(const char *data, idx_t size, uint32_t width, uint32_t scale, OWNED &result,
	                                char decimal_separator = '.', optional_ptr<ClientContext> context = nullptr);

	//! Drops the digits below power_of_ten (at least ten) with the rounding mode
	template <DecimalRounding ROUNDING>
	static T RoundDroppedDigits(T value, T power_of_ten) {
		D_ASSERT(power_of_ten >= 10);
		T quotient;
		T remainder;
		if constexpr (std::is_same<T, hugeint_t>::value) {
			// hugeint division and modulo both run a full DivMod and discard half of the result
			quotient = Hugeint::DivMod(value, power_of_ten, remainder);
		} else {
			quotient = UnsafeNumericCast<T>(value / power_of_ten);
			remainder = UnsafeNumericCast<T>(value % power_of_ten);
		}
		if (remainder == 0) {
			return quotient;
		}
		// the quotient is truncated towards zero, the mode decides whether it moves one away from zero
		const bool negative = value < 0;
		bool away;
		switch (ROUNDING) {
		case DecimalRounding::TRUNCATE:
			away = false;
			break;
		case DecimalRounding::FLOOR:
			away = negative;
			break;
		case DecimalRounding::CEIL:
			away = !negative;
			break;
		default: {
			const T magnitude = UnsafeNumericCast<T>(negative ? -remainder : remainder);
			const T half = UnsafeNumericCast<T>(power_of_ten / 2);
			away = magnitude > half ||
			       (magnitude == half && (ROUNDING == DecimalRounding::HALF_AWAY_FROM_ZERO || quotient % 2 != 0));
			break;
		}
		}
		if (away) {
			quotient = UnsafeNumericCast<T>(negative ? quotient - 1 : quotient + 1);
		}
		return quotient;
	}
	//! Drops drop_digits digits of an unscaled value with the rounding and appends restore_digits zero digits, i.e.
	//! rounds the value at a digit position; false if the result does not fit the width
	template <DecimalRounding ROUNDING>
	static bool TryRound(T value, idx_t drop_digits, idx_t restore_digits, uint32_t width, RoundState &state,
	                     T &result) {
		T rounded = drop_digits > 0 ? RoundDroppedDigits<ROUNDING>(value, PowerOfTen(drop_digits)) : value;
		if (restore_digits > 0) {
			rounded = UnsafeNumericCast<T>(rounded * PowerOfTen(restore_digits));
		}
		result = rounded;
		return FitsWidth(rounded, width);
	}
	//! Rescales an unscaled value to target_scale (rounding half away from zero) and checks it fits target_width
	static bool TryRescale(T value, uint32_t source_scale, uint32_t target_width, uint32_t target_scale, OWNED &result,
	                       optional_ptr<ClientContext> context = nullptr) {
		D_ASSERT(target_width <= MAX_WIDTH && target_scale <= target_width);
		if (target_scale >= source_scale) {
			// the product fits the width when the value fits the width without the appended digits
			const idx_t shift = target_scale - source_scale;
			if (!FitsWidth(value, target_width - shift)) {
				return false;
			}
			result = UnsafeNumericCast<T>(value * PowerOfTen(shift));
			return true;
		}
		result =
		    RoundDroppedDigits<DecimalRounding::HALF_AWAY_FROM_ZERO>(value, PowerOfTen(source_scale - target_scale));
		return FitsWidth(result, target_width);
	}

	//! Converts an unscaled value with the given scale to a FLOAT or DOUBLE, which these tiers always fit
	DUCKDB_API static bool TryToFloating(T value, uint32_t scale, float &result,
	                                     optional_ptr<ClientContext> context = nullptr);
	DUCKDB_API static bool TryToFloating(T value, uint32_t scale, double &result,
	                                     optional_ptr<ClientContext> context = nullptr);
	static double ToDouble(T value, uint32_t scale, optional_ptr<ClientContext> context = nullptr) {
		double result;
		TryToFloating(value, scale, result, context);
		return result;
	}
	//! Converts a double to the unscaled value of a DECIMAL(width, scale), false if it does not fit
	DUCKDB_API static bool TryFromDouble(double value, uint32_t width, uint32_t scale, OWNED &result,
	                                     optional_ptr<ClientContext> context = nullptr);

private:
	static hugeint_t FromIntegerToHugeint(T value) {
		if constexpr (std::is_same<T, hugeint_t>::value) {
			return value;
		} else {
			return Hugeint::Convert(value);
		}
	}
};

template <>
struct DecimalHelper<int16_t> : FixedSizeDecimalHelper<int16_t, NumericHelper> {};
template <>
struct DecimalHelper<int32_t> : FixedSizeDecimalHelper<int32_t, NumericHelper> {};
template <>
struct DecimalHelper<int64_t> : FixedSizeDecimalHelper<int64_t, NumericHelper> {};
template <>
struct DecimalHelper<hugeint_t> : FixedSizeDecimalHelper<hugeint_t, Hugeint> {};

} // namespace duckdb
