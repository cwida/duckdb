//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/index_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/constraint_check_mode.hpp"
#include "duckdb/common/enums/index_removal_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_lock.hpp"

#include <functional>
#include <memory>

namespace duckdb {

class ConflictManager;
class IndexEntry;
class IndexBinder;
class TableIndexList;
class TableIndexWriter;
struct IndexStorageInfo;

//! IndexBindState transitions index binding phases and marks entries whose physical index has been destroyed.
enum class IndexBindState : uint8_t { UNBOUND, BINDING, BOUND, RETIRED };

using IndexRebuildAppend = std::function<void(DataChunk &chunk, Vector &row_ids)>;
using IndexRebuildScan = std::function<void(const vector<column_t> &column_ids, const IndexRebuildAppend &append)>;
using IndexRemapApply = std::function<void(DataChunk &chunk, Vector &old_row_ids, Vector &new_row_ids)>;
using IndexRemapScan = std::function<void(const IndexRemapApply &apply)>;

//! Owns the optional indexes used to represent transaction and checkpoint deltas for an IndexEntry.
class IndexDeltas {
public:
	optional_ptr<const BoundIndex> Find(IndexDeltaType type) const;
	optional_ptr<BoundIndex> Find(IndexDeltaType type);
	BoundIndex &GetOrCreate(BoundIndex &index, IndexDeltaType type);
	bool ShouldUse(optional_idx active_checkpoint) const;
	ErrorData MergeCheckpointDeltas(BoundIndex &index);
	void MarkWritten(optional_idx checkpoint_id);
	void Reset();

private:
	const unique_ptr<BoundIndex> &GetPointer(IndexDeltaType type) const;
	unique_ptr<BoundIndex> &GetPointer(IndexDeltaType type);

	struct CheckpointDeltas {
		optional_idx last_written_checkpoint;
		unique_ptr<BoundIndex> added_data;
		unique_ptr<BoundIndex> removed_data;
	};

	unique_ptr<BoundIndex> deleted_rows_in_use;
	CheckpointDeltas checkpoint;
};

//! IndexReadHandle provides shared access to a stable physical index.
//! Other readers can access the entry concurrently, while replacement and entry-level mutations are blocked.
template <class TARGET>
class IndexReadHandle {
public:
	IndexReadHandle(IndexReadHandle &&) = default;
	IndexReadHandle &operator=(IndexReadHandle &&) = delete;

	const TARGET *operator->() const &;
	const TARGET *operator->() const && = delete;

	optional_ptr<const TARGET> FindDelta(IndexDeltaType type) const &;
	optional_ptr<const TARGET> FindDelta(IndexDeltaType type) const && = delete;

private:
	friend class IndexEntry;
	IndexReadHandle(shared_ptr<IndexEntry> entry, unique_ptr<StorageLockKey> lock);

	bool IsValid() const;
	const IndexEntry &GetEntry() const;

	//! Declared before lock so the lock is released before entry ownership is released.
	shared_ptr<IndexEntry> entry;
	unique_ptr<StorageLockKey> lock;
};

//! IndexWriteHandle provides exclusive access to the physical index and checkpoint state.
template <class TARGET>
class IndexWriteHandle {
public:
	IndexWriteHandle(IndexWriteHandle &&) = default;
	IndexWriteHandle &operator=(IndexWriteHandle &&) = delete;

	TARGET *operator->() &;
	TARGET *operator->() && = delete;

private:
	friend class IndexEntry;
	IndexWriteHandle(shared_ptr<IndexEntry> entry, unique_ptr<StorageLockKey> lock);

	bool IsValid() const;
	IndexEntry &GetMutableEntry();

