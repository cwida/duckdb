#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/transaction_data.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/metadata/metadata_writer.hpp"
#include "duckdb/storage/checkpoint/row_group_writer.hpp"

namespace duckdb {

RowVersionManager::RowVersionManager(BufferManager &buffer_manager_p) noexcept
    : allocator(STANDARD_VECTOR_SIZE * sizeof(transaction_t), buffer_manager_p.GetTemporaryBlockManager(),
                MemoryTag::BASE_TABLE) {
}

idx_t RowVersionManager::GetRowCount(ScanOptions options, idx_t count) {
	return GetRowCount(options, 0, count);
}

idx_t RowVersionManager::GetRowCount(ScanOptions options, idx_t start_vector, idx_t count) {
	lock_guard<mutex> l(version_lock);
	idx_t total_count = 0;
	// Only visit version vectors belonging to this scan assignment.
	for (idx_t r = 0, i = start_vector; r < count; r += STANDARD_VECTOR_SIZE, i++) {
		idx_t segment_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - r);
		if (segment_count == 0) {
			break;
		}
		if (i >= vector_info.size() || !vector_info[i]) {
			// no version info - this means all rows are visible
			total_count += segment_count;
			continue;
		}
		idx_t row_count = vector_info[i]->GetRowCount(options, segment_count);
		total_count += row_count;
	}
	return total_count;
}

optional_ptr<ChunkVectorInfo> RowVersionManager::GetChunkInfo(idx_t vector_idx) {
	if (vector_idx >= vector_info.size()) {
		return nullptr;
	}
	return vector_info[vector_idx].get();
}

idx_t RowVersionManager::GetSelVector(ScanOptions options, idx_t vector_idx, SelectionVector &sel_vector,
                                      idx_t max_count) {
	lock_guard<mutex> l(version_lock);
	auto chunk_info = GetChunkInfo(vector_idx);
	if (!chunk_info) {
		return max_count;
	}
	return chunk_info->GetSelVector(options, sel_vector, max_count);
}

idx_t RowVersionManager::GetVisibleRows(TransactionData transaction, const idx_t *offsets, idx_t count,
                                        SelectionVector &visible_sel) {
	if (count == 0) {
		return 0;
	}
	const lock_guard<mutex> lock(version_lock);
	idx_t visible_count = 0;
	idx_t idx = 0;
	while (idx < count) {
		const idx_t vector_idx = offsets[idx] / STANDARD_VECTOR_SIZE;
		const idx_t base = vector_idx * STANDARD_VECTOR_SIZE;
		const idx_t vector_end = base + STANDARD_VECTOR_SIZE;
		// The first position after the current same-version-vector run.
		idx_t next_idx = idx + 1;
		// Extend the run while offsets remain within the same version-info vector.
		while (next_idx < count && offsets[next_idx] >= base && offsets[next_idx] < vector_end) {
			next_idx++;
		}
		auto info = GetChunkInfo(vector_idx);
		if (!info) {
			// no version info, which means entire chunk is visible
			for (idx_t k = idx; k < next_idx; k++) {
				visible_sel.set_index(visible_count++, k);
			}
		} else {
			for (idx_t k = idx; k < next_idx; k++) {
				if (info->Fetch(transaction, NumericCast<row_t>(offsets[k] - base))) {
					visible_sel.set_index(visible_count++, k);
				}
			}
		}
		idx = next_idx;
	}
	return visible_count;
}

void RowVersionManager::FillVectorInfo(idx_t vector_idx) {
	if (vector_idx < vector_info.size()) {
		return;
	}
	vector_info.reserve(vector_idx + 1);
	for (idx_t i = vector_info.size(); i <= vector_idx; i++) {
		vector_info.emplace_back();
	}
}

