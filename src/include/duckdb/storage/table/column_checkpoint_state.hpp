//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/column_checkpoint_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/column_data.hpp"

namespace duckdb {
class ColumnData;
class DatabaseInstance;
class RowGroup;
class PartialBlockManager;
class TableDataWriter;

struct ColumnCheckpointState {
	ColumnCheckpointState(const RowGroup &row_group, ColumnData &original_column,
	                      PartialBlockManager &partial_block_manager);
	virtual ~ColumnCheckpointState();

	const RowGroup &row_group;
	const ColumnData &original_column;
	vector<DataPointer> data_pointers;
	unique_ptr<BaseStatistics> global_stats;

protected:
	PartialBlockManager &partial_block_manager;
	shared_ptr<ColumnData> result_column;

private:
	ColumnData &original_column_mutable;

public:
	virtual shared_ptr<ColumnData> CreateEmptyColumnData();
	virtual ColumnData &GetResultColumn();
	virtual shared_ptr<ColumnData> GetFinalResult();

	virtual unique_ptr<BaseStatistics> GetStatistics();

	virtual void FlushSegmentInternal(unique_ptr<ColumnSegment> segment, idx_t segment_size);
	virtual void FlushSegment(unique_ptr<ColumnSegment> segment, BufferHandle handle, idx_t segment_size);
	virtual PersistentColumnData ToPersistentData();

	PartialBlockManager &GetPartialBlockManager() {
		return partial_block_manager;
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