	//! Declared before lock so the lock is released before entry ownership is released.
	shared_ptr<IndexEntry> entry;
	unique_ptr<StorageLockKey> lock;
};

//! IndexEntry contains an atomic in addition to the index to ensure correct binding.
//! The IndexEntry provides a stable logical identity which refers to an interchangeable snapshot of an index.
class IndexEntry : public enable_shared_from_this<IndexEntry> {
public:
	IndexEntry(unique_ptr<Index> index, idx_t index_oid, ConstraintCheckMode check_mode);
	//! Append a chunk to the physical index, buffering it while the index is unbound.
	void Append(DataChunk &chunk, Vector &row_ids);
	//! Appends a chunk using delete and checkpoint indexes where required.
	ErrorData Append(DataChunk &chunk, Vector &row_ids, const shared_ptr<IndexEntry> &delete_entry,
	                 IndexAppendMode append_mode, optional_idx active_checkpoint);
	//! Reverts an append to the physical index or its checkpoint delta.
	void RevertAppend(DataChunk &chunk, Vector &row_ids);
	//! Appends deleted rows to the bound physical index if it enforces uniqueness.
	void AppendToDeleteIndexes(DataChunk &chunk, Vector &row_ids);
	//! Applies a removal or removal rollback to the physical index and its deltas.
	void RemoveFromIndex(DataChunk &chunk, Vector &row_ids, IndexRemovalType removal_type,
	                     optional_idx active_checkpoint);
	//! Returns whether the physical index enforces a unique constraint.
	bool IsUnique() const;
	//! Returns whether the constraint enforced by the physical index is only checked when committing.
	bool IsDeferred() const;
	//! Returns whether the physical index matches the foreign key columns and role.
	bool IsForeignKeyIndex(const vector<PhysicalIndex> &fk_keys, ForeignKeyType fk_type) const;
	//! Returns the name of the physical index.
	Identifier GetName() const;
	//! Returns the physical index type.
	string GetIndexType() const;
	//! Returns whether an unbound index has buffered WAL operations.
	bool HasBufferedReplays() const;
	//! Destroys the physical index.
	void Retire();
	//! Binds the unbound physical index without replacing it.
	unique_ptr<BoundIndex> Bind(IndexBinder &binder, const vector<LogicalType> &table_types);
	//! Replaces the unbound physical index with its bound representation.
	void CommitBind(unique_ptr<BoundIndex> bound_index);
	//! Verifies that rows can be appended to the bound physical index.
	void VerifyAppend(const shared_ptr<IndexEntry> &delete_entry, DataChunk &chunk,
	                  optional_ptr<ConflictManager> manager);
	//! Verifies a foreign key constraint against the physical index and its checkpoint deltas.
	void VerifyForeignKey(const shared_ptr<IndexEntry> &delete_entry, DataChunk &chunk,
	                      ConflictManager &conflict_manager);
	//! Constructs the physical index's constraint violation message.
	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index, DataChunk &input) const;
	//! Verifies that the physical index is not updated by the given columns.
	void VerifyUpdate(const vector<PhysicalIndex> &column_ids) const;
	//! Vacuums the physical index if it is bound.
	void Vacuum();
	//! Rebuilds the bound physical index with chunks supplied by the scan callback.
	void Rebuild(const IndexRebuildScan &scan);
	//! Holds the exclusive entry lock while invoking scan once for deletes and once for appends.
	//! The scan callback must not re-enter this IndexEntry.
	void RemapRowIds(const IndexRemapScan &scan);
	//! Verifies the buffers of the physical index and its delete delta.
	void VerifyBuffers();
	//! Returns a copy of the physical index's table storage metadata.
	IndexInfo GetStorageInfo() const;
	//! Returns the in-memory size of the physical index, or zero if it is unbound.
	idx_t GetInMemorySize() const;
	//! Persist the index through the index writer and optionally returns a shadow index.
	IndexCheckpointResult Checkpoint(TableIndexWriter &writer);
	//! Install a shadow index in place of the current live index.
	void InstallShadow(unique_ptr<BoundIndex> shadow_index);
	//! Serializes the bound physical index for the write-ahead log.
	IndexStorageInfo SerializeToWAL(const StorageVersion version);
	//! Merges checkpoint deltas into the bound physical index and marks the checkpoint as written.
	void MergeCheckpointDeltas(optional_idx checkpoint_id);
	//! Adds the required transaction-local delete and append indexes.
	void InitializeLocalIndexes(TableIndexList &delete_indexes, TableIndexList &append_indexes) const;
	//! Adds only the transaction-local delete index.
	void InitializeLocalDeleteIndex(TableIndexList &delete_indexes) const;

public:
	//! Acquire shared access to a stable physical index.
	template <class TARGET>
	IndexReadHandle<TARGET> GetReadHandle() {
		return IndexReadHandle<TARGET>(shared_from_this(), lock.GetSharedLock());
	}
	//! Acquire exclusive access to the physical index and checkpoint state.
	template <class TARGET>
	IndexWriteHandle<TARGET> GetWriteHandle() {
		return IndexWriteHandle<TARGET>(shared_from_this(), lock.GetExclusiveLock());
	}
	IndexBindState GetBindState() const {
		return bind_state.load();
	}
	void SetBindState(IndexBindState state) {
		bind_state = state;
	}
	idx_t GetIndexOid() const {
		return index_oid;
	}

private:
	template <class>
	friend class IndexReadHandle;
	template <class>
	friend class IndexWriteHandle;
	void InitializeLocalIndexesInternal(TableIndexList &delete_indexes,
	                                    optional_ptr<TableIndexList> append_indexes) const;

