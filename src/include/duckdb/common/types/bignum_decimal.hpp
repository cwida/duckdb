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
class Vector;

//! The operations on the unscaled value of a DECIMAL wider than 38 digits, a bignum_t (the BIGNUM blob): the interface
//! of the fixed-size tiers (see decimal_helper.hpp), whose results are owned blobs here as the values have no fixed
//! size, and the conversions only this tier needs
template <>
struct DecimalHelper<bignum_t> {
	//! An owned result, a BIGNUM blob
	using OWNED = string;
	static constexpr bool IS_WIDE = true;

	//! The scratch of TryRound: the heap of the result vector, where the results are allocated, and the buffers of the
	//! digit arithmetic
	struct RoundState {
		DUCKDB_API RoundState(Vector &result, optional_ptr<ClientContext> context);

		StringHeap &heap;
		BignumArithmeticState state;
	};

	//! 10^exponent as a BIGNUM blob, the powers of everyday widths are cached; a long computation checks for
	//! interrupts through the context, as do the other conversions below
	DUCKDB_API static string PowerOfTen(idx_t exponent, optional_ptr<ClientContext> context = nullptr);
	//! 2^exponent as a BIGNUM blob
	DUCKDB_API static string PowerOfTwo(idx_t exponent);
	//! Whether |value| < 10^width, i.e. the unscaled value fits a DECIMAL of the given width
	DUCKDB_API static bool FitsWidth(const bignum_t &value, uint32_t width);
	DUCKDB_API static bool FitsWidth(const BignumIntermediate &value, uint32_t width);
	//! Whether the unscaled value is zero
	DUCKDB_API static bool IsZero(const bignum_t &value);
	//! Whether the unscaled value is negative
	DUCKDB_API static bool IsNegative(const bignum_t &value);
	//! A bignum_t over an owned blob, which must outlive it
	static bignum_t Load(const OWNED &owned) {
		return bignum_t(string_t(owned));
	}
	//! Stores an owned blob in the heap of a vector
	DUCKDB_API static bignum_t Store(Vector &result, const OWNED &owned);
	//! The BIGNUM blob of an integer
	DUCKDB_API static string FromInteger(hugeint_t value);
	DUCKDB_API static string FromInteger(uhugeint_t value);
	//! Converts an unscaled value to a hugeint, returns false if it does not fit
	DUCKDB_API static bool TryToHugeint(const bignum_t &value, hugeint_t &result);
	DUCKDB_API static bool TryToUhugeint(const bignum_t &value, uhugeint_t &result);

	//! Formats an unscaled value with the given width and scale, e.g. 12345 with scale 2 -> "123.45"
	DUCKDB_API static string ToString(const bignum_t &value, uint32_t width, uint32_t scale,
	                                  optional_ptr<ClientContext> context = nullptr);
	//! Parses a decimal string into the BIGNUM blob of the unscaled value of a DECIMAL(width, scale), rounding half
	//! away from zero
	DUCKDB_API static bool TryParse(const char *data, idx_t size, uint32_t width, uint32_t scale, string &result,
	                                char decimal_separator = '.', optional_ptr<ClientContext> context = nullptr);

	//! Drops drop_digits decimal digits of an unscaled value with the given rounding and appends restore_digits zero
	//! digits, i.e. rounds the value at a digit position; returns false if the result does not fit the width. The
	//! result is allocated in the heap of the state
	DUCKDB_API static bool TryRound(const bignum_t &value, idx_t drop_digits, idx_t restore_digits,
	                                DecimalRounding rounding, uint32_t width, RoundState &state, bignum_t &result);
	template <DecimalRounding ROUNDING>
	static bool TryRound(const bignum_t &value, idx_t drop_digits, idx_t restore_digits, uint32_t width,
	                     RoundState &state, bignum_t &result) {
		return TryRound(value, drop_digits, restore_digits, ROUNDING, width, state, result);
	}
	//! Rescales an unscaled value to target_scale (rounding half away from zero), the result is the BIGNUM blob of the
	//! rescaled value
	DUCKDB_API static string Rescale(const bignum_t &value, uint32_t source_scale, uint32_t target_scale,
	                                 optional_ptr<ClientContext> context = nullptr);
	//! Rescales an unscaled value to target_scale (rounding half away from zero) and checks it fits target_width, the
	//! result is the BIGNUM blob of the rescaled value
	DUCKDB_API static bool TryRescale(const bignum_t &value, uint32_t source_scale, uint32_t target_width,
	                                  uint32_t target_scale, string &result,
	                                  optional_ptr<ClientContext> context = nullptr);

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
};

} // namespace duckdb
