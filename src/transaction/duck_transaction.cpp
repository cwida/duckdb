#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/transaction_data.hpp"
#include "duckdb/transaction/commit_state.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/update_info.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

TransactionData::TransactionData(DuckTransaction &transaction_p) // NOLINT
    : transaction(&transaction_p), view(transaction_p.GetSnapshotView()) {
}
TransactionData::TransactionData(transaction_t transaction_id_p, VisibilityBound visibility_bound_p)
    : transaction(nullptr), view(transaction_id_p, visibility_bound_p) {
}

DuckTransaction::DuckTransaction(DuckTransactionManager &manager, ClientContext &context_p, transaction_t start_time,
                                 SnapshotView view_p, idx_t catalog_version_p)
    : Transaction(manager, context_p), start_time(start_time), view(view_p), commit_id(0),
      catalog_version(catalog_version_p), awaiting_cleanup(false), undo_buffer(*this, context_p),
      storage(make_uniq<LocalStorage>(context_p, *this)) {
	D_ASSERT(IsCommitted(start_time) && !IsCommitted(view.transaction_id));
}

SnapshotView DuckTransaction::GetSnapshotView() const {
	return view;
}

DuckTransaction::~DuckTransaction() {
}

DuckTransaction &DuckTransaction::Get(ClientContext &context, AttachedDatabase &db) {
	return DuckTransaction::Get(context, db.GetCatalog());
}

DuckTransaction &DuckTransaction::Get(ClientContext &context, Catalog &catalog) {
	auto &transaction = Transaction::Get(context, catalog);
	if (!transaction.IsDuckTransaction()) {
		throw InternalException("DuckTransaction::Get called on non-DuckDB transaction");
	}
	return transaction.Cast<DuckTransaction>();
}

DuckTransactionManager &DuckTransaction::GetTransactionManager() {
	return manager.Cast<DuckTransactionManager>();
}

LocalStorage &DuckTransaction::GetLocalStorage() {
	return *storage;
}

void DuckTransaction::PushCatalogEntry(CatalogEntry &entry, data_ptr_t extra_data, idx_t extra_data_size) {
	idx_t alloc_size = sizeof(CatalogEntry *);
	if (extra_data_size > 0) {
		alloc_size += extra_data_size + sizeof(idx_t);
	}

	auto undo_entry = undo_buffer.CreateEntry(UndoFlags::CATALOG_ENTRY, alloc_size);
	auto ptr = undo_entry.GetDataMutable();
	// store the pointer to the catalog entry
	Store<CatalogEntry *>(&entry, ptr);
	if (extra_data_size > 0) {
		// copy the extra data behind the catalog entry pointer (if any)
		ptr += sizeof(CatalogEntry *);
		// first store the extra data size
		Store<idx_t>(extra_data_size, ptr);
		ptr += sizeof(idx_t);
		// then copy over the actual data
		memcpy(ptr, extra_data, extra_data_size);
	}
}

void DuckTransaction::PushAttach(AttachedDatabase &db) {
	auto undo_entry = undo_buffer.CreateEntry(UndoFlags::ATTACHED_DATABASE, sizeof(AttachedDatabase *));
	auto ptr = undo_entry.GetDataMutable();
	// store the pointer to the database
	Store<CatalogEntry *>(&db, ptr);
}

void DuckTransaction::PushWrite(UndoFlags type, DuckTableEntry &table_entry, RowVersionManager &info, idx_t vector_idx,
                                idx_t base_row, const WrittenRows &written) {
	auto alloc_size = DeleteInfo::GetAllocationSize(written.count, written.is_consecutive, written.previous_id_count);

	auto undo_entry = undo_buffer.CreateEntry(type, alloc_size);
	auto delete_info = reinterpret_cast<DeleteInfo *>(undo_entry.GetDataMutable());
	delete_info->version_info = &info;
	delete_info->vector_idx = vector_idx;
	delete_info->table = &table_entry;
	delete_info->count = written.count;
	delete_info->base_row = base_row;
	delete_info->is_consecutive = written.is_consecutive;
	delete_info->previous_id_count = written.previous_id_count;
	if (!written.is_consecutive) {
		std::copy_n(written.rows, written.count, delete_info->GetRows());
	}
	std::copy_n(written.previous_ids, written.previous_id_count, delete_info->GetPreviousIds());
}

