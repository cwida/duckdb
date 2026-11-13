//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/numeric_range_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/table/numeric_range.hpp"

namespace duckdb {

class StatsStructFields;

//! The exact value range of NUMERIC statistics: it decides the representation that holds every value, which lets
//! operators work on the values in that representation instead of on canonical blobs. NUMERIC statistics are string
//! statistics over the canonical blobs, so the range extends their extra data; it is unknown for values that did not
//! come from storage.
struct NumericRangeStats : public StringStatsExtraData {
public:
	bool has_range = false;
	NumericRange range;

public:
	DUCKDB_API static void Set(BaseStatistics &stats, const NumericRange &range);
	//! The range of the statistics, if it is known
	DUCKDB_API static optional_ptr<const NumericRange> TryGet(const BaseStatistics &stats);
	//! The representation that holds every value described by the statistics; invalid when it is unknown
	DUCKDB_API static LogicalType GetRepresentation(const BaseStatistics &stats);

	DUCKDB_API static void Construct(BaseStatistics &stats);
	DUCKDB_API static void Copy(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &stats);
	DUCKDB_API static void ToStruct(const BaseStatistics &stats, child_list_t<Value> &children);
	DUCKDB_API static void SetFromStruct(BaseStatistics &stats, StatsStructFields &fields);

private:
	static NumericRangeStats &Get(const BaseStatistics &stats);
};

} // namespace duckdb
