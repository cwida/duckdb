//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/bignum_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array_ptr.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {
class BaseStatistics;
struct SelectionVector;
class Vector;
class Serializer;
class Deserializer;

//! Statistics of values stored as BIGNUM blobs (decimals wider than 38 digits). The byte order of the BIGNUM encoding
//! is the numeric order, so the bounds are blobs compared bytewise. A bound keeps at most MAX_BOUND_SIZE bytes: a
//! longer blob is cut after the header and the first magnitude bytes, which makes the bound inexact. The bytes a cut
//! min bound omits read as 0x00 and those of a cut max bound as 0xFF, so a cut min is below the value it was cut from
//! and a cut max above it: the bounds always enclose every value, they are only not attained when inexact.
struct BignumStatsData {
	//! The magnitude bytes a bound keeps (256 bits): decimals of up to 77 digits keep exact bounds
	static constexpr uint32_t MAX_BOUND_DATA_BYTES = 32;

	//! Whether the min/max are known - unknown bounds cannot prune anything
	bool has_min;
	bool has_max;
	//! Whether the bound is the exact min/max of the values, false when it was cut
	bool min_exact;
	bool max_exact;
	//! The minimum and maximum bound - empty (zero bytes) while no value has been seen
	string_t min;
	string_t max;
};

struct BignumStats {
	//! Unknown statistics - the bounds are not known
	DUCKDB_API static BaseStatistics CreateUnknown(LogicalType type);
	//! Empty statistics - the bounds are known, no value has been seen yet
	DUCKDB_API static BaseStatistics CreateEmpty(LogicalType type);

	//! Whether the stats have a known min and max that cover at least one value
	DUCKDB_API static bool HasMinMax(const BaseStatistics &stats);
	//! Whether the min/max is attained by a value, i.e. was not cut to the bound size
	DUCKDB_API static bool MinIsExact(const BaseStatistics &stats);
	DUCKDB_API static bool MaxIsExact(const BaseStatistics &stats);
	//! The min/max bound as a value of the type of the statistics - throws if there is none
	DUCKDB_API static Value Min(const BaseStatistics &stats);
	DUCKDB_API static Value Max(const BaseStatistics &stats);
	//! The min/max bound as a complete BIGNUM blob (a cut bound is padded) - only valid if HasMinMax
	DUCKDB_API static string MinBlob(const BaseStatistics &stats);
	DUCKDB_API static string MaxBlob(const BaseStatistics &stats);
	//! Sets the min/max to a value of the type of the statistics (a NULL value makes the bound unknown)
	DUCKDB_API static void SetMin(BaseStatistics &stats, const Value &val);
	DUCKDB_API static void SetMax(BaseStatistics &stats, const Value &val);
	//! Sets the min/max to a value, which is cut when it is longer than a bound
	DUCKDB_API static void SetMin(BaseStatistics &stats, string_t blob);
	DUCKDB_API static void SetMax(BaseStatistics &stats, string_t blob);

	//! Extends the bounds with a value
	DUCKDB_API static void Update(BaseStatistics &stats, string_t blob);
	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other);
	//! Merges exact bounds (empty when no value was seen), as collected by a StatsWriter
	DUCKDB_API static void Merge(BaseStatistics &stats, string_t min, string_t max);
	DUCKDB_API static void Copy(BaseStatistics &stats, const BaseStatistics &other);

	//! Whether a comparison with the constants can be satisfied by values within the bounds
	DUCKDB_API static FilterPropagateResult CheckZonemap(const BaseStatistics &stats, ExpressionType comparison_type,
	                                                     array_ptr<const Value> constants);

	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &stats);

	DUCKDB_API static child_list_t<Value> ToStruct(const BaseStatistics &stats);
	DUCKDB_API static void Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
	                              idx_t count);

private:
	static BignumStatsData &GetDataUnsafe(BaseStatistics &stats);
	static const BignumStatsData &GetDataUnsafe(const BaseStatistics &stats);
	//! Stores a copy of the blob in the statistics, cut to the bound size, and returns a reference to it
	static string_t AssignBound(BaseStatistics &stats, string_t blob, bool is_min, bool &exact);
	static void MergeBound(BaseStatistics &stats, string_t blob, bool exact, bool is_min);
};

} // namespace duckdb