	//! Replace the physical index, this assumes an exclusive lock on the index is being held.
	void InstallShadowInternal(unique_ptr<BoundIndex> shadow_index);

private:
	atomic<IndexBindState> bind_state;
	//! The OID of this index.
	const idx_t index_oid;
	//! Phase-fair lock protecting the physical index and all delta indexes owned by this entry.
	mutable StorageLock lock;
	//! The physical index owned by this stable logical entry.
	unique_ptr<Index> owned_index;
	//! Whether the enforced constraint is deferred. Derived from the catalog constraint, not serialized.
	const ConstraintCheckMode check_mode;
	IndexDeltas deltas;
};

template <class TARGET>
IndexReadHandle<TARGET>::IndexReadHandle(shared_ptr<IndexEntry> entry_p, unique_ptr<StorageLockKey> lock_p)
    : entry(std::move(entry_p)), lock(std::move(lock_p)) {
	D_ASSERT(IsValid());
	if (!entry->owned_index) {
		throw InternalException("Cannot acquire a handle to a retired index entry");
	}
	GetEntry().owned_index->template Cast<TARGET>();
}

template <class TARGET>
bool IndexReadHandle<TARGET>::IsValid() const {
	D_ASSERT(bool(entry) == bool(lock));
	return entry != nullptr;
}

template <class TARGET>
const IndexEntry &IndexReadHandle<TARGET>::GetEntry() const {
	D_ASSERT(IsValid());
	return *entry;
}

template <class TARGET>
const TARGET *IndexReadHandle<TARGET>::operator->() const & {
	return std::addressof(GetEntry().owned_index->template Cast<TARGET>());
}

template <class TARGET>
optional_ptr<const TARGET> IndexReadHandle<TARGET>::FindDelta(IndexDeltaType type) const & {
	auto index = GetEntry().deltas.Find(type);
	if (!index) {
		return nullptr;
	}
	return index->template Cast<TARGET>();
}

template <class TARGET>
IndexWriteHandle<TARGET>::IndexWriteHandle(shared_ptr<IndexEntry> entry_p, unique_ptr<StorageLockKey> lock_p)
    : entry(std::move(entry_p)), lock(std::move(lock_p)) {
	D_ASSERT(IsValid());
	if (!entry->owned_index) {
		throw InternalException("Cannot acquire a handle to a retired index entry");
	}
	GetMutableEntry().owned_index->template Cast<TARGET>();
}

template <class TARGET>
bool IndexWriteHandle<TARGET>::IsValid() const {
	D_ASSERT(bool(entry) == bool(lock));
	return entry != nullptr;
}

template <class TARGET>
IndexEntry &IndexWriteHandle<TARGET>::GetMutableEntry() {
	D_ASSERT(IsValid());
	return *entry;
}

template <class TARGET>
TARGET *IndexWriteHandle<TARGET>::operator->() & {
	return std::addressof(GetMutableEntry().owned_index->template Cast<TARGET>());
}

} // namespace duckdb
