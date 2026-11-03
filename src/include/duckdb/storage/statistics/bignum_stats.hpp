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

//! Statistics of values stored as BIGNUM blobs (decimals wider than 38 digits): the byte order of the BIGNUM
//! encoding is the numeric order, so the exact min/max are kept as blobs and compared with memcmp
struct BignumStatsData {
	//! Whether the min/max are known - unknown bounds cannot prune anything
	bool has_min;
	bool has_max;
	//! The minimum and maximum blob - empty (zero bytes) while no value has been seen
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
	//! The min/max as a value of the type of the statistics - throws if there is none
	DUCKDB_API static Value Min(const BaseStatistics &stats);
	DUCKDB_API static Value Max(const BaseStatistics &stats);
	//! The min/max blob - only valid if HasMinMax
	DUCKDB_API static string_t MinBlob(const BaseStatistics &stats);
	DUCKDB_API static string_t MaxBlob(const BaseStatistics &stats);
	//! Sets the min/max to a value of the type of the statistics (a NULL value makes the bound unknown)
	DUCKDB_API static void SetMin(BaseStatistics &stats, const Value &val);
	DUCKDB_API static void SetMax(BaseStatistics &stats, const Value &val);
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
	//! Stores a copy of the blob in the statistics and returns a reference to it
	static string_t AssignBlob(BaseStatistics &stats, string_t blob, bool is_min);
	static void MergeBound(BaseStatistics &stats, string_t blob, bool is_min);
};

} // namespace duckdb