void DuckTransaction::PushAppend(DuckTableEntry &table_entry, idx_t start_row, idx_t row_count) {
	auto undo_entry = undo_buffer.CreateEntry(UndoFlags::INSERT_TUPLE, sizeof(AppendInfo));
	auto append_info = reinterpret_cast<AppendInfo *>(undo_entry.GetDataMutable());
	append_info->table = &table_entry;
	append_info->start_row = start_row;
	append_info->count = row_count;
}

UndoBufferReference DuckTransaction::CreateUpdateInfo(DuckTableEntry &table_entry, idx_t type_size, idx_t capacity,
                                                      idx_t value_capacity, idx_t row_group_start) {
	idx_t alloc_size = UpdateInfo::GetAllocSize(type_size, capacity, value_capacity);
	auto undo_entry = undo_buffer.CreateEntry(UndoFlags::UPDATE_TUPLE, alloc_size);
	auto &update_info = UpdateInfo::Get(undo_entry);
	UpdateInfo::Initialize(update_info, table_entry, GetTransactionId(), row_group_start, capacity, value_capacity);
	return undo_entry;
}

void DuckTransaction::PushSequenceUsage(SequenceCatalogEntry &sequence, const SequenceData &data) {
	lock_guard<mutex> l(sequence_lock);
	auto entry = sequence_usage.find(sequence);
	if (entry == sequence_usage.end()) {
		auto undo_entry = undo_buffer.CreateEntry(UndoFlags::SEQUENCE_VALUE, sizeof(SequenceValue));
		auto sequence_info = reinterpret_cast<SequenceValue *>(undo_entry.GetDataMutable());
		sequence_info->entry = &sequence;
		sequence_info->usage_count = data.usage_count;
		sequence_info->counter = data.counter;
		sequence_usage.emplace(sequence, *sequence_info);
	} else {
		auto &sequence_info = entry->second.get();
		D_ASSERT(RefersToSameObject(*sequence_info.entry, sequence));
		sequence_info.usage_count = data.usage_count;
		sequence_info.counter = data.counter;
	}
}

bool DuckTransaction::ChangesMade() {
	return undo_buffer.ChangesMade() || storage->ChangesMade();
}

UndoBufferProperties DuckTransaction::GetUndoProperties() {
	auto properties = undo_buffer.GetProperties();
	properties.estimated_size += storage->EstimatedSize();
	return properties;
}

bool DuckTransaction::AutomaticCheckpoint(AttachedDatabase &db, const UndoBufferProperties &properties) {
	if (is_checkpoint_transaction) {
		return false;
	}
	if (!ChangesMade()) {
		// read-only transactions cannot trigger an automated checkpoint
		return false;
	}
	if (db.IsReadOnly()) {
		// when attaching a database in read-only mode we cannot checkpoint
		// note that attaching a database in read-only mode does NOT mean we never make changes
		// WAL replay can make changes to the database - but only in the in-memory copy of the
		return false;
	}
	auto &storage_manager = db.GetStorageManager();
	return storage_manager.AutomaticCheckpoint(properties.estimated_size);
}

bool DuckTransaction::ShouldWriteToWAL(AttachedDatabase &db) {
	if (!ChangesMade()) {
		return false;
	}
	if (db.IsSystem()) {
		return false;
	}
	if (db.GetRecoveryMode() == RecoveryMode::NO_WAL_WRITES) {
		// WAL writes are explicitly disabled
		return false;
	}
	auto &storage_manager = db.GetStorageManager();
	if (!storage_manager.HasWAL()) {
		return false;
	}
	return true;
}

