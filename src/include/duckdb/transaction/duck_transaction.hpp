//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/duck_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/transaction/undo_buffer.hpp"
#include "duckdb/common/enums/active_transaction_state.hpp"

namespace duckdb {
class CommitDropState;
class DuckTableEntry;
class RowGroupCollection;
class RowVersionManager;
class DuckTransactionManager;
class StorageLockKey;
class StorageCommitState;
struct UndoBufferProperties;
struct WrittenRows;

struct CommitInfo {
	transaction_t commit_id;
	ActiveTransactionState active_transactions = ActiveTransactionState::UNSET;
	optional_ptr<CommitDropState> drop_state;
	//! WAL offset covering the commit's flush marker (0 if no WAL was written)
	idx_t wal_sync_offset = 0;
};

class DuckTransaction : public Transaction {
public:
	DuckTransaction(DuckTransactionManager &manager, ClientContext &context, transaction_t start_time,
	                SnapshotView view, idx_t catalog_version);
	~DuckTransaction() override;

	//! The start timestamp of this transaction
	transaction_t start_time;
	//! What this transaction sees: its own writes, and everything before its visibility bound
	SnapshotView view;
	//! The commit id of this transaction, if it has successfully been committed
	transaction_t commit_id;
	//! WAL offset covering the commit's flush marker, set when the commit is published; the commit
	//! is durable once the WAL is synced up to it (0 while uncommitted or when nothing was written)
	idx_t wal_sync_offset = 0;
	//! The committed catalog version just before this commit published
	idx_t catalog_version_before_commit = 0;

	atomic<idx_t> catalog_version;

	//! Transactions undergo Cleanup, after (1) removing them directly in RemoveTransaction,
	//! or (2) after they enter cleanup_queue.
	//! Some (after rollback) enter cleanup_queue, but do not require Cleanup.
	bool awaiting_cleanup;

public:
	static DuckTransaction &Get(ClientContext &context, AttachedDatabase &db);
	static DuckTransaction &Get(ClientContext &context, Catalog &catalog);
	LocalStorage &GetLocalStorage();

	void PushCatalogEntry(CatalogEntry &entry, data_ptr_t extra_data, idx_t extra_data_size);
	void PushAttach(AttachedDatabase &db);

	void SetModifications(DatabaseModificationType type) override;

	bool ShouldWriteToWAL(AttachedDatabase &db);
	ErrorData PreFlushOptimisticBlocks(AttachedDatabase &db) noexcept;
	//! Appends the local storage to the tables; with a WAL, the commit state records the optimistically written blocks
	ErrorData AppendLocalStorage(ClientContext &context, AttachedDatabase &db,
	                             unique_ptr<StorageCommitState> &commit_state) noexcept;
	//! Writes the undo buffer to the WAL, with the commit state of AppendLocalStorage
	ErrorData WriteToWAL(ClientContext &context, AttachedDatabase &db,
	                     unique_ptr<StorageCommitState> &commit_state) noexcept;
	//! Commit the current transaction with the given commit identifier. Returns an error message if the transaction
	//! commit failed, or an empty string if the commit was successful
	ErrorData Commit(AttachedDatabase &db, CommitInfo &commit_info,
	                 unique_ptr<StorageCommitState> commit_state) noexcept;
	//! Returns whether or not a commit of this transaction should trigger an automatic checkpoint
	bool AutomaticCheckpoint(AttachedDatabase &db, const UndoBufferProperties &properties);

	//! Rollback
	ErrorData Rollback();
	//! Cleanup the undo buffer
	void Cleanup(VisibilityBound lowest_visibility_bound);

	bool ChangesMade();
	UndoBufferProperties GetUndoProperties();

	//! Pushes the undo entry of a delete (DELETE_TUPLE) or of an update's markers (UPDATE_ROWS) in one vector
	void PushWrite(UndoFlags type, DuckTableEntry &table_entry, RowVersionManager &info, idx_t vector_idx,
	               idx_t base_row, const WrittenRows &written);
	void PushSequenceUsage(SequenceCatalogEntry &entry, const SequenceData &data);
	void PushAppend(DuckTableEntry &table_entry, idx_t row_start, idx_t row_count);
	UndoBufferReference CreateUpdateInfo(DuckTableEntry &table_entry, idx_t type_size, idx_t capacity,
	                                     idx_t value_capacity, idx_t row_group_start);

	DuckTransactionManager &GetTransactionManager();
	bool IsDuckTransaction() const override {
		return true;
	}
	SnapshotView GetSnapshotView() const override;
	transaction_t GetTransactionId() const {
		return view.transaction_id;
	}

	unique_ptr<StorageLockKey> TryGetCheckpointLock();

	void SetIsCheckpointTransaction() {
		is_checkpoint_transaction = true;
	}

private:
	//! The undo buffer is used to store old versions of rows that are updated
	//! or deleted
	UndoBuffer undo_buffer;
	//! The set of uncommitted appends for the transaction
	unique_ptr<LocalStorage> storage;
	//! Lock that prevents checkpoints from starting
	unique_ptr<StorageLockKey> checkpoint_lock;
	//! Lock that prevents vacuums from starting
	unique_ptr<StorageLockKey> vacuum_lock;
	//! The storage this transaction dropped, which the transaction manager owns until the cleanup frees it
	optional_ptr<CommitDropState> dropped_storage;
	//! Lock for accessing sequence_usage
	mutex sequence_lock;
	//! Map of all sequences that were used during the transaction and the value they had in this transaction
	reference_map_t<SequenceCatalogEntry, reference<SequenceValue>> sequence_usage;
	//! Flag to prevent auto-checkpointing inside a checkpoint transaction.
	bool is_checkpoint_transaction = false;
};

} // namespace duckdb
