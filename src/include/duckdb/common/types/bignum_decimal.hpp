//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/bignum_decimal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bignum.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace duckdb {
class ClientContext;
class StringHeap;

//! How the digits dropped by BignumDecimal::TryRound are rounded
enum class BignumRounding : uint8_t { TRUNCATE, FLOOR, CEIL, HALF_AWAY_FROM_ZERO, HALF_TO_EVEN };

//! Helpers for DECIMAL values wider than 38 digits, whose unscaled value is a bignum_t (the BIGNUM blob)
class BignumDecimal {
public:
	//! 10^exponent as a BIGNUM blob, the powers of everyday widths are cached; a long computation checks for
	//! interrupts through the context, as do the other conversions below
	DUCKDB_API static string PowerOfTen(idx_t exponent, optional_ptr<ClientContext> context = nullptr);
	//! 2^exponent as a BIGNUM blob
	DUCKDB_API static string PowerOfTwo(idx_t exponent);
	//! Whether |value| < 10^width, i.e. the unscaled value fits a DECIMAL of the given width
	DUCKDB_API static bool FitsWidth(const bignum_t &value, uint32_t width);
	DUCKDB_API static bool FitsWidth(const BignumIntermediate &value, uint32_t width);

	//! Formats an unscaled value with the given scale, e.g. 12345 with scale 2 -> "123.45"
	DUCKDB_API static string ToString(const bignum_t &value, uint32_t scale,
	                                  optional_ptr<ClientContext> context = nullptr);
	//! Parses a decimal string into the BIGNUM blob of the unscaled value of a DECIMAL(width, scale), rounding half
	//! away from zero
	DUCKDB_API static bool TryParse(const char *data, idx_t size, uint32_t width, uint32_t scale, string &result,
	                                char decimal_separator = '.', optional_ptr<ClientContext> context = nullptr);

	//! Rescales an unscaled value to target_scale (rounding half away from zero), the result is the BIGNUM blob of the
	//! rescaled value
	DUCKDB_API static string Rescale(const bignum_t &value, uint32_t source_scale, uint32_t target_scale,
	                                 optional_ptr<ClientContext> context = nullptr);
	//! Rescales an unscaled value to target_scale (rounding half away from zero) and checks it fits target_width, the
	//! result is the BIGNUM blob of the rescaled value
	DUCKDB_API static bool TryRescale(const bignum_t &value, uint32_t source_scale, uint32_t target_width,
	                                  uint32_t target_scale, string &result,
	                                  optional_ptr<ClientContext> context = nullptr);

	//! Drops drop_digits decimal digits of an unscaled value with the given rounding and appends restore_digits zero
	//! digits, i.e. rounds the value at a digit position; returns false if the result does not fit the width. The
	//! result is allocated in the heap, the state holds the scratch buffers of the digit arithmetic
	DUCKDB_API static bool TryRound(const bignum_t &value, idx_t drop_digits, idx_t restore_digits,
	                                BignumRounding rounding, uint32_t width, StringHeap &heap,
	                                BignumArithmeticState &state, string_t &result);

	//! The BIGNUM blob of an integer
	DUCKDB_API static string FromInteger(hugeint_t value);
	DUCKDB_API static string FromInteger(uhugeint_t value);
	//! Converts an unscaled value to a hugeint, returns false if it does not fit
	DUCKDB_API static bool TryToHugeint(const bignum_t &value, hugeint_t &result);
	DUCKDB_API static bool TryToUhugeint(const bignum_t &value, uhugeint_t &result);

	//! Converts an unscaled value with the given scale to a FLOAT or DOUBLE, rounding once (through the decimal
	//! string), returns false if the value is out of range
	DUCKDB_API static bool TryToFloating(const bignum_t &value, uint32_t scale, float &result,
	                                     optional_ptr<ClientContext> context = nullptr);
	DUCKDB_API static bool TryToFloating(const bignum_t &value, uint32_t scale, double &result,
	                                     optional_ptr<ClientContext> context = nullptr);
	//! Converts an unscaled value with the given scale to a double, infinite when the value is out of range
	DUCKDB_API static double ToDouble(const bignum_t &value, uint32_t scale,
	                                  optional_ptr<ClientContext> context = nullptr);
	//! Converts a double to the BIGNUM blob of the unscaled value of a DECIMAL(width, scale), returns false if it does
	//! not fit
	DUCKDB_API static bool TryFromDouble(double value, uint32_t width, uint32_t scale, string &result,
	                                     optional_ptr<ClientContext> context = nullptr);
	//! The BIGNUM blob of a finite double with an integral value, every such value converts exactly
	DUCKDB_API static string FromDouble(double value);

	//! Converts a two's complement integer of "size" bytes (big- or little-endian) into the BIGNUM blob of its value
	DUCKDB_API static string FromTwosComplement(const_data_ptr_t data, idx_t size, bool big_endian);
	//! Writes the unscaled value as a two's complement integer of "size" bytes, throws if it does not fit
	DUCKDB_API static void ToTwosComplement(const bignum_t &value, data_ptr_t result, idx_t size, bool big_endian);

	//! Whether the unscaled value is zero
	DUCKDB_API static bool IsZero(const bignum_t &value);
	//! Whether the unscaled value is negative
	DUCKDB_API static bool IsNegative(const bignum_t &value);
};

} // namespace duckdb
