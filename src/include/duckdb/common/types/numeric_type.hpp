//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/numeric_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {
class Value;

//! NUMERIC is the exact numeric type whose physical representation is not fixed by a declared width and scale but
//! inferred from the values it holds. A NUMERIC column is bound to a concrete representation (TINYINT .. HUGEINT or
//! DECIMAL(width, scale)) that covers every value in the table; each row group stores its rows in the narrowest
//! representation that fits them. The rules below decide those representations.
struct NumericType {
	//! Whether values of this type can be stored in a NUMERIC column without loss (integers and decimals)
	DUCKDB_API static bool IsExact(const LogicalType &type);
	//! The representation of a NUMERIC column that holds no values
	DUCKDB_API static LogicalType DefaultType();
	//! The number of integer digits and the scale that an exact type can hold
	DUCKDB_API static void GetDigits(const LogicalType &type, uint32_t &integer_digits, uint32_t &scale);
	//! The narrowest representation that holds every value of both inputs (type-level widening, used for inserts)
	DUCKDB_API static LogicalType Widen(const LogicalType &left, const LogicalType &right);
	//! The same; false when there is no common representation (the values would need more than 255 digits)
	DUCKDB_API static bool TryWiden(const LogicalType &left, const LogicalType &right, LogicalType &result);
	//! The narrowest representation for values in [min, max] with the given number of fractional digits
	//! (value-level, used for the bound type of a column and the storage type of a row group)
	DUCKDB_API static LogicalType ForRange(const Value &min, const Value &max, uint32_t scale);
	//! The same; false when there is no common representation
	DUCKDB_API static bool TryForRange(const Value &min, const Value &max, uint32_t scale, LogicalType &result);
	//! The storage type of a row group whose values have no common representation: their canonical encoding
	DUCKDB_API static LogicalType CanonicalStorageType();
	DUCKDB_API static bool IsCanonicalStorage(const LogicalType &type);
	//! The number of integer digits of an exact numeric value (0 for |value| < 1)
	DUCKDB_API static uint32_t IntegerDigits(const Value &value);
	//! The number of decimal digits of an unscaled value
	DUCKDB_API static idx_t Digits(hugeint_t value);
	//! The narrowest representation for unscaled values in [min, max] at the given scale
	DUCKDB_API static LogicalType ForUnscaledRange(hugeint_t min, hugeint_t max, uint32_t scale);
	//! The narrowest representation for values with the given number of integer digits and scale
	DUCKDB_API static LogicalType ForDigits(idx_t integer_digits, uint32_t scale);
	//! The unscaled value range a fixed-size exact representation can hold; false for representations beyond 128 bits
	DUCKDB_API static bool TryGetRepresentationRange(const LogicalType &type, hugeint_t &min, hugeint_t &max,
	                                                 uint32_t &scale);
	//! The widest supported representation of exact numbers with the given scale
	DUCKDB_API static LogicalType WidestType(uint32_t scale);
	//! Whether the type is NUMERIC or has NUMERIC nested anywhere inside it
	DUCKDB_API static bool ContainsNumeric(const LogicalType &type);
	//! The type with every NUMERIC replaced by the default representation
	DUCKDB_API static LogicalType WithDefaultRepresentation(const LogicalType &type);
	//! The target of a cast to a type containing NUMERIC: every NUMERIC takes the representation of the source value
	//! at that position (exact values keep their type, NULLs take the default representation); throws for inexact
	//! sources
	DUCKDB_API static LogicalType ResolveCastTarget(const LogicalType &source, const LogicalType &target);
	//! The type with every exact numeric type replaced by NUMERIC: the declared type of a column that stores the
	//! results of an exact numeric expression
	DUCKDB_API static LogicalType ExactLeavesToNumeric(const LogicalType &type);
	//! The type with every DECIMAL replaced by NUMERIC
	DUCKDB_API static LogicalType DecimalsAsNumeric(const LogicalType &type);
	//! The exact value of a numeric string literal ([-]digits[.digits][e[-]digits]) in the narrowest representation
	//! that holds it; false for anything else (infinities, text)
	DUCKDB_API static bool TryParseLiteral(const string &text, Value &result);
	//! Parses the exponent of scientific notation, starting after the 'e'; false for other text
	DUCKDB_API static bool TryParseExponent(const char *text, idx_t pos, idx_t end, int64_t &exponent);
	//! The significant integer digits and the fractional digits of a number written in text; false for other text
	DUCKDB_API static bool TryCountDigits(const char *text, idx_t size, idx_t &integer_digits,
	                                      idx_t &fractional_digits);
	//! Whether a number in text uses scientific notation, and its plain decimal notation (for validated text)
	DUCKDB_API static bool HasExponent(const char *text, idx_t size);
	DUCKDB_API static string ExpandExponent(const char *text, idx_t size);
	//! The narrowest representation that holds every value with these digits; false beyond the widest DECIMAL
	DUCKDB_API static bool TryForDigits(idx_t integer_digits, idx_t fractional_digits, LogicalType &type);
	//! The type with every NUMERIC replaced by VARCHAR: how readers deliver NUMERIC columns, the storage parses them
	DUCKDB_API static LogicalType AsText(const LogicalType &type);
	//! Error thrown for values that cannot be stored in a NUMERIC column
	DUCKDB_API static string NotExactMessage(const LogicalType &type);
};

} // namespace duckdb