void RowVersionManager::AppendVersionInfo(TransactionData transaction, idx_t count, idx_t row_group_start,
                                          idx_t row_group_end) {
	lock_guard<mutex> lock(version_lock);
	needs_compression_check = true;
	idx_t start_vector_idx = row_group_start / STANDARD_VECTOR_SIZE;
	idx_t end_vector_idx = (row_group_end - 1) / STANDARD_VECTOR_SIZE;

	// fill-up vector_info
	FillVectorInfo(end_vector_idx);

	// insert the version info nodes
	for (idx_t vector_idx = start_vector_idx; vector_idx <= end_vector_idx; vector_idx++) {
		idx_t vector_start =
		    vector_idx == start_vector_idx ? row_group_start - start_vector_idx * STANDARD_VECTOR_SIZE : 0;
		idx_t vector_end =
		    vector_idx == end_vector_idx ? row_group_end - end_vector_idx * STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE;
		if (vector_start == 0 && vector_end == STANDARD_VECTOR_SIZE) {
			// entire vector is encapsulated by append: store a single constant insert id
			vector_info[vector_idx] = make_uniq<ChunkVectorInfo>(allocator, vector_idx * STANDARD_VECTOR_SIZE,
			                                                     transaction.GetTransactionId());
		} else {
			// part of a vector is encapsulated: append to that part
			if (!vector_info[vector_idx]) {
				// first time appending to this vector: create new info
				vector_info[vector_idx] = make_uniq<ChunkVectorInfo>(allocator, vector_idx * STANDARD_VECTOR_SIZE);
			}
			vector_info[vector_idx]->Append(vector_start, vector_end, transaction.GetTransactionId());
		}
	}
}

void RowVersionManager::CommitAppend(transaction_t commit_id, idx_t row_group_start, idx_t count) {
	if (count == 0) {
		return;
	}
	idx_t row_group_end = row_group_start + count;

	lock_guard<mutex> lock(version_lock);
	idx_t start_vector_idx = row_group_start / STANDARD_VECTOR_SIZE;
	idx_t end_vector_idx = (row_group_end - 1) / STANDARD_VECTOR_SIZE;
	for (idx_t vector_idx = start_vector_idx; vector_idx <= end_vector_idx; vector_idx++) {
		idx_t vstart = vector_idx == start_vector_idx ? row_group_start - start_vector_idx * STANDARD_VECTOR_SIZE : 0;
		idx_t vend =
		    vector_idx == end_vector_idx ? row_group_end - end_vector_idx * STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE;
		auto &info = *vector_info[vector_idx];
		info.CommitAppend(commit_id, vstart, vend);
	}
}

void RowVersionManager::CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t row_group_start, idx_t count) {
	if (count == 0) {
		return;
	}
	idx_t row_group_end = row_group_start + count;

	lock_guard<mutex> lock(version_lock);
	idx_t start_vector_idx = row_group_start / STANDARD_VECTOR_SIZE;
	idx_t end_vector_idx = (row_group_end - 1) / STANDARD_VECTOR_SIZE;
	for (idx_t vector_idx = start_vector_idx; vector_idx <= end_vector_idx; vector_idx++) {
		idx_t vcount =
		    vector_idx == end_vector_idx ? row_group_end - end_vector_idx * STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE;
		if (vcount != STANDARD_VECTOR_SIZE) {
			// not written fully - skip
			continue;
		}
		if (vector_idx >= vector_info.size() || !vector_info[vector_idx]) {
			// already vacuumed - skip
			continue;
		}
		auto &info = *vector_info[vector_idx];
		// if we wrote the entire chunk info try to compress it
		auto cleanup = info.Cleanup(lowest_visibility_bound);
		if (cleanup) {
			vector_info[vector_idx].reset();
		}
	}
}

void RowVersionManager::RevertAppend(idx_t new_count) {
	lock_guard<mutex> lock(version_lock);
	idx_t partial_vector_idx = new_count / STANDARD_VECTOR_SIZE;
	idx_t partial_vector_count = new_count % STANDARD_VECTOR_SIZE;
	if (partial_vector_count > 0 && partial_vector_idx < vector_info.size() && vector_info[partial_vector_idx]) {
		// the reverted rows of the partially kept vector must not keep the insert id of the reverted append
		needs_compression_check = true;
		vector_info[partial_vector_idx]->RevertAppend(partial_vector_count);
	}
	idx_t start_vector_idx = (new_count + (STANDARD_VECTOR_SIZE - 1)) / STANDARD_VECTOR_SIZE;
	for (idx_t vector_idx = start_vector_idx; vector_idx < vector_info.size(); vector_idx++) {
		vector_info[vector_idx].reset();
	}
}

