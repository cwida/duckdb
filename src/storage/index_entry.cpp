#include "duckdb/storage/table/index_entry.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"

#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"

namespace duckdb {

IndexEntry::IndexEntry(unique_ptr<Index> index_p, idx_t index_oid_p, const ConstraintCheckMode check_mode_p)
    : index_oid(index_oid_p), owned_index(std::move(index_p)), check_mode(check_mode_p) {
	if (owned_index->IsBound()) {
		bind_state = IndexBindState::BOUND;
	} else {
		bind_state = IndexBindState::UNBOUND;
	}
}

void IndexEntry::Append(DataChunk &chunk, Vector &row_ids) {
	auto entry_lock = lock.GetExclusiveLock();
	if (!owned_index->IsBound()) {
		auto &unbound_index = owned_index->Cast<UnboundIndex>();
		unbound_index.BufferChunk(chunk, row_ids, BufferedIndexReplay::INSERT_ENTRY);
		return;
	}
	auto &bound_index = owned_index->Cast<BoundIndex>();
	bound_index.Append(chunk, row_ids);
}

ErrorData IndexEntry::Append(DataChunk &chunk, Vector &row_ids, const shared_ptr<IndexEntry> &delete_entry,
                             IndexAppendMode append_mode, optional_idx active_checkpoint) {
	auto entry_lock = lock.GetExclusiveLock();
	if (!owned_index->IsBound()) {
		auto &unbound_index = owned_index->Cast<UnboundIndex>();
		unbound_index.BufferChunk(chunk, row_ids, BufferedIndexReplay::INSERT_ENTRY);
		return ErrorData();
	}
	auto &bound_index = owned_index->Cast<BoundIndex>();

	unique_ptr<StorageLockKey> delete_lock;
	optional_ptr<const BoundIndex> delete_index;
	if (bound_index.IsUnique() && delete_entry) {
		delete_lock = delete_entry->lock.GetSharedLock();
		D_ASSERT(delete_entry->owned_index->IsBound());
		delete_index = delete_entry->owned_index->Cast<BoundIndex>();
	}

	bool lookup_main_index = false;
	optional_ptr<BoundIndex> append_index;
	if (bound_index.SupportsDeltaIndexes() && deltas.ShouldUse(active_checkpoint)) {
		append_index = deltas.GetOrCreate(bound_index, IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT);
		if (bound_index.IsUnique()) {
			lookup_main_index = true;
		}
	}

	ErrorData error;
	try {
		if (lookup_main_index) {
			IndexAppendInfo lookup_append_info;
			if (delete_index) {
				lookup_append_info.delete_indexes.push_back(*delete_index);
			}
			if (auto delta = deltas.Find(IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT)) {
				lookup_append_info.delete_indexes.push_back(*delta);
			}
			bound_index.VerifyAppend(chunk, lookup_append_info, optional_ptr<ConflictManager>());
		}

		IndexAppendInfo index_append_info(append_mode, nullptr);
		if (delete_index) {
			index_append_info.delete_indexes.push_back(*delete_index);
		}
		if (append_index) {
			error = append_index->Append(chunk, row_ids, index_append_info);
		} else {
			error = bound_index.Append(chunk, row_ids, index_append_info);
		}
	} catch (std::exception &ex) {
		error = ErrorData(ex);
	}
	return error;
}

void IndexEntry::RevertAppend(DataChunk &chunk, Vector &row_ids) {
	auto entry_lock = lock.GetExclusiveLock();
	if (auto delta = deltas.Find(IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT)) {
		delta->Delete(chunk, row_ids);
		return;
	}
	if (owned_index->IsBound()) {
		owned_index->Cast<BoundIndex>().Delete(chunk, row_ids);
	}
}

void IndexEntry::InitializeLocalIndexes(TableIndexList &delete_indexes, TableIndexList &append_indexes) const {
	InitializeLocalIndexesInternal(delete_indexes, &append_indexes);
}

void IndexEntry::InitializeLocalDeleteIndex(TableIndexList &delete_indexes) const {
	InitializeLocalIndexesInternal(delete_indexes, nullptr);
}

void IndexEntry::InitializeLocalIndexesInternal(TableIndexList &delete_indexes,
                                                optional_ptr<TableIndexList> append_indexes) const {
	auto entry_lock = lock.GetSharedLock();
	if (owned_index->GetConstraintType() == IndexConstraintType::NONE || !owned_index->IsBound()) {
		return;
	}
	auto &bound_index = owned_index->Cast<BoundIndex>();
	if (!bound_index.SupportsDeltaIndexes()) {
		return;
	}

	auto constraint_type = bound_index.GetConstraintType();
	delete_indexes.AddIndex(bound_index.CreateEmptyCopy(constraint_type), index_oid, ConstraintCheckMode::DEFAULT);
	// Deferred constraints have no append index: transaction-local rows are verified when committing.
	if (append_indexes && !IsDeferred()) {
		append_indexes->AddIndex(bound_index.CreateEmptyCopy(constraint_type), index_oid, ConstraintCheckMode::DEFAULT);
	}
}

void IndexEntry::AppendToDeleteIndexes(DataChunk &chunk, Vector &row_ids) {
	auto entry_lock = lock.GetExclusiveLock();
	D_ASSERT(owned_index->IsBound());
	if (!owned_index->IsUnique()) {
		return;
	}
	auto &bound_index = owned_index->Cast<BoundIndex>();
	IndexAppendInfo index_append_info(IndexAppendMode::IGNORE_DUPLICATES, nullptr);
	auto result = bound_index.Append(chunk, row_ids, index_append_info);
	if (result.HasError()) {
		throw InternalException("unexpected constraint violation on delete ART: ", result.Message());
	}
}

static void AppendIndexEntries(BoundIndex &index, DataChunk &chunk, Vector &row_ids) {
	IndexAppendInfo append_info;
	const auto error = index.Append(chunk, row_ids, append_info);
	if (error.HasError()) {
		throw InternalException("Failed to append to %s: %s", index.GetIndexName(), error.Message());
	}
}

static bool TryDeleteAll(BoundIndex &index, DataChunk &chunk, Vector &row_ids) {
	const auto delete_count = index.TryDelete(chunk, row_ids, nullptr, nullptr);
	if (delete_count == 0) {
		return false;
	}
	if (delete_count != chunk.size()) {
		// This should not happen: RemoveFromIndexes works on a per-row-group basis, and appends during a checkpoint
		// always use new row groups, so the two groups of data should be separate.
		throw InternalException("IndexEntry::RemoveFromIndex - partially deleted from the checkpoint delta");
	}
	return true;
}

static void ApplyIndexRemoval(BoundIndex &index, IndexDeltas &deltas, DataChunk &chunk, Vector &row_ids,
                              const IndexRemovalType removal_type) {
	const auto supports_delta_indexes = index.SupportsDeltaIndexes();
	// Not all indexes require delta indexes - if an index does not require this we skip creating and appending to
	// "deleted_rows_in_use".
	switch (removal_type) {
	case IndexRemovalType::MAIN_INDEX_ONLY:
		// Directly remove from the main index without appending to delta indexes.
		index.Delete(chunk, row_ids);
		break;
	case IndexRemovalType::REVERT_MAIN_INDEX_ONLY:
		// Revert main index only append - just add back to the index.
		AppendIndexEntries(index, chunk, row_ids);
		break;
	case IndexRemovalType::MAIN_INDEX:
		// Regular removal from the main index - add rows to the delta index if required.
		if (supports_delta_indexes) {
			auto &deleted_rows = deltas.GetOrCreate(index, IndexDeltaType::DELETED_ROWS_IN_USE);
			AppendIndexEntries(deleted_rows, chunk, row_ids);
		}
		index.Delete(chunk, row_ids);
		break;
	case IndexRemovalType::REVERT_MAIN_INDEX:
		// Revert regular append to the main index - remove from deleted_rows_in_use if we appended there before.
		AppendIndexEntries(index, chunk, row_ids);
		if (supports_delta_indexes) {
			if (auto delta = deltas.Find(IndexDeltaType::DELETED_ROWS_IN_USE)) {
				delta->Delete(chunk, row_ids);
			}
		}
		break;
	case IndexRemovalType::DELETED_ROWS_IN_USE:
		// Remove from the removal index if we appended any rows.
		if (supports_delta_indexes) {
			if (auto delta = deltas.Find(IndexDeltaType::DELETED_ROWS_IN_USE)) {
				delta->Delete(chunk, row_ids);
			}
		}
		break;
	default:
		throw InternalException("Unsupported IndexRemovalType");
	}
}

static void ApplyIndexRemovalDuringCheckpoint(BoundIndex &index, IndexDeltas &deltas, DataChunk &chunk, Vector &row_ids,
                                              const IndexRemovalType removal_type) {
	D_ASSERT(removal_type != IndexRemovalType::DELETED_ROWS_IN_USE);

	switch (removal_type) {
	case IndexRemovalType::MAIN_INDEX_ONLY:
	case IndexRemovalType::MAIN_INDEX: {
		// Removing from the main index cannot happen directly due to the concurrent checkpoint; add the removal to a
		// delta index.
		auto &removed_data = deltas.GetOrCreate(index, IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT);
		auto added_data = deltas.Find(IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT);

		// If we have also added data during this checkpoint, we might need to remove from there instead.
		// We FIRST try to remove from "added_data_during_checkpoint"; any rows that are not there are added to
		// "removed_data_during_checkpoint".
		if (!added_data || !TryDeleteAll(*added_data, chunk, row_ids)) {
			AppendIndexEntries(removed_data, chunk, row_ids);
		}
		if (removal_type == IndexRemovalType::MAIN_INDEX) {
			// MAIN_INDEX also needs to retain the rows in deleted_rows_in_use.
			auto &deleted_rows = deltas.GetOrCreate(index, IndexDeltaType::DELETED_ROWS_IN_USE);
			AppendIndexEntries(deleted_rows, chunk, row_ids);
		}
		break;
	}
	case IndexRemovalType::REVERT_MAIN_INDEX_ONLY:
	case IndexRemovalType::REVERT_MAIN_INDEX: {
		auto &removed_data = deltas.GetOrCreate(index, IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT);
		// Revert adding to the main index.
		// We have added data during this checkpoint as well, so the removal might have EITHER:
		// (1) added data to "removed_data_during_checkpoint" or
		// (2) removed data from "added_data_during_checkpoint".
		// Revert by first trying to remove from "removed_data_during_checkpoint"; any rows that were not removed are
		// re-added to "added_data_during_checkpoint".
		if (auto added_data = deltas.Find(IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT)) {
			if (!TryDeleteAll(removed_data, chunk, row_ids)) {
				AppendIndexEntries(*added_data, chunk, row_ids);
			}
		} else {
			removed_data.Delete(chunk, row_ids);
		}
		if (removal_type == IndexRemovalType::REVERT_MAIN_INDEX) {
			// We also need to remove from "deleted_rows_in_use".
			if (auto delta = deltas.Find(IndexDeltaType::DELETED_ROWS_IN_USE)) {
				delta->Delete(chunk, row_ids);
			}
		}
		break;
	}
	default:
		throw InternalException("Unsupported IndexRemovalType");
	}
}

void IndexEntry::RemoveFromIndex(DataChunk &chunk, Vector &row_ids, const IndexRemovalType removal_type,
                                 const optional_idx active_checkpoint) {
	auto entry_lock = lock.GetExclusiveLock();
	if (!owned_index->IsBound()) {
		// Buffer the delete: chunk is in table layout with all indexed columns populated.
		owned_index->Cast<UnboundIndex>().BufferChunk(chunk, row_ids, BufferedIndexReplay::DEL_ENTRY);
		return;
	}

	auto &bound_index = owned_index->Cast<BoundIndex>();
	// Check which indexes we should append to or remove from. This method might also involve appending to indexes:
	// delta indexes must be filled with data we are removing, or we may be reverting a previous removal.
	// Not all indexes require delta indexes, so those skip "deleted_rows_in_use" bookkeeping.
	if (removal_type == IndexRemovalType::DELETED_ROWS_IN_USE) {
		// Cleanup always removes directly from "deleted_rows_in_use", even during a checkpoint.
		ApplyIndexRemoval(bound_index, deltas, chunk, row_ids, removal_type);
	} else if (bound_index.SupportsDeltaIndexes() && deltas.ShouldUse(active_checkpoint)) {
		// During a checkpoint, route changes through the checkpoint deltas instead of the main index.
		ApplyIndexRemovalDuringCheckpoint(bound_index, deltas, chunk, row_ids, removal_type);
	} else {
		ApplyIndexRemoval(bound_index, deltas, chunk, row_ids, removal_type);
	}
}

bool IndexEntry::IsUnique() const {
	auto entry_lock = lock.GetSharedLock();
	return owned_index->IsUnique();
}

bool IndexEntry::IsDeferred() const {
	return check_mode == ConstraintCheckMode::DEFERRED;
}

bool IndexEntry::IsForeignKeyIndex(const vector<PhysicalIndex> &fk_keys, const ForeignKeyType fk_type) const {
	auto entry_lock = lock.GetSharedLock();
	if (fk_type == ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE) {
		// Foreign keys cannot reference a deferred key.
		if (!owned_index->IsUnique() || IsDeferred()) {
			return false;
		}
	} else if (!owned_index->IsForeign()) {
		return false;
	}
	const auto &column_ids = owned_index->GetColumnIds();
	if (fk_keys.size() != column_ids.size()) {
		return false;
	}

	for (const auto &fk_key : fk_keys) {
		bool found = false;
		for (const auto index_key : column_ids) {
			if (fk_key.index == index_key) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

Identifier IndexEntry::GetName() const {
	auto entry_lock = lock.GetSharedLock();
	return owned_index->GetIndexName();
}

string IndexEntry::GetIndexType() const {
	auto entry_lock = lock.GetSharedLock();
	return owned_index->GetIndexType();
}

bool IndexEntry::HasBufferedReplays() const {
	auto entry_lock = lock.GetSharedLock();
	if (!owned_index || owned_index->IsBound()) {
		return false;
	}
	return owned_index->Cast<UnboundIndex>().HasBufferedReplays();
}

void IndexEntry::Retire() {
	auto entry_lock = lock.GetExclusiveLock();
	deltas.Reset();
	owned_index->ResetStorage();
	owned_index.reset();
	bind_state = IndexBindState::RETIRED;
}

unique_ptr<BoundIndex> IndexEntry::Bind(IndexBinder &binder, const vector<LogicalType> &table_types) {
	auto entry_lock = lock.GetExclusiveLock();
	return owned_index->Cast<UnboundIndex>().Bind(binder, table_types);
}

void IndexEntry::CommitBind(unique_ptr<BoundIndex> bound_index) {
	auto entry_lock = lock.GetExclusiveLock();
	owned_index = std::move(bound_index);
	bind_state = IndexBindState::BOUND;
}

void IndexEntry::VerifyAppend(const shared_ptr<IndexEntry> &delete_entry, DataChunk &chunk,
                              optional_ptr<ConflictManager> manager) {
	auto entry_lock = lock.GetExclusiveLock();
	D_ASSERT(owned_index->IsBound());
	auto &bound_index = owned_index->Cast<BoundIndex>();

	IndexAppendInfo index_append_info;
	unique_ptr<StorageLockKey> delete_lock;
	if (delete_entry) {
		delete_lock = delete_entry->lock.GetSharedLock();
		D_ASSERT(delete_entry->owned_index->IsBound());
		index_append_info.delete_indexes.push_back(delete_entry->owned_index->Cast<BoundIndex>());
	}
	if (!manager) {
		if (auto delta = deltas.Find(IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT)) {
			index_append_info.delete_indexes.push_back(*delta);
		}
	}
	bound_index.VerifyAppend(chunk, index_append_info, manager);
	if (auto delta = deltas.Find(IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT)) {
		// rows committed while a checkpoint runs are only in this delta
		delta->VerifyAppend(chunk, index_append_info, manager);
	}
}

void IndexEntry::VerifyForeignKey(const shared_ptr<IndexEntry> &delete_entry, DataChunk &chunk,
                                  ConflictManager &conflict_manager) {
	auto entry_lock = lock.GetExclusiveLock();
	D_ASSERT(owned_index->IsBound());
	auto &bound_index = owned_index->Cast<BoundIndex>();

	IndexAppendInfo index_append_info;
	unique_ptr<StorageLockKey> delete_lock;
	if (delete_entry) {
		delete_lock = delete_entry->lock.GetSharedLock();
		D_ASSERT(delete_entry->owned_index->IsBound());
		index_append_info.delete_indexes.push_back(delete_entry->owned_index->Cast<BoundIndex>());
	}
	if (auto delta = deltas.Find(IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT)) {
		index_append_info.delete_indexes.push_back(*delta);
	}

	bound_index.VerifyConstraint(chunk, index_append_info, conflict_manager);
	if (auto delta = deltas.Find(IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT)) {
		// if we have added any rows during checkpoint - check in that index as well
		IndexAppendInfo added_during_checkpoint_info;
		delta->VerifyConstraint(chunk, added_during_checkpoint_info, conflict_manager);
	}
}

string IndexEntry::GetConstraintViolationMessage(const VerifyExistenceType verify_type, const idx_t failed_index,
                                                 DataChunk &input) const {
	auto entry_lock = lock.GetSharedLock();
	D_ASSERT(owned_index->IsBound());
	return owned_index->Cast<BoundIndex>().GetConstraintViolationMessage(verify_type, failed_index, input);
}

void IndexEntry::VerifyUpdate(const vector<PhysicalIndex> &column_ids) const {
#ifdef DEBUG
	auto entry_lock = lock.GetSharedLock();
	D_ASSERT(owned_index->IsBound());
	const auto &bound_index = owned_index->Cast<BoundIndex>();
	D_ASSERT(!bound_index.IndexIsUpdated(column_ids));
#endif
}

void IndexEntry::Vacuum() {
	auto entry_lock = lock.GetExclusiveLock();
	if (owned_index->IsBound()) {
		owned_index->Cast<BoundIndex>().Vacuum();
	}
}

void IndexEntry::Rebuild(const IndexRebuildScan &scan) {
	auto entry_lock = lock.GetExclusiveLock();
	if (!owned_index->IsBound()) {
		throw InternalException("RebuildIndexes expects all indexes to be bound during checkpoint");
	}

	auto &bound_index = owned_index->Cast<BoundIndex>();
	bound_index.ResetStorage();

	IndexRebuildAppend append = [&](DataChunk &chunk, Vector &row_ids) {
		auto error = bound_index.Append(chunk, row_ids);
		if (error.HasError()) {
			throw InternalException("Failed to rebuild index %s after vacuum: %s", bound_index.GetIndexName(),
			                        error.Message());
		}
	};
	scan(bound_index.GetColumnIds(), append);
	bound_index.Verify();
}

void IndexEntry::RemapRowIds(const IndexRemapScan &scan) {
	auto entry_lock = lock.GetExclusiveLock();
	D_ASSERT(owned_index->IsBound());
	auto &bound_index = owned_index->Cast<BoundIndex>();

	// Delete old rowids first to avoid same-key rowid collisions within the task.
	scan([&](DataChunk &chunk, Vector &old_row_ids, Vector &) { bound_index.Delete(chunk, old_row_ids); });
	// Remapping must not re-run uniqueness checks for already-validated rows.
	scan([&](DataChunk &chunk, Vector &, Vector &new_row_ids) {
		IndexAppendInfo append_info(IndexAppendMode::INSERT_DUPLICATES, nullptr);
		auto error = bound_index.Append(chunk, new_row_ids, append_info);
		if (error.HasError()) {
			error.Throw();
		}
	});
}

void IndexEntry::VerifyBuffers() {
	auto entry_lock = lock.GetExclusiveLock();
	if (auto delta = deltas.Find(IndexDeltaType::DELETED_ROWS_IN_USE)) {
		delta->VerifyBuffers();
	}
	if (owned_index->IsBound()) {
		owned_index->Cast<BoundIndex>().VerifyBuffers();
	}
}

IndexInfo IndexEntry::GetStorageInfo() const {
	auto entry_lock = lock.GetSharedLock();
	IndexInfo result;
	result.is_primary = owned_index->IsPrimary();
	result.is_unique = owned_index->IsUnique() || result.is_primary;
	result.is_foreign = owned_index->IsForeign();
	result.check_mode = check_mode;
	result.column_set = owned_index->GetColumnIdSet();
	return result;
}

idx_t IndexEntry::GetInMemorySize() const {
	auto entry_lock = lock.GetSharedLock();
	if (!owned_index->IsBound()) {
		return 0;
	}
	return owned_index->Cast<BoundIndex>().GetInMemorySize();
}

IndexCheckpointResult IndexEntry::Checkpoint(TableIndexWriter &index_writer) {
	auto entry_lock = lock.GetExclusiveLock();
	D_ASSERT(owned_index);

	auto storage_version = index_writer.GetStorageVersion();
	if (owned_index->GetCheckpointMode() == IndexCheckpointMode::DEFERRED) {
		if (owned_index->IsBound() && !owned_index->Cast<BoundIndex>().SupportsDeltaIndexes()) {
			throw InternalException("Deferred index checkpointing requires delta index support");
		}
		return owned_index->Checkpoint(index_writer.GetSharedPartialBlockManager(), storage_version);
	}

	auto partial_block_manager = index_writer.CreatePerIndexPartialBlockManager();
	auto checkpoint = owned_index->Checkpoint(partial_block_manager, storage_version);
	partial_block_manager.FlushPartialBlocks();

	InstallShadowInternal(std::move(checkpoint.shadow_index));
	return checkpoint;
}

void IndexEntry::InstallShadow(unique_ptr<BoundIndex> shadow_index) {
	auto entry_lock = lock.GetExclusiveLock();
	InstallShadowInternal(std::move(shadow_index));
}

void IndexEntry::InstallShadowInternal(unique_ptr<BoundIndex> shadow_index) {
	if (!shadow_index) {
		return;
	}
	D_ASSERT(owned_index && owned_index->IsBound());
	owned_index = std::move(shadow_index);
}

IndexStorageInfo IndexEntry::SerializeToWAL(const StorageVersion version) {
	auto entry_lock = lock.GetExclusiveLock();
	// We never write an unbound index to the WAL.
	D_ASSERT(owned_index->IsBound());
	return owned_index->Cast<BoundIndex>().SerializeToWAL(version);
}

void IndexEntry::MergeCheckpointDeltas(const optional_idx checkpoint_id) {
	auto entry_lock = lock.GetExclusiveLock();
	// Merge any data appended to the index while the checkpoint was running.
	if (!owned_index->IsBound()) {
		return;
	}
	auto &bound_index = owned_index->Cast<BoundIndex>();
	auto error = deltas.MergeCheckpointDeltas(bound_index);
	if (error.HasError()) {
		throw InternalException("Failed to merge checkpoint delta - this signifies a bug or broken index: %s",
		                        error.Message());
	}
	deltas.MarkWritten(checkpoint_id);
}

const unique_ptr<BoundIndex> &IndexDeltas::GetPointer(const IndexDeltaType type) const {
	switch (type) {
	case IndexDeltaType::DELETED_ROWS_IN_USE:
		return deleted_rows_in_use;
	case IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT:
		return checkpoint.added_data;
	case IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT:
		return checkpoint.removed_data;
	}
	throw InternalException("Unsupported index delta type");
}

unique_ptr<BoundIndex> &IndexDeltas::GetPointer(const IndexDeltaType type) {
	switch (type) {
	case IndexDeltaType::DELETED_ROWS_IN_USE:
		return deleted_rows_in_use;
	case IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT:
		return checkpoint.added_data;
	case IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT:
		return checkpoint.removed_data;
	}
	throw InternalException("Unsupported index delta type");
}

optional_ptr<const BoundIndex> IndexDeltas::Find(const IndexDeltaType type) const {
	return GetPointer(type).get();
}

optional_ptr<BoundIndex> IndexDeltas::Find(const IndexDeltaType type) {
	return GetPointer(type).get();
}

BoundIndex &IndexDeltas::GetOrCreate(BoundIndex &index, const IndexDeltaType type) {
	auto &delta = GetPointer(type);
	if (!delta) {
		D_ASSERT(index.SupportsDeltaIndexes());
		auto constraint_type = index.GetConstraintType();
		if (type == IndexDeltaType::DELETED_ROWS_IN_USE) {
			// deleted_rows_in_use allows duplicates regardless of whether the main index is unique
			constraint_type = IndexConstraintType::NONE;
		}
		delta = index.CreateEmptyCopy(constraint_type);
		D_ASSERT(delta);
	}
	return *delta;
}

bool IndexDeltas::ShouldUse(const optional_idx active_checkpoint) const {
	if (!active_checkpoint.IsValid()) {
		return false;
	}
	if (!checkpoint.last_written_checkpoint.IsValid()) {
		return true;
	}
	return active_checkpoint.GetIndex() != checkpoint.last_written_checkpoint.GetIndex();
}

ErrorData IndexDeltas::MergeCheckpointDeltas(BoundIndex &index) {
	for (const auto type :
	     {IndexDeltaType::REMOVED_DATA_DURING_CHECKPOINT, IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT}) {
		auto &delta = GetPointer(type);
		if (!delta) {
			continue;
		}
		auto error = index.MergeCheckpointDelta(type, *delta);
		if (error.HasError()) {
			return error;
		}
		delta.reset();
	}
	return ErrorData();
}

void IndexDeltas::MarkWritten(const optional_idx checkpoint_id) {
	checkpoint.last_written_checkpoint = checkpoint_id;
}

void IndexDeltas::Reset() {
	deleted_rows_in_use.reset();
	checkpoint.added_data.reset();
	checkpoint.removed_data.reset();
	checkpoint.last_written_checkpoint = optional_idx();
}

} // namespace duckdb
