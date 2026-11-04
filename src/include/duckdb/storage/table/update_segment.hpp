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
#include "duckdb/common/atomic.hpp"
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

class UpdateSegment;

//! The update segment of a column, shared with the column a checkpoint rewrites it into
struct ColumnUpdates {
	mutex lock;
	unique_ptr<UpdateSegment> segment;
	//! The column a checkpoint last wrote the segment's values into; null once that column is gone
	optional_ptr<ColumnData> newest_column;

	//! Clears the segment if only the newest column holds it and nothing needs its versions anymore
	void ClearIfLastHolder(const unique_lock<mutex> &guard, idx_t holders);
};

//! The updates to one column of a row group: per vector, a root with the newest values and a chain of older ones
class UpdateSegment {
public:
	explicit UpdateSegment(ColumnData &column_data);
	~UpdateSegment();

public:
	const LogicalType &GetType() const {
		return type;
	}
	const vector<column_t> &GetNestedColumnPath() const {
		return nested_column_path;
	}

	bool HasUpdates() const;
	bool HasUncommittedUpdates(idx_t vector_index);
	bool HasUpdates(idx_t vector_index) const;
	bool HasUpdates(idx_t start_row_idx, idx_t end_row_idx);
	//! Whether a committed update in this segment still has to be written by a checkpoint
	bool HasUnserializedChanges() const;
	//! Whether nothing needs the contents anymore: no older versions remain and every committed update is checkpointed
	bool CanBeCleared() const;
	//! Drops the contents unless something still needs them; keeps the segment object, which undo entries point to
	void ClearIfUnused();
	void MarkCommitted(transaction_t commit_id);
	void MarkCheckpointed(VisibilityBound visibility_bound);
	//! The vectors with updates committed below the bound that take some of their old values from the base data
	vector<idx_t> GetVectorsReadingBase(VisibilityBound visibility_bound);
	//! Copies those updates with the old values from the base data, which a rewrite replaces
	void StoreBaseValues(VisibilityBound visibility_bound, idx_t vector_index, Vector &base_data);

	void FetchUpdates(TransactionData transaction, idx_t vector_index, Vector &result);
	void FetchCommitted(idx_t vector_index, Vector &result);
	void FetchCommittedRange(const SnapshotView &view, idx_t start_row, idx_t count, Vector &result);
	void Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index, Vector &update,
	            row_t *ids, idx_t count, Vector &base_data, idx_t row_group_start);
	void FetchRows(TransactionData transaction, const idx_t *offsets, const SelectionVector &sel, idx_t count,
	               Vector &result, idx_t result_offset);

	void RollbackUpdate(UpdateInfo &info);
	void CleanupUpdateInternal(const StorageLockKey &lock, UpdateInfo &info);
	void CleanupUpdate(UpdateInfo &info);

	unique_ptr<BaseStatistics> GetStatistics();
	//! Widens the statistics by those of an update, or of the column this segment was handed over from
	void MergeStatistics(const BaseStatistics &other);

private:
	//! The type of the column
	LogicalType type;
	//! The column indexes from below the top-level column down to this column
	vector<column_t> nested_column_path;
	//! The buffer manager the root node allocates from
	BufferManager &buffer_manager;
	//! The newest commit id of an update on this segment that no checkpoint has written yet, or 0
	atomic<transaction_t> newest_uncheckpointed_update_commit;
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
	//! String heap, only used for strings; a clear replaces it, scan results holding its strings keep the old one alive
	shared_ptr<StringHeap> heap;

public:
	typedef void (*merge_update_function_t)(UpdateInfo &info, UnifiedVectorFormat &update, row_t *ids, idx_t count,
	                                        const SelectionVector &sel, idx_t vector_offset);
	typedef void (*save_undo_function_t)(UpdateInfo &source, UpdateInfo &root, row_t *ids, idx_t count,
	                                     const SelectionVector &sel, idx_t vector_offset, UpdateInfo &target);
	typedef void (*rollback_update_function_t)(UpdateInfo &root, UpdateInfo &rollback_info);
	typedef void (*fetch_update_function_t)(const SnapshotView &view, UpdateInfo &info, Vector &result);
	typedef void (*fetch_committed_function_t)(UpdateInfo &info, Vector &result);
	typedef void (*fetch_committed_range_function_t)(const SnapshotView &view, UpdateInfo &info, idx_t start, idx_t end,
	                                                 idx_t result_offset, Vector &result);
	typedef void (*fetch_rows_function_t)(const SnapshotView &view, UpdateInfo &info, const idx_t *offsets,
	                                      const SelectionVector &sel, idx_t fetch_offset, idx_t count,
	                                      idx_t vector_offset, Vector &result, idx_t result_offset);
	typedef void (*statistics_update_function_t)(UpdateSegment *segment, SegmentStatistics &stats,
	                                             UnifiedVectorFormat &update, idx_t count);
	typedef idx_t (*get_effective_updates_t)(UnifiedVectorFormat &update_format, row_t *ids, idx_t count,
	                                         SelectionVector &sel, Vector &base_data, idx_t id_offset);
	typedef void (*store_base_values_function_t)(UpdateInfo &source, Vector &base_data, StringHeap &heap,
	                                             UpdateInfo &target);

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
	store_base_values_function_t store_base_values_function;

private:
	UndoBufferPointer GetUpdateNode(StorageLockKey &lock, idx_t vector_idx) const;
	bool IsUnused(StorageLockKey &lock) const;
	//! Keeps the string heap alive for as long as the result holds strings from it
	void AddHeapReference(Vector &result);
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
