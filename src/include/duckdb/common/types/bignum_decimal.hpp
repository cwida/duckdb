//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/bignum_decimal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace duckdb {
struct BignumIntermediate;
class StringHeap;

//! Helpers for DECIMAL values wider than 38 digits, stored as BIGNUM blobs
class BignumDecimal {
public:
	//! 10^exponent as a BIGNUM blob, for exponent in [0, Decimal::MAX_WIDTH_DECIMAL]
	DUCKDB_API static const string &PowerOfTen(idx_t exponent);
	//! Whether |value| < 10^width, i.e. the unscaled value fits a DECIMAL of the given width
	DUCKDB_API static bool FitsWidth(const string_t &value, uint32_t width);
	DUCKDB_API static bool FitsWidth(const BignumIntermediate &value, uint32_t width);

	//! Formats an unscaled value with the given scale, e.g. 12345 with scale 2 -> "123.45"
	DUCKDB_API static string ToString(const string_t &value, uint32_t scale);
	//! Parses a decimal string into the unscaled value of a DECIMAL(width, scale), rounding half away from zero
	DUCKDB_API static bool TryParse(const char *data, idx_t size, uint32_t width, uint32_t scale, string &result,
	                                char decimal_separator = '.');

	//! Rescales an unscaled value to target_scale (rounding half away from zero) and checks it fits target_width
	DUCKDB_API static bool TryRescale(const string_t &value, uint32_t source_scale, uint32_t target_width,
	                                  uint32_t target_scale, string &result);

	//! The BIGNUM blob of an integer
	DUCKDB_API static string FromInteger(hugeint_t value);
	DUCKDB_API static string FromInteger(uhugeint_t value);
	//! Converts an unscaled value to a hugeint, returns false if it does not fit
	DUCKDB_API static bool TryToHugeint(const string_t &value, hugeint_t &result);
	DUCKDB_API static bool TryToUhugeint(const string_t &value, uhugeint_t &result);

	//! Converts an unscaled value with the given scale to a double (correctly rounded through the decimal string)
	DUCKDB_API static double ToDouble(const string_t &value, uint32_t scale);
	//! Converts a double to the unscaled value of a DECIMAL(width, scale), returns false if it does not fit
	DUCKDB_API static bool TryFromDouble(double value, uint32_t width, uint32_t scale, string &result);

	//! Converts a two's complement integer of "size" bytes (big- or little-endian) into the BIGNUM blob of its value
	DUCKDB_API static string FromTwosComplement(const_data_ptr_t data, idx_t size, bool big_endian);
	//! Writes the unscaled value as a two's complement integer of "size" bytes, throws if it does not fit
	DUCKDB_API static void ToTwosComplement(const string_t &value, data_ptr_t result, idx_t size, bool big_endian);

	//! Whether the unscaled value is zero
	DUCKDB_API static bool IsZero(const string_t &value);
	//! Whether the unscaled value is negative
	DUCKDB_API static bool IsNegative(const string_t &value);
};

} // namespace duckdb
