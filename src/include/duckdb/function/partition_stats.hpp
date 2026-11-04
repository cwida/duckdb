//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/partition_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/storage/storage_index.hpp"

namespace duckdb {

//! How a table is partitioned by a given set of columns
enum class TablePartitionInfo : uint8_t {
	NOT_PARTITIONED,         // the table is not partitioned by the given set of columns
	SINGLE_VALUE_PARTITIONS, // each partition has exactly one unique value (e.g. bounds = [1,1][2,2][3,3])
	OVERLAPPING_PARTITIONS,  // the partitions overlap **only** at the boundaries (e.g. bounds = [1,2][2,3][3,4]
	DISJOINT_PARTITIONS      // the partitions are disjoint (e.g. bounds = [1,2][3,4][5,6])
};

enum class CountType { COUNT_EXACT, COUNT_APPROXIMATE };

struct PartitionRowGroup {
	virtual ~PartitionRowGroup() = default;
	virtual unique_ptr<BaseStatistics> GetColumnStatistics(const StorageIndex &storage_index) = 0;
	virtual bool MinMaxIsExact(const StorageIndex &storage_index) = 0;
	//! Whether this row group has data that has not been durably checkpointed/flushed yet, or widened statistics.
	virtual bool HasPendingWrites() = 0;
};

struct PartitionStatistics {
	PartitionStatistics();

	//! The row id start
	optional_idx row_start;
	//! The amount of rows in the partition
	idx_t count;
	//! Whether or not the count is exact or approximate
	CountType count_type;
	//! Optional accessor for row group statistics
	shared_ptr<PartitionRowGroup> partition_row_group;
};

} // namespace duckdb
