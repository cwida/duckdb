//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/variant_comparison.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/variant.hpp"

namespace duckdb {

class VariantNode;

//! The sort-key rank of a variant value - determines the cross-type ordering (type-first).
//! Ranks start at 1 so that every rank is strictly greater than the LIST/STRING delimiter (0); this
//! guarantees a shorter list/object sorts before a longer one (the delimiter is smaller than any
//! following element's rank byte) and that an element can never be confused with an end-of-list marker.
enum class VariantComparisonType : uint8_t {
	BOOLEAN = 1,
	NUMBER,
	REAL,
	VARCHAR,
	BLOB,
	UUID,
	TIMESTAMP,    // DATE and all non-tz TIMESTAMP precisions, compared as nanoseconds since the epoch
	TIMESTAMP_TZ, // TIMESTAMP WITH TIME ZONE (all precisions), compared as nanoseconds since the epoch (UTC)
	TIME,         // TIME (all precisions), compared as nanoseconds since midnight
	TIME_TZ,      // TIME WITH TIME ZONE
	INTERVAL,
	GEOMETRY,
	GEOGRAPHY,
	BITSTRING,
	ARRAY,
	OBJECT,
	//! VARIANT_NULL ranks last so a nested NULL element orders after all non-null values, matching the
	//! native "NULLS LAST" ordering of nested NULLs under ASC. For DESC the ORDER BY operator reverses
	//! the (always-ascending) comparator key, which then places nested NULLs first - exactly as native
	//! nested ordering does (where the null position within nested types follows ASC/DESC).
	NULL_VALUE
};

//===--------------------------------------------------------------------===//
// NUMBER rank encoding
//===--------------------------------------------------------------------===//
// All integers, decimals and bignums fold into a single NUMBER rank and are compared by numeric
// value. To make values of different scales/widths comparable (and equal when numerically equal),
// each value is reduced to an order-preserving "decimal-scientific" form:
//
//     value = sign * d1.d2 ... dk * 10^adjexp     (d1 != 0, trailing zeros stripped)
//
// and encoded as [class][adjexp][digits][terminator], where everything after the class byte is
// complemented for negatives. This makes e.g. 1::TINYINT, 1::BIGINT, 1.00::DECIMAL and 1::BIGNUM all
// encode identically, and orders them numerically (so -100.5 < 0 < 1.5 < 15 < ...).
struct VariantNumberKey {
	//! 0 = negative, 1 = zero, 2 = positive (so negatives sort before zero before positives)
	data_t number_class;
	int64_t adjusted_exponent;
	//! significant decimal digits ('0'..'9'), leading digit non-zero, trailing zeros stripped
	string digits;

	bool operator==(const VariantNumberKey &other) const {
		if (number_class != other.number_class) {
			return false;
		}
		if (adjusted_exponent != other.adjusted_exponent) {
			return false;
		}
		if (digits != other.digits) {
			return false;
		}
		return true;
	}
};

VariantComparisonType GetVariantComparisonType(VariantLogicalType type);
//! Compute the canonical logical value in the REAL rank, this is not directly byte comparable.
double VariantGetRealValue(VariantLogicalType type_id, const VariantNode &node);
//! Compute the number key for any value in the NUMBER rank (integer, decimal or bignum)
VariantNumberKey VariantGetNumberKey(VariantLogicalType type_id, const VariantNode &node);
//! Compute the nanoseconds since the epoch for any TIMESTAMP rank value
hugeint_t VariantGetTimestampValue(VariantLogicalType type_id, const VariantNode &node);
//! Compute the nanoseconds since the epoch (UTC) for any TIMESTAMP_TZ rank value
hugeint_t VariantGetTimestampTZValue(VariantLogicalType type_id, const VariantNode &node);
//! Compute the nanoseconds since midnight for any TIME rank value
int64_t VariantGetTimeValue(VariantLogicalType type_id, const VariantNode &node);

} // namespace duckdb
