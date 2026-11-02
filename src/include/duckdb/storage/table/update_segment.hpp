//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/update_segment.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/storage/statistics/segment_statistics.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/transaction/undo_buffer_allocator.hpp"
#include "duckdb/transaction/transaction_data.hpp"

namespace duckdb {
class ColumnData;
class DataTable;
class DuckTableEntry;
struct SelectionVector;
class Vector;
struct UpdateInfo;
struct UpdateNode;
struct UndoBufferAllocator;

class UpdateSegment {
public:
	explicit UpdateSegment(ColumnData &column_data);
	~UpdateSegment();

	ColumnData &column_data;

public:
	bool HasUpdates() const;
	bool HasUncommittedUpdates(idx_t vector_index);
	bool HasUpdates(idx_t vector_index) const;
	bool HasUpdates(idx_t start_row_idx, idx_t end_row_idx);

	void FetchUpdates(TransactionData transaction, idx_t vector_index, Vector &result);
	void FetchCommitted(idx_t vector_index, Vector &result);
	void FetchCommittedRange(idx_t start_row, idx_t count, Vector &result);
	void Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index, Vector &update,
	            row_t *ids, idx_t count, Vector &base_data, idx_t row_group_start);
	void FetchRows(TransactionData transaction, const idx_t *offsets, const SelectionVector &sel, idx_t count,
	               Vector &result, idx_t result_offset);

	void RollbackUpdate(UpdateInfo &info);
	void CleanupUpdateInternal(const StorageLockKey &lock, UpdateInfo &info);
	void CleanupUpdate(UpdateInfo &info);

	unique_ptr<BaseStatistics> GetStatistics();
	StringHeap &GetStringHeap() {
		return heap;
	}

private:
	//! The lock for the update segment
	mutable StorageLock lock;
	//! The root node (if any)
	unique_ptr<UpdateNode> root;
	//! Update statistics
	SegmentStatistics stats;
	//! Stats lock
	mutex stats_lock;
	//! Internal type size
	idx_t type_size;
	//! String heap, only used for strings
	StringHeap heap;

public:
	typedef void (*merge_update_function_t)(UpdateInfo &info, UnifiedVectorFormat &update, row_t *ids, idx_t count,
	                                        const SelectionVector &sel, idx_t vector_offset);
	typedef void (*save_undo_function_t)(UpdateInfo &source, UpdateInfo &root, row_t *ids, idx_t count,
	                                     const SelectionVector &sel, idx_t vector_offset, UpdateInfo &target);
	typedef void (*rollback_update_function_t)(UpdateInfo &root, UpdateInfo &rollback_info);
	typedef void (*fetch_update_function_t)(const SnapshotView &view, UpdateInfo &info, Vector &result);
	typedef void (*fetch_committed_function_t)(UpdateInfo &info, Vector &result);
	typedef void (*fetch_committed_range_function_t)(UpdateInfo &info, idx_t start, idx_t end, idx_t result_offset,
	                                                 Vector &result);
	typedef void (*fetch_rows_function_t)(const SnapshotView &view, UpdateInfo &info, const idx_t *offsets,
	                                      const SelectionVector &sel, idx_t fetch_offset, idx_t count,
	                                      idx_t vector_offset, Vector &result, idx_t result_offset);
	typedef idx_t (*statistics_update_function_t)(UpdateSegment *segment, SegmentStatistics &stats,
	                                              UnifiedVectorFormat &update, idx_t count, SelectionVector &sel);
	typedef idx_t (*get_effective_updates_t)(UnifiedVectorFormat &update_format, row_t *ids, idx_t count,
	                                         SelectionVector &sel, Vector &base_data, idx_t id_offset);

private:
	merge_update_function_t merge_update_function;
	save_undo_function_t save_undo_function;
	rollback_update_function_t rollback_update_function;
	fetch_update_function_t fetch_update_function;
	fetch_committed_function_t fetch_committed_function;
	fetch_committed_range_function_t fetch_committed_range;
	fetch_rows_function_t fetch_rows_function;
	statistics_update_function_t statistics_update_function;
	get_effective_updates_t get_effective_updates;

private:
	UndoBufferPointer GetUpdateNode(StorageLockKey &lock, idx_t vector_idx) const;
	void InitializeUpdateInfo(idx_t vector_idx);
	void ReallocateRootInfoIfNeeded(UpdateInfo &current_info, idx_t update_count, idx_t vector_index);
};

struct UpdateNode {
	explicit UpdateNode(BufferManager &manager);
	~UpdateNode();

	UndoBufferAllocator allocator;
	vector<UndoBufferPointer> info;
};

} // namespace duckdb
