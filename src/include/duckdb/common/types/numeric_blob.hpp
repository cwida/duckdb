//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/numeric_blob.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! The canonical representation of a NUMERIC value: an order-preserving, normalized byte string
//! [class][exponent][digits][terminator]. The class byte is 0x00 for negative numbers, 0x40 for zero and 0x80 for
//! positive numbers; the exponent is the order-preserving two-byte adjusted decimal exponent of the first significant
//! digit; the digits are the significant digits (1..10 each, no leading or trailing zeros) followed by a 0x00
//! terminator. Every payload byte is complemented for negative numbers so that larger magnitudes sort first. Byte order
//! is numeric order and equal numbers have equal bytes, whatever representation they came from.
struct NumericBlob {
	//! The longest blob: class, exponent, the digits of a 255-digit value and the terminator
	static constexpr idx_t MAX_BLOB_SIZE = 3 + 255 + 1;

	//! Encodes decimal text ([-]digits[.digits], surrounding spaces allowed); false for text that is not a plain number
	DUCKDB_API static bool TryEncode(const char *text, idx_t size, string &result);
	//! Encodes decimal text, throwing for text that is not a plain number
	DUCKDB_API static string Encode(const string &text);
	//! The size of the blob that starts at `data`, of which `available` bytes can be read
	DUCKDB_API static idx_t BlobSize(const_data_ptr_t data, idx_t available);
	//! The decimal text of a blob: no exponent, no trailing zeros
	DUCKDB_API static string ToText(const string_t &blob);
	//! The digits before and after the decimal point that a blob needs
	DUCKDB_API static void Digits(const string_t &blob, idx_t &integer_digits, idx_t &fractional_digits);
	//! The narrowest representation that holds every value of a canonical NUMERIC vector
	DUCKDB_API static LogicalType RangeType(Vector &canonical, idx_t count);
	//! The narrowest representation holding every value of a canonical vector; false when there is none
	DUCKDB_API static bool TryRangeType(Vector &canonical, idx_t count, LogicalType &type);
	//! Encodes a vector of an exact representation into a flat canonical NUMERIC vector
	DUCKDB_API static void Encode(Vector &typed, idx_t count, Vector &result);
	//! Decodes a canonical NUMERIC vector into a vector of the given representation
	DUCKDB_API static void Decode(Vector &canonical, idx_t count, Vector &typed);
	//! Decodes a canonical NUMERIC vector into a vector of a fixed-size representation; false (with the typed vector
	//! unspecified) when a value does not fit
	DUCKDB_API static bool TryDecode(Vector &canonical, idx_t count, Vector &typed);
	//! The unscaled 128-bit value and the scale (without trailing zeros) of a blob; false beyond 38 digits
	DUCKDB_API static bool TryDecodeUnscaled(const string_t &blob, hugeint_t &unscaled, uint32_t &scale);
	//! Writes the blob of unscaled * 10^-scale into a buffer of MAX_BLOB_SIZE bytes and returns its length
	DUCKDB_API static idx_t EncodeUnscaled(hugeint_t unscaled, uint32_t scale, data_ptr_t buffer);
	//! Adds the blob of unscaled * 10^-scale to a canonical NUMERIC vector
	DUCKDB_API static string_t AddUnscaled(Vector &result, hugeint_t unscaled, uint32_t scale);
	//! Strips the trailing zeros of the fractional part of decimal text (and the point if nothing remains)
	DUCKDB_API static string NormalizeText(const string &text);
};

} // namespace duckdb
