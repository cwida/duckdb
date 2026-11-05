//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/list_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/storage/statistics/stats_merge_type.hpp"

namespace duckdb {
class BaseStatistics;
struct SelectionVector;
class Vector;
class Value;

struct ListStatsData {
	//! Whether or not the minimum list length is known
	bool has_min_list_length;
	//! Whether or not the maximum list length is known
	bool has_max_list_length;
	//! Whether or not the total list length is known
	bool has_total_list_length;
	//! The minimum length of any non-NULL list
	uint64_t min_list_length;
	//! The maximum length of any non-NULL list
	uint64_t max_list_length;
	//! The total (summed) length of all non-NULL lists
	uint64_t total_list_length;

public:
	bool HasMinListLength() const;
	//! The minimum, maximum and total list length, which are invalid if they are unknown
	optional_idx MinLength() const;
	optional_idx MaxLength() const;
	optional_idx TotalLength() const;
};

struct ListStats {
	DUCKDB_API static void Construct(BaseStatistics &stats);
	DUCKDB_API static BaseStatistics CreateUnknown(LogicalType type);
	DUCKDB_API static BaseStatistics CreateEmpty(LogicalType type);

	DUCKDB_API static const BaseStatistics &GetChildStats(const BaseStatistics &stats);
	DUCKDB_API static BaseStatistics &GetChildStats(BaseStatistics &stats);
	DUCKDB_API static void SetChildStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats);

	//! Returns the minimum list length of the non-NULL lists, if known
	DUCKDB_API static optional_idx MinListLength(const BaseStatistics &stats);
	//! Returns the maximum list length of the non-NULL lists, if known
	DUCKDB_API static optional_idx MaxListLength(const BaseStatistics &stats);
	//! Returns the total list length of the non-NULL lists, if known
	DUCKDB_API static optional_idx TotalListLength(const BaseStatistics &stats);
	//! Sets the list length bounds, which are unknown if not set, and makes the total list length unknown
	DUCKDB_API static void SetLengthBounds(BaseStatistics &stats, optional_idx min_length, optional_idx max_length);
	//! Merges in the length stats of a set of lists
	DUCKDB_API static void MergeInLengths(BaseStatistics &stats, idx_t min_length, idx_t max_length,
	                                      idx_t total_length);
	//! Returns a copy of the list length stats
	DUCKDB_API static ListStatsData GetLengthData(const BaseStatistics &stats);
	//! Overwrites the list length stats
	DUCKDB_API static void SetLengthData(BaseStatistics &stats, const ListStatsData &data);
	//! Resets the list length stats to the stats of an empty set of lists
	DUCKDB_API static void ResetLengthData(BaseStatistics &stats);
	//! Marks the total list length as unknown
	DUCKDB_API static void ResetTotalListLength(BaseStatistics &stats);

	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &base);

	DUCKDB_API static child_list_t<Value> ToStruct(const BaseStatistics &stats);

	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other,
	                             StatsMergeType merge_type = StatsMergeType::MERGE_STATS);
	DUCKDB_API static void Copy(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static void Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
	                              idx_t count);

private:
	static ListStatsData &GetDataUnsafe(BaseStatistics &stats);
	static const ListStatsData &GetDataUnsafe(const BaseStatistics &stats);
};

} // namespace duckdb
