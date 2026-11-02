//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_version_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/storage/checkpoint/row_group_writer.hpp"

namespace duckdb {

struct DeleteInfo;
class MetadataManager;
class BufferManager;
struct MetaBlockPointer;

class RowVersionManager {
public:
	explicit RowVersionManager(BufferManager &buffer_manager) noexcept;

	//! Returns the number of non-deleted rows in this segment
	idx_t GetRowCount(ScanOptions options, idx_t count);
	//! Count visible rows starting at a vector boundary, including a partial final vector
	idx_t GetRowCount(ScanOptions options, idx_t start_vector, idx_t count);

	idx_t GetSelVector(ScanOptions options, idx_t vector_idx, SelectionVector &sel_vector, idx_t max_count);
	//! Bulk visibility check. Returns the number of visible rows.
	idx_t GetVisibleRows(TransactionData transaction, const idx_t *offsets, idx_t count, SelectionVector &visible_sel);

	void AppendVersionInfo(TransactionData transaction, idx_t count, idx_t row_group_start, idx_t row_group_end);
	void CommitAppend(transaction_t commit_id, idx_t row_group_start, idx_t count);
	void RevertAppend(idx_t new_count);
	void CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t row_group_start, idx_t count);

	//! Writes a delete or an update into the rows of the vector (see ChunkVectorInfo::WriteRows)
	void WriteRows(idx_t vector_idx, TransactionData transaction, const row_t ids[], idx_t count, row_t offset,
	               bool is_update, WrittenRows &written);
	//! Replaces the ids of old_id on the rows of a write by those of new_id (see ChunkVectorInfo::CommitWrite)
	void CommitWrite(transaction_t old_id, transaction_t new_id, const DeleteInfo &info, bool is_delete);
	//! Rolls back a write (see ChunkVectorInfo::RollbackWrite)
	void RollbackWrite(idx_t vector_idx, const uint16_t rows[], idx_t count, const transaction_t previous_ids[],
	                   idx_t previous_id_count);

	//! Attempts to compress the per-row insert/delete ids of each vector into constants. Ids that precede
	//! lowest_visibility_bound look the same to every active and future transaction, so they can collapse.
	//! Cheap when nothing can have changed: the pass only runs when version ids were modified since the
	//! last pass, or when a previous pass left ids that can still compress once older transactions finish
	void CompressVersionIds(VisibilityBound lowest_visibility_bound);

	vector<MetaBlockPointer> Checkpoint(RowGroupWriter &writer);
	static shared_ptr<RowVersionManager> Deserialize(MetaBlockPointer delete_pointer, MetadataManager &manager);

	bool HasUnserializedChanges(VisibilityBound bound);
	bool HasDeletes();
	bool HasUncommittedChanges();
	vector<MetaBlockPointer> GetStoragePointers();

private:
	mutex version_lock;
	FixedSizeAllocator allocator;
	vector<unique_ptr<ChunkVectorInfo>> vector_info;
	//! The newest and the oldest delete commit that has not been written by a checkpoint yet
	optional_idx newest_uncheckpointed_delete_commit;
	optional_idx oldest_uncheckpointed_delete_commit;
	vector<MetaBlockPointer> storage_pointers;
	//! Whether a compression pass may achieve anything: set when version ids are modified, cleared when a
	//! pass finds no ids that could still compress. For deserialized version info this is derived from the
	//! deserialized content (with the current storage format checkpointed ids are always settled).
	bool needs_compression_check = false;

private:
	FixedSizeAllocator &GetAllocator() {
		return allocator;
	}
	optional_ptr<ChunkVectorInfo> GetChunkInfo(idx_t vector_idx);
	ChunkVectorInfo &GetVectorInfo(idx_t vector_idx);
	void FillVectorInfo(idx_t vector_idx);
};

} // namespace duckdb
