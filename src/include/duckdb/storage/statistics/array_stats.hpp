//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/array_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/storage/statistics/stats_merge_type.hpp"

namespace duckdb {
class BaseStatistics;
struct SelectionVector;
class Vector;
class Serializer;
class Deserializer;
class Value;
class StatsStructFields;

struct ArrayStats {
	DUCKDB_API static void Construct(BaseStatistics &stats);
	DUCKDB_API static BaseStatistics CreateUnknown(LogicalType type);
	DUCKDB_API static BaseStatistics CreateEmpty(LogicalType type);

	DUCKDB_API static const BaseStatistics &GetChildStats(const BaseStatistics &stats);
	DUCKDB_API static BaseStatistics &GetChildStats(BaseStatistics &stats);
	DUCKDB_API static void SetChildStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats);

	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &base);

	DUCKDB_API static child_list_t<Value> ToStruct(const BaseStatistics &stats, bool exact);
	//! Sets the parts of the statistics the struct fields describe (the inverse of ToStruct)
	DUCKDB_API static void SetFromStruct(BaseStatistics &stats, StatsStructFields &fields);

	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other,
	                             StatsMergeType merge_type = StatsMergeType::MERGE_STATS);
	DUCKDB_API static void Copy(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static void Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
	                              idx_t count);
};

} // namespace duckdb