ErrorData DuckTransaction::PreFlushOptimisticBlocks(AttachedDatabase &db) noexcept {
	ErrorData error;
	if (!ShouldWriteToWAL(db)) {
		return error;
	}
	try {
		storage->FlushBulkAppendBlocksAndSync(db);
	} catch (std::exception &ex) {
		// fail the commit: the flush machinery cannot safely be re-run after an error, and a failed
		// fsync must not be retried (the retry can succeed without the data being durable)
		error = ErrorData(ex);
	}
	return error;
}

static void RevertCommitState(unique_ptr<StorageCommitState> &commit_state) noexcept {
	try {
		commit_state->RevertCommit();
		commit_state.reset();
	} catch (std::exception &) {
		// Ignore this error. If we fail to RevertCommit(), just return the original exception
	}
}

ErrorData DuckTransaction::AppendLocalStorage(ClientContext &context, AttachedDatabase &db,
                                              unique_ptr<StorageCommitState> &commit_state) noexcept {
	ErrorData error_data;
	try {
		if (ShouldWriteToWAL(db)) {
			auto &storage_manager = db.GetStorageManager();
			commit_state = storage_manager.GenStorageCommitState(*storage_manager.GetWAL());
		}
		auto &profiler = *context.client_data->profiler;
		auto commit_timer = profiler.StartTimer<MetricStorageCommitLocalStorageLatency>();
		storage->Commit(commit_state.get());
		commit_timer.EndTimer();
	} catch (std::exception &ex) {
		// Call RevertCommit() outside this try-catch as it itself may throw
		error_data = ErrorData(ex);
	}
	if (commit_state && error_data.HasError()) {
		RevertCommitState(commit_state);
	}
	return error_data;
}

ErrorData DuckTransaction::WriteToWAL(ClientContext &context, AttachedDatabase &db,
                                      unique_ptr<StorageCommitState> &commit_state) noexcept {
	ErrorData error_data;
	try {
		// the append may have consumed the last local change: do not ask ShouldWriteToWAL again here
		D_ASSERT(commit_state);
		auto wal = db.GetStorageManager().GetWAL();
		auto &profiler = *context.client_data->profiler;
		auto wal_timer = profiler.StartTimer<MetricStorageWriteToWALLatency>();
		undo_buffer.WriteToWAL(*wal, commit_state.get());
		wal_timer.EndTimer();

		// no FileSync is required here: any optimistically written blocks that the WAL references
		// have already been synced by FlushBulkAppendBlocksAndSync, before the commit locks were taken
		D_ASSERT(!commit_state->HasRowGroupData() || storage->SyncedFlushedBlocks());
	} catch (std::exception &ex) {
		// Call RevertCommit() outside this try-catch as it itself may throw
		error_data = ErrorData(ex);
	}
	if (commit_state && error_data.HasError()) {
		RevertCommitState(commit_state);
	}
	return error_data;
}

