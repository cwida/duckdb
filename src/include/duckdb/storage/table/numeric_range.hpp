//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/numeric_range.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/shredded_numeric_range.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

//! The exact value range of NUMERIC data: a minimum and maximum of any width and the fractional digits the values need
//! (trailing zeros do not count). It decides the narrowest representation that holds the values, and it describes
//! them as statistics of a representation or as canonical NUMERIC statistics.
class NumericRange {
public:
	bool HasValues() const {
		return has_values;
	}
	const Value &Min() const {
		return min;
	}
	const Value &Max() const {
		return max;
	}
	uint32_t Scale() const {
		return scale;
	}

	//! Widen the range with an exact value
	void Update(const Value &value);
	//! Widen the range with the values of a vector of an exact representation, or of canonical NUMERIC values
	void Update(const Vector &vector, idx_t count);
	void UpdateCanonical(const Vector &canonical, idx_t count);
	//! Widen the range with statistics of an exact representation; false when they are not exact (wide decimal
	//! statistics only keep prefixes)
	bool TryUpdate(const BaseStatistics &typed_stats);
	void Merge(const NumericRange &other);
	//! Orders two exact values, whatever their representations
	static int Compare(const Value &left, const Value &right);

	//! The narrowest representation that holds every value (the default representation for an empty range), or the
	//! canonical storage type when no representation holds them all
	LogicalType ResolveType() const;
	//! Statistics of the range in an exact representation: numeric statistics, or string statistics of the BIGNUM blobs
	//! for wide decimals
	BaseStatistics ToTyped(const LogicalType &type) const;
	//! Canonical NUMERIC statistics: string statistics over the canonical blobs of the minimum and maximum
	BaseStatistics ToCanonical() const;
	//! The range as unscaled values at the scale of a representation that holds it; false when there are no values,
	//! for wide decimals, or when 128 bits do not suffice
	bool TryGetUnscaled(const LogicalType &representation, ShreddedNumericRange &result) const;

	void Serialize(Serializer &serializer) const;
	static NumericRange Deserialize(Deserializer &deserializer);

private:
	bool has_values = false;
	//! The fractional digits the values need
	uint32_t scale = 0;
	//! The minimum and maximum as DECIMAL values of the scale
	Value min;
	Value max;
};

} // namespace duckdb