ChunkVectorInfo &RowVersionManager::GetVectorInfo(idx_t vector_idx) {
	FillVectorInfo(vector_idx);

	if (!vector_info[vector_idx]) {
		// no info yet: create it
		vector_info[vector_idx] = make_uniq<ChunkVectorInfo>(allocator, vector_idx * STANDARD_VECTOR_SIZE);
	}
	return *vector_info[vector_idx];
}

void RowVersionManager::WriteRows(idx_t vector_idx, TransactionData transaction, const row_t ids[], idx_t count,
                                  row_t offset, bool is_update, WrittenRows &written) {
	lock_guard<mutex> lock(version_lock);
	needs_compression_check = true;
	GetVectorInfo(vector_idx).WriteRows(transaction, ids, count, offset, is_update, written);
}

void RowVersionManager::CommitWrite(transaction_t old_id, transaction_t new_id, const DeleteInfo &info,
                                    bool is_delete) {
	lock_guard<mutex> lock(version_lock);
	needs_compression_check = true;
	if (is_delete && IsCommitted(new_id)) {
		// a reverted commit (new_id is the transaction id again) changes nothing a checkpoint writes
		if (!newest_uncheckpointed_delete_commit.IsValid() || new_id > newest_uncheckpointed_delete_commit.GetIndex()) {
			newest_uncheckpointed_delete_commit = new_id;
		}
		if (!oldest_uncheckpointed_delete_commit.IsValid() || new_id < oldest_uncheckpointed_delete_commit.GetIndex()) {
			oldest_uncheckpointed_delete_commit = new_id;
		}
	}
	GetVectorInfo(info.vector_idx).CommitWrite(old_id, new_id, info.GetStoredRows(), info.count);
}

void RowVersionManager::RollbackWrite(idx_t vector_idx, const uint16_t rows[], idx_t count,
                                      const transaction_t previous_ids[], idx_t previous_id_count) {
	lock_guard<mutex> lock(version_lock);
	needs_compression_check = true;
	GetVectorInfo(vector_idx).RollbackWrite(rows, count, previous_ids, previous_id_count);
}

void RowVersionManager::CompressVersionIds(VisibilityBound lowest_visibility_bound) {
	lock_guard<mutex> lock(version_lock);
	if (!needs_compression_check) {
		// no version ids were modified since the last pass, and the last pass left nothing
		// that could still become compressible - nothing to do
#ifdef DEBUG
		// a cleared manager-level check implies every vector's check is disarmed - verify the
		// per-vector claims that the skipped pass relies on
		for (auto &info : vector_info) {
			if (info) {
				D_ASSERT(!info->RecheckCompression());
				info->VerifyCachedCompressionState();
			}
		}
#endif
		return;
	}
	bool pending = false;
	for (auto &info : vector_info) {
		if (info && info->CompressVersionIds(lowest_visibility_bound) == VersionCompressionResult::PENDING) {
			// some ids can still compress once the lowest active start advances - check again next pass
			pending = true;
		}
	}
	needs_compression_check = pending;
	// compression frees the per-row id segments - release any buffers that are now empty
	// (Free keeps the last buffer with free space alive to prevent buffer creation fluctuation)
	allocator.RemoveEmptyBuffers();
}