ErrorData DuckTransaction::Commit(AttachedDatabase &db, CommitInfo &commit_info,
                                  unique_ptr<StorageCommitState> commit_state) noexcept {
	this->commit_id = commit_info.commit_id;
	if (!ChangesMade()) {
		// no need to flush anything if we made no changes
		return ErrorData();
	}
	D_ASSERT(db.IsSystem() || db.IsTemporary() || !IsReadOnly());

	UndoBuffer::IteratorState iterator_state;
	optional_ptr<BlockManager> block_manager;
	if (db.HasStorageManager()) {
		block_manager = db.GetStorageManager().GetBlockManager();
	}
	CommitDropState drop_state(block_manager);
	commit_info.drop_state = &drop_state;

	ErrorData error_data;
	try {
		storage->Commit(commit_state.get());
		undo_buffer.Commit(iterator_state, commit_info);
		if (!db.IsSystem() && !db.IsTemporary() && Settings::Get<DebugForceCommitFailureSetting>(db.GetDatabase())) {
			throw InvalidInputException("Forced commit failure (debug_force_commit_failure)");
		}
		if (commit_state) {
			// if we have written to the WAL - flush after the commit has been successful. With no other
			// transaction there is nothing to share the sync with, so sync now under the transaction lock;
			// otherwise only write the marker and sync once the locks are released
			bool sync_now = commit_info.active_transactions == ActiveTransactionState::NO_OTHER_TRANSACTIONS;
			commit_info.wal_sync_offset = commit_state->FlushCommit(sync_now);
		}
		drop_state.FinalizeCommit();
		return ErrorData();
	} catch (std::exception &ex) {
		// Record the error and run RevertCommit() outside this try-catch: RevertCommit() iterates the
		// undo buffer and may itself throw (e.g. Pin() failing under memory pressure), which would
		// escape this noexcept function and trigger std::terminate.
		error_data = ErrorData(ex);
	}

	try {
		undo_buffer.RevertCommit(iterator_state, GetTransactionId());
		if (!db.IsSystem() && !db.IsTemporary() &&
		    Settings::Get<DebugForceCommitRevertFailureSetting>(db.GetDatabase())) {
			throw IOException("Forced RevertCommit failure (debug_force_commit_revert_failure)");
		}
		if (commit_state) {
			// if we have written to the WAL - truncate the WAL on failure
			commit_state->RevertCommit();
		}
	} catch (std::exception &ex) {
		// If we fail to revert the commit, the database is left in an undefined state - invalidate it.
		// Record both the original commit error and the revert error so the root cause stays visible.
		ValidChecker::Invalidate(db.GetDatabase(),
		                         "Failed to revert transaction commit, database is in an undefined state. "
		                         "Original commit error: " +
		                             error_data.RawMessage() + ". RevertCommit error: " + ErrorData(ex).RawMessage());
	} catch (...) {
		// last line of defense: this is a noexcept function, nothing may escape
		ValidChecker::Invalidate(db.GetDatabase(),
		                         "Failed to revert transaction commit (unknown error), database is in an "
		                         "undefined state. Original commit error: " +
		                             error_data.RawMessage());
	}
	return error_data;
}

ErrorData DuckTransaction::Rollback() {
	try {
		storage->Rollback();
		undo_buffer.Rollback();
		return ErrorData();
	} catch (std::exception &ex) {
		return ErrorData(ex);
	}
}

void DuckTransaction::Cleanup(VisibilityBound lowest_visibility_bound) {
	undo_buffer.Cleanup(lowest_visibility_bound);
}

void DuckTransaction::SetModifications(DatabaseModificationType type) {
	if (!checkpoint_lock) {
		bool require_write_lock = false;
		require_write_lock = require_write_lock || type.UpdateData();
		require_write_lock = require_write_lock || type.AlterTable();
		require_write_lock = require_write_lock || type.CreateCatalogEntry();
		require_write_lock = require_write_lock || type.DropCatalogEntry();
		require_write_lock = require_write_lock || type.CreateIndex();
		// not SEQUENCE: a checkpoint writes a sequence's current state, and WAL replay keeps the most used one

		if (require_write_lock) {
			// obtain a shared checkpoint lock to prevent concurrent checkpoints while this transaction is running
			checkpoint_lock = GetTransactionManager().SharedCheckpointLock();
		}
	}
	if (!vacuum_lock) {
		bool require_vacuum_lock = false;
		require_vacuum_lock = require_vacuum_lock || type.InsertData();
		require_vacuum_lock = require_vacuum_lock || type.DeleteData();

		if (require_vacuum_lock) {
			vacuum_lock = GetTransactionManager().SharedVacuumLock();
		}
	}
}

unique_ptr<StorageLockKey> DuckTransaction::TryGetCheckpointLock() {
	if (!checkpoint_lock) {
		return GetTransactionManager().TryGetCheckpointLock();
	} else {
		return GetTransactionManager().TryUpgradeCheckpointLock(*checkpoint_lock);
	}
}

} // namespace duckdb
