//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/integer_cast_operator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

namespace duckdb {
template <typename T>
struct IntegerCastData {
	using ResultType = T;
	using StoreType = T;
	ResultType result;
};

struct IntegerCastOperation {
	template <class T, bool NEGATIVE>
	static bool HandleDigit(T &state, uint8_t digit) {
		using store_t = typename T::StoreType;
		if (NEGATIVE) {
			if (DUCKDB_UNLIKELY(state.result < (NumericLimits<store_t>::Minimum() + digit) / 10)) {
				return false;
			}
			state.result = UnsafeNumericCast<store_t>(state.result * 10 - digit);
		} else {
			if (DUCKDB_UNLIKELY(state.result > (NumericLimits<store_t>::Maximum() - digit) / 10)) {
				return false;
			}
			state.result = UnsafeNumericCast<store_t>(state.result * 10 + digit);
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleHexDigit(T &state, uint8_t digit) {
		using store_t = typename T::StoreType;
		if (DUCKDB_UNLIKELY(state.result > (NumericLimits<store_t>::Maximum() - digit) / 16)) {
			return false;
		}
		state.result = UnsafeNumericCast<store_t>(state.result * 16 + digit);
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleBinaryDigit(T &state, uint8_t digit) {
		using store_t = typename T::StoreType;
		if (DUCKDB_UNLIKELY(state.result > (NumericLimits<store_t>::Maximum() - digit) / 2)) {
			return false;
		}
		state.result = UnsafeNumericCast<store_t>(state.result * 2 + digit);
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleExponent(T &state, int16_t exponent) {
		// Simple integers don't deal with Exponents
		return false;
	}

	template <class T, bool NEGATIVE, bool ALLOW_EXPONENT>
	static bool HandleDecimal(T &state, uint8_t digit) {
		// Simple integers don't deal with Decimals
		return false;
	}

	template <class T, bool NEGATIVE>
	static bool Finalize(T &state) {
		return true;
	}
};

template <typename T>
struct IntegerDecimalCastData {
	using ResultType = T;
	using StoreType = int64_t;
	StoreType result;
	StoreType decimal;
	idx_t decimal_digits;
};

template <>
struct IntegerDecimalCastData<uint64_t> {
	using ResultType = uint64_t;
	using StoreType = uint64_t;
	StoreType result;
	StoreType decimal;
	idx_t decimal_digits;
};

struct IntegerDecimalCastOperation : IntegerCastOperation {
	template <class T, bool NEGATIVE>
	static bool HandleExponent(T &state, int16_t exponent) {
		using store_t = typename T::StoreType;

		int16_t e = exponent;
		// Negative Exponent
		if (e < 0) {
			while (e++ < 0) {
				state.decimal = state.result % 10;
				state.result /= 10;
				if (state.result == 0 && state.decimal == 0) {
					break;
				}
			}
			if (state.decimal < 0) {
				state.decimal = -state.decimal;
			}
			state.decimal_digits = 1;
			return Finalize<T, NEGATIVE>(state);
		}

		// Positive Exponent
		while (state.result != 0 && e-- > 0) {
			if (!TryMultiplyOperator::Operation(state.result, (store_t)10, state.result)) {
				return false;
			}
		}

		if (state.decimal == 0) {
			return Finalize<T, NEGATIVE>(state);
		}

		// Handle decimals
		auto shift = static_cast<int64_t>(exponent) - static_cast<int64_t>(state.decimal_digits);
		store_t remainder = 0;
		if (shift < 0) {
			if (static_cast<uint64_t>(-shift) < NumericLimits<store_t>::Digits()) {
				store_t power = 1;
				while (shift++ < 0) {
					power *= 10;
				}
				remainder = state.decimal % power;
				state.decimal /= power;
			} else {
				// power would overflow and exceed decimal: all of decimal is fractional
				remainder = state.decimal;
				state.decimal = 0;
			}
		} else {
			while (shift-- > 0) {
				if (!TryMultiplyOperator::Operation(state.decimal, (store_t)10, state.decimal)) {
					return false;
				}
			}
		}

		state.decimal_digits -= static_cast<idx_t>(exponent);

		if (NEGATIVE) {
			if (!TrySubtractOperator::Operation(state.result, state.decimal, state.result)) {
				return false;
			}
		} else if (!TryAddOperator::Operation(state.result, state.decimal, state.result)) {
			return false;
		}
		state.decimal = remainder;
		return Finalize<T, NEGATIVE>(state);
	}

	template <class T, bool NEGATIVE, bool ALLOW_EXPONENT>
	static bool HandleDecimal(T &state, uint8_t digit) {
		using store_t = typename T::StoreType;
		// digits past this position cannot affect the result for any int16_t exponent
		static constexpr uint16_t MAX_DECIMAL_DIGITS = -NumericLimits<int16_t>::Minimum();
		if (DUCKDB_UNLIKELY(state.decimal_digits == MAX_DECIMAL_DIGITS ||
		                    state.decimal > (NumericLimits<store_t>::Maximum() - digit) / 10)) {
			// Simply ignore any more decimals
			return true;
		}
		state.decimal_digits++;
		state.decimal = state.decimal * 10 + digit;
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool Finalize(T &state) {
		using result_t = typename T::ResultType;
		using store_t = typename T::StoreType;

		result_t tmp;
		if (!TryCast::Operation<store_t, result_t>(state.result, tmp)) {
			return false;
		}

		while (state.decimal > 10) {
			state.decimal /= 10;
			state.decimal_digits--;
		}

		bool success = true;
		if (state.decimal_digits == 1 && state.decimal >= 5) {
			if (NEGATIVE) {
				success = TrySubtractOperator::Operation(tmp, (result_t)1, tmp);
			} else {
				success = TryAddOperator::Operation(tmp, (result_t)1, tmp);
			}
		}
		state.result = tmp;
		return success;
	}
};

template <class T, bool NEGATIVE, bool ALLOW_EXPONENT, class OP = IntegerCastOperation, char decimal_separator = '.'>
static bool IntegerCastLoop(const char *buf, idx_t len, T &result, bool strict) {
	idx_t start_pos;
	if (NEGATIVE) {
		start_pos = 1;
	} else {
		if (*buf == '+') {
			if (strict) {
				// leading plus is not allowed in strict mode
				return false;
			}
			start_pos = 1;
		} else {
			start_pos = 0;
		}
	}
	idx_t pos = start_pos;
	while (pos < len) {
		if (!StringUtil::CharacterIsDigit(buf[pos])) {
			// not a digit!
			if (buf[pos] == decimal_separator) {
				if (strict) {
					return false;
				}
				bool number_before_period = pos > start_pos;
				// decimal point: we accept decimal values for integers as well
				// we just truncate them
				// make sure everything after the period is a number
				pos++;
				idx_t start_digit = pos;
				while (pos < len) {
					if (!StringUtil::CharacterIsDigit(buf[pos])) {
						break;
					}
					if (!OP::template HandleDecimal<T, NEGATIVE, ALLOW_EXPONENT>(
					        result, UnsafeNumericCast<uint8_t>(buf[pos] - '0'))) {
						return false;
					}
					pos++;

					if (pos != len && buf[pos] == '_') {
						// Skip one underscore if it is not the last character and followed by a digit
						pos++;
						if (pos == len || !StringUtil::CharacterIsDigit(buf[pos])) {
							return false;
						}
					}
				}
				// make sure there is either (1) one number after the period, or (2) one number before the period
				// i.e. we accept "1." and ".1" as valid numbers, but not "."
				if (!(number_before_period || pos > start_digit)) {
					return false;
				}
				if (pos >= len) {
					break;
				}
			}
			if (StringUtil::CharacterIsSpace(buf[pos])) {
				if (pos == start_pos) {
					// a sign followed only by whitespace is not a number
					return false;
				}
				// skip any trailing spaces
				while (++pos < len) {
					if (!StringUtil::CharacterIsSpace(buf[pos])) {
						return false;
					}
				}
				break;
			}
			if (ALLOW_EXPONENT) {
				if (buf[pos] == 'e' || buf[pos] == 'E') {
					if (strict) {
						return false;
					}
					if (pos == start_pos) {
						return false;
					}
					pos++;
					if (pos >= len) {
						return false;
					}
					using ExponentData = IntegerCastData<int16_t>;
					ExponentData exponent {};
					int negative = buf[pos] == '-';
					if (negative) {
						if (!IntegerCastLoop<ExponentData, true, false, IntegerCastOperation, decimal_separator>(
						        buf + pos, len - pos, exponent, strict)) {
							return false;
						}
					} else {
						if (!IntegerCastLoop<ExponentData, false, false, IntegerCastOperation, decimal_separator>(
						        buf + pos, len - pos, exponent, strict)) {
							return false;
						}
					}
					return OP::template HandleExponent<T, NEGATIVE>(result, exponent.result);
				}
			}
			return false;
		}
		auto digit = UnsafeNumericCast<uint8_t>(buf[pos++] - '0');
		if (!OP::template HandleDigit<T, NEGATIVE>(result, digit)) {
			return false;
		}

		if (pos != len && buf[pos] == '_' && !strict) {
			// Skip one underscore if it is not the last character and followed by a digit
			pos++;
			if (pos == len || !StringUtil::CharacterIsDigit(buf[pos])) {
				return false;
			}
		}
	}
	if (!OP::template Finalize<T, NEGATIVE>(result)) {
		return false;
	}
	return pos > start_pos;
}

template <class T, bool NEGATIVE, bool ALLOW_EXPONENT, class OP = IntegerCastOperation>
static bool IntegerHexCastLoop(const char *buf, idx_t len, T &result, bool strict) {
	if (ALLOW_EXPONENT || NEGATIVE) {
		return false;
	}
	idx_t start_pos = 1;
	idx_t pos = start_pos;
	char current_char;
	while (pos < len) {
		current_char = StringUtil::CharacterToLower(buf[pos]);
		if (!StringUtil::CharacterIsHex(current_char)) {
			return false;
		}
		uint8_t digit;
		if (current_char >= 'a') {
			digit = UnsafeNumericCast<uint8_t>(current_char - 'a' + 10);
		} else {
			digit = UnsafeNumericCast<uint8_t>(current_char - '0');
		}
		pos++;

		if (pos != len && buf[pos] == '_') {
			// Skip one underscore if it is not the last character and followed by a hex
			pos++;
			if (pos == len || !StringUtil::CharacterIsHex(buf[pos])) {
				return false;
			}
		}

		if (!OP::template HandleHexDigit<T, NEGATIVE>(result, digit)) {
			return false;
		}
	}
	if (!OP::template Finalize<T, NEGATIVE>(result)) {
		return false;
	}
	return pos > start_pos;
}

template <class T, bool NEGATIVE, bool ALLOW_EXPONENT, class OP = IntegerCastOperation>
static bool IntegerBinaryCastLoop(const char *buf, idx_t len, T &result, bool strict) {
	if (ALLOW_EXPONENT || NEGATIVE) {
		return false;
	}
	idx_t start_pos = 1;
	idx_t pos = start_pos;
	uint8_t digit;
	char current_char;
	while (pos < len) {
		current_char = buf[pos];
		if (current_char == '0') {
			digit = 0;
		} else if (current_char == '1') {
			digit = 1;
		} else {
			return false;
		}
		pos++;
		if (pos != len && buf[pos] == '_') {
			// Skip one underscore if it is not the last character and followed by a digit
			pos++;
			if (pos == len || (buf[pos] != '0' && buf[pos] != '1')) {
				return false;
			}
		}

		if (!OP::template HandleBinaryDigit<T, NEGATIVE>(result, digit)) {
			return false;
		}
	}
	if (!OP::template Finalize<T, NEGATIVE>(result)) {
		return false;
	}
	return pos > start_pos;
}

template <class T, bool IS_SIGNED = true, bool ALLOW_EXPONENT = true, class OP = IntegerCastOperation,
          bool ZERO_INITIALIZE = true, char decimal_separator = '.'>
static bool TryIntegerCast(const char *buf, idx_t len, T &result, bool strict) {
	// skip any spaces at the start
	while (len > 0 && StringUtil::CharacterIsSpace(*buf)) {
		buf++;
		len--;
	}
	if (len == 0) {
		return false;
	}
	if (ZERO_INITIALIZE) {
		memset(&result, 0, sizeof(T));
	}
	// if the number is negative, we set the negative flag and skip the negative sign
	if (*buf == '-') {
		if (!IS_SIGNED) {
			// Need to check if its not -0
			idx_t pos = 1;
			while (pos < len) {
				if (buf[pos++] != '0') {
					return false;
				}
			}
		}
		return IntegerCastLoop<T, true, ALLOW_EXPONENT, OP, decimal_separator>(buf, len, result, strict);
	}
	if (len > 1 && *buf == '0') {
		if (buf[1] == 'x' || buf[1] == 'X') {
			// If it starts with 0x or 0X, we parse it as a hex value
			buf++;
			len--;
			return IntegerHexCastLoop<T, false, false, OP>(buf, len, result, strict);
		} else if (buf[1] == 'b' || buf[1] == 'B') {
			// If it starts with 0b or 0B, we parse it as a binary value
			buf++;
			len--;
			return IntegerBinaryCastLoop<T, false, false, OP>(buf, len, result, strict);
		} else if (strict && StringUtil::CharacterIsDigit(buf[1])) {
			// leading zeros are not allowed in strict mode
			return false;
		}
	}
	return IntegerCastLoop<T, false, ALLOW_EXPONENT, OP, decimal_separator>(buf, len, result, strict);
}

template <typename T, bool IS_SIGNED = true>
static inline bool TrySimpleIntegerCast(const char *buf, idx_t len, T &result, bool strict) {
	IntegerCastData<T> simple_data;
	if (TryIntegerCast<IntegerCastData<T>, IS_SIGNED, false, IntegerCastOperation>(buf, len, simple_data, strict)) {
		result = (T)simple_data.result;
		return true;
	}

	// Simple integer cast failed, try again with decimals/exponents included
	// FIXME: This could definitely be improved as some extra work is being done here. It is more important that
	//  "normal" integers (without exponent/decimals) are still being parsed quickly.
	IntegerDecimalCastData<T> cast_data;
	if (TryIntegerCast<IntegerDecimalCastData<T>, IS_SIGNED, true, IntegerDecimalCastOperation>(buf, len, cast_data,
	                                                                                            strict)) {
		result = (T)cast_data.result;
		return true;
	}
	return false;
}
} // namespace duckdb