vector<MetaBlockPointer> RowVersionManager::Checkpoint(RowGroupWriter &writer) {
	lock_guard<mutex> lock(version_lock);
	auto &manager = *writer.GetMetadataManager();
	auto options = writer.GetCheckpointOptions();
	if (!oldest_uncheckpointed_delete_commit.IsValid() ||
	    oldest_uncheckpointed_delete_commit.GetIndex() >= options.visibility_bound) {
		// nothing below the bound changed since the last checkpoint; a delete above it is written by the next one
		// ensure the blocks we are pointing to are not marked as free
		manager.ClearModifiedBlocks(storage_pointers);
		// return the current set of pointers
		return storage_pointers;
	}
	// first count how many chunk infos we need to serialize
	vector<pair<idx_t, reference<ChunkVectorInfo>>> to_serialize;
	for (idx_t vector_idx = 0; vector_idx < vector_info.size(); vector_idx++) {
		auto chunk_info = vector_info[vector_idx].get();
		if (!chunk_info) {
			continue;
		}
		if (!chunk_info->HasDeletes(options.visibility_bound)) {
			continue;
		}
		to_serialize.emplace_back(vector_idx, *chunk_info);
	}

	storage_pointers.clear();

	if (!to_serialize.empty()) {
		MetadataWriter metadata_writer(manager, &storage_pointers);
		// now serialize the actual version information
		metadata_writer.Write<idx_t>(to_serialize.size());
		for (auto &entry : to_serialize) {
			auto &vector_idx = entry.first;
			auto &chunk_info = entry.second.get();
			metadata_writer.Write<idx_t>(vector_idx);
			chunk_info.Write(metadata_writer, options.visibility_bound);
		}
		metadata_writer.Flush();
	}

	if (newest_uncheckpointed_delete_commit.IsValid() &&
	    newest_uncheckpointed_delete_commit.GetIndex() < options.visibility_bound) {
		// the last checkpointed id was either before or on the transaction we are checkpointing
		// nothing to checkpoint in future commits until more deletes appear
		newest_uncheckpointed_delete_commit = optional_idx();
		oldest_uncheckpointed_delete_commit = optional_idx();
	}
	return storage_pointers;
}

shared_ptr<RowVersionManager> RowVersionManager::Deserialize(MetaBlockPointer delete_pointer,
                                                             MetadataManager &manager) {
	if (!delete_pointer.IsValid()) {
		return nullptr;
	}
	auto version_info = make_shared_ptr<RowVersionManager>(manager.GetBufferManager());
	MetadataReader source(manager, delete_pointer, &version_info->storage_pointers);
	auto chunk_count = source.Read<idx_t>();
	D_ASSERT(chunk_count > 0);
	for (idx_t i = 0; i < chunk_count; i++) {
		idx_t vector_index = source.Read<idx_t>();
		if (vector_index * STANDARD_VECTOR_SIZE >= Storage::MAX_ROW_GROUP_SIZE) {
			throw IOException("In DeserializeDeletes, vector_index %llu is out of range for the max row group size of "
			                  "%llu. Corrupted file?",
			                  vector_index, Storage::MAX_ROW_GROUP_SIZE);
		}

		version_info->FillVectorInfo(vector_index);
		auto info = ChunkVectorInfo::Read(version_info->GetAllocator(), source);
		if (info && info->RecheckCompression()) {
			// with the current storage format deserialized ids are always settled, but Read
			// derives this from the deserialized content - follow its verdict
			version_info->needs_compression_check = true;
		}
		version_info->vector_info[vector_index] = std::move(info);
	}
	version_info->newest_uncheckpointed_delete_commit = optional_idx();
	version_info->oldest_uncheckpointed_delete_commit = optional_idx();
	return version_info;
}

bool RowVersionManager::HasUnserializedChanges(VisibilityBound bound) {
	lock_guard<mutex> lock(version_lock);
	return oldest_uncheckpointed_delete_commit.IsValid() && oldest_uncheckpointed_delete_commit.GetIndex() < bound;
}

bool RowVersionManager::HasDeletes() {
	lock_guard<mutex> lock(version_lock);
	for (auto &info : vector_info) {
		if (info && info->AnyDeleted()) {
			return true;
		}
	}
	return false;
}

bool RowVersionManager::HasUncommittedChanges() {
	lock_guard<mutex> lock(version_lock);
	for (auto &info : vector_info) {
		if (info && info->HasUncommittedChanges()) {
			return true;
		}
	}
	return false;
}

vector<MetaBlockPointer> RowVersionManager::GetStoragePointers() {
	lock_guard<mutex> lock(version_lock);
	D_ASSERT(!newest_uncheckpointed_delete_commit.IsValid());
	return storage_pointers;
}

} // namespace duckdb
