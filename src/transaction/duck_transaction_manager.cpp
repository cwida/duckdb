#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/logging/log_manager.hpp"

#include "duckdb/main/client_data.hpp"

#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/commit_state.hpp"
#include "duckdb/transaction/transaction_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/checkpoint/checkpoint_options.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static ErrorData BuildAutocheckpointError(AttachedDatabase &db, const std::exception &ex) {
	ErrorData original(ex);
	string recovery = db.IsInitialDatabase() ? "Reopen the database instance to recover."
	                                         : "Detach and reattach the database to recover.";
	string msg = StringUtil::Format("Transaction COMMIT succeeded and is durable, but the autocheckpoint failed. %s %s",
	                                recovery, original.RawMessage());
	return ErrorData(original.Type(), msg);
}

void DuckCleanupInfo::Cleanup() {
	for (auto &transaction : transactions) {
		if (transaction->awaiting_cleanup) {
			transaction->Cleanup(lowest_visibility_bound);
			transaction->awaiting_cleanup = false;
		}
	}
}

bool DuckCleanupInfo::ScheduleCleanup() noexcept {
	return !transactions.empty();
}

DuckTransactionManager::DuckTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	// the first timestamp a real transaction can draw: one past the bootstrap stamp, which is also
	// the bound the system transaction reads at
	current_start_timestamp = SYSTEM_TRANSACTION_TIMESTAMP + 1;
	last_commit = SYSTEM_TRANSACTION_TIMESTAMP;
	// transaction ID starts very high:
	// it should be much higher than the current start timestamp
	// if transaction_id < start_timestamp for any set of active transactions
	// uncommitted data could be read by
	current_transaction_id = TRANSACTION_ID_START;
	lowest_visibility_bound = VisibilityBound::IncludingUncommitted();
	active_checkpoint = 0;
	if (!db.GetCatalog().IsDuckCatalog()) {
		// Specifically the StorageManager of the DuckCatalog is relied on, with `db.GetStorageManager`
		throw InternalException("DuckTransactionManager should only be created together with a DuckCatalog");
	}
}

DuckTransactionManager::~DuckTransactionManager() {
	D_ASSERT(!HasUnsyncedCommits());
}

DuckTransactionManager &DuckTransactionManager::Get(AttachedDatabase &db) {
	auto &transaction_manager = TransactionManager::Get(db);
	if (!transaction_manager.IsDuckTransactionManager()) {
		throw InternalException("Calling DuckTransactionManager::Get on non-DuckDB transaction manager");
	}
	return reinterpret_cast<DuckTransactionManager &>(transaction_manager);
}

Transaction &DuckTransactionManager::StartTransaction(ClientContext &context) {
	// obtain the transaction lock during this function
	auto &meta_transaction = MetaTransaction::Get(context);
	unique_lock<mutex> start_lock(start_transaction_lock, std::defer_lock);
	if (!meta_transaction.IsReadOnly()) {
		start_lock.lock();
	}
	lock_guard<mutex> lock(transaction_lock);
	if (current_start_timestamp >= TRANSACTION_ID_START) { // LCOV_EXCL_START
		throw InternalException("Cannot start more transactions, ran out of "
		                        "transaction identifiers!");
	} // LCOV_EXCL_STOP

	// obtain the start time and transaction ID of this transaction
	transaction_t start_time = current_start_timestamp++;
	transaction_t transaction_id = current_transaction_id++;
	// snapshots must not observe commits that are not yet durable, nor a newer catalog version
	auto durable = GetDurableSnapshot();
	// the transaction sees its own writes, and every durable commit before its start time
	SnapshotView view(transaction_id,
	                  VisibilityBound::Min(VisibilityBound::Before(start_time), durable.visibility_bound));
	auto catalog_version = MinValue<idx_t>(last_committed_version, durable.catalog_version);
	if (active_transactions.empty()) {
		lowest_visibility_bound = view.visibility_bound;
	}

	// create the actual transaction
	auto transaction = make_uniq<DuckTransaction>(*this, context, start_time, view, catalog_version);
	auto &transaction_ref = *transaction;

	// store it in the set of active transactions
	active_transactions.push_back(std::move(transaction));
	return transaction_ref;
}

void DuckTransactionManager::SetActiveCheckpoint(idx_t checkpoint_id) {
	// called under the commit lock: a commit's flush and its commit or revert are entirely before or after this
	active_checkpoint = checkpoint_id;
}

void DuckTransactionManager::ResetActiveCheckpoint() {
	active_checkpoint = 0;
}

CommitDropState &DuckTransactionManager::AddDroppedStorage(unique_ptr<CommitDropState> drop_state) {
	D_ASSERT(drop_state->commit_id != 0);
	drop_state->DetachIndexes();
	auto &result = *drop_state;
	lock_guard<mutex> guard(dropped_storage_lock);
	dropped_storage.push_back(std::move(drop_state));
	return result;
}

void DuckTransactionManager::ExcludeDroppedStorage(VisibilityBound visibility_bound) {
	lock_guard<mutex> exclude_or_free_guard(exclude_or_free_lock);
	vector<reference<CommitDropState>> drop_states;
	{
		lock_guard<mutex> guard(dropped_storage_lock);
		for (auto &drop_state : dropped_storage) {
			if (drop_state->commit_id < visibility_bound) {
				drop_states.push_back(*drop_state);
			}
		}
	}
	for (auto &drop_state : drop_states) {
		drop_state.get().ExcludeFromCheckpoints();
	}
}

void DuckTransactionManager::FreeDroppedStorage(CommitDropState &drop_state) {
	lock_guard<mutex> exclude_or_free_guard(exclude_or_free_lock);
	unique_ptr<CommitDropState> owned_state;
	{
		lock_guard<mutex> guard(dropped_storage_lock);
		for (idx_t i = 0; i < dropped_storage.size(); i++) {
			if (dropped_storage[i].get() == &drop_state) {
				owned_state = std::move(dropped_storage[i]);
				dropped_storage.erase_at(i);
				break;
			}
		}
	}
	if (!owned_state) {
		throw InternalException("FreeDroppedStorage called with storage that is not registered");
	}
	// removed first: if freeing fails, the state is destroyed while unwinding
	owned_state->Free();
}

DuckTransactionManager::CheckpointDecision::CheckpointDecision(string reason_p)
    : can_checkpoint(false), reason(std::move(reason_p)) {
}

DuckTransactionManager::CheckpointDecision::CheckpointDecision(CheckpointType type) : can_checkpoint(true), type(type) {
}

DuckTransactionManager::CheckpointDecision::~CheckpointDecision() {
}

bool DuckTransactionManager::HasOtherTransactions(DuckTransaction &transaction) {
	for (auto &active_transaction : active_transactions) {
		if (!RefersToSameObject(*active_transaction, transaction)) {
			return true;
		}
	}
	return false;
}

DuckTransactionManager::CheckpointDecision
DuckTransactionManager::CanCheckpoint(DuckTransaction &transaction, unique_ptr<StorageLockKey> &lock,
                                      const UndoBufferProperties &undo_properties) {
	if (db.IsSystem()) {
		return CheckpointDecision("system transaction");
	}
	if (transaction.IsReadOnly()) {
		return CheckpointDecision("transaction is read-only");
	}
	auto &storage_manager = db.GetStorageManager();
	if (!storage_manager.IsLoaded()) {
		return CheckpointDecision("cannot checkpoint while loading");
	}
	if (!transaction.AutomaticCheckpoint(db, undo_properties)) {
		return CheckpointDecision("no reason to automatically checkpoint");
	}
	if (Settings::Get<DebugSkipCheckpointOnCommitSetting>(db.GetDatabase())) {
		return CheckpointDecision("checkpointing on commit disabled through configuration");
	}
	// try to lock the checkpoint lock
	lock = transaction.TryGetCheckpointLock();
	if (!lock) {
		return CheckpointDecision("Failed to obtain checkpoint lock - another thread is writing/checkpointing or "
		                          "another read transaction relies on data that is not yet committed");
	}
	return CheckpointDecision(CheckpointType::FULL_CHECKPOINT);
}

DuckTransactionManager::CheckpointDecision
DuckTransactionManager::GetCheckpointType(DuckTransaction &transaction, const UndoBufferProperties &undo_properties) {
	auto &storage_manager = db.GetStorageManager();
	auto checkpoint_type = CheckpointType::FULL_CHECKPOINT;
	bool has_other_transactions = HasOtherTransactions(transaction);
	if (has_other_transactions) {
		if (undo_properties.has_updates) {
			// if we have made updates in this transaction we cannot checkpoint in the presence of other transactions
			string other_transactions;
			for (auto &active_transaction : active_transactions) {
				if (!RefersToSameObject(*active_transaction, transaction)) {
					if (!other_transactions.empty()) {
						other_transactions += ", ";
					}
					other_transactions += "[" + to_string(active_transaction->GetTransactionId()) + "]";
				}
			}
			if (!other_transactions.empty()) {
				// there are other transactions!
				// these active transactions might need data from BEFORE this transaction
				// this transaction has performed updates - we cannot checkpoint
				return CheckpointDecision(
				    "Transaction has performed updates and there are other transactions active\nActive transactions: " +
				    other_transactions);
			}
		}
		// otherwise - we need to do a concurrent checkpoint
		checkpoint_type = CheckpointType::CONCURRENT_CHECKPOINT;
	}
	if (storage_manager.InMemory() && !storage_manager.CompressionIsEnabled()) {
		if (checkpoint_type == CheckpointType::CONCURRENT_CHECKPOINT) {
			return CheckpointDecision("Cannot vacuum, and compression is disabled for in-memory table");
		}
		return CheckpointDecision(CheckpointType::VACUUM_ONLY);
	}
	return CheckpointDecision(checkpoint_type);
}

void DuckTransactionManager::Checkpoint(ClientContext &context, bool force) {
	if (ValidChecker::IsInvalidated(db)) {
		throw IOException("%s", ValidChecker::InvalidatedMessage(db));
	}
	auto &storage_manager = db.GetStorageManager();
	auto current = Transaction::TryGet(context, db);
	if (current) {
		if (force) {
			throw TransactionException(
			    "Cannot FORCE CHECKPOINT: the current transaction has been started for this database");
		} else {
			auto &duck_transaction = current->Cast<DuckTransaction>();
			if (duck_transaction.ChangesMade()) {
				throw TransactionException("Cannot CHECKPOINT: the current transaction has transaction local changes");
			}
		}
	}

	unique_ptr<StorageLockKey> lock;
	if (!force) {
		// not a force checkpoint
		// try to get the checkpoint lock
		lock = checkpoint_lock.TryGetExclusiveLock();
		if (!lock) {
			// we could not manage to get the lock - cancel
			throw TransactionException("Cannot CHECKPOINT: there are other write transactions active. Try using FORCE "
			                           "CHECKPOINT to wait until all active transactions are finished");
		}

	} else {
		// force checkpoint - wait to get an exclusive lock
		// grab the start_transaction_lock to prevent new transactions from starting
		lock_guard<mutex> start_lock(start_transaction_lock);
		// wait until any active transactions are finished
		while (!lock) {
			context.InterruptCheck();
			lock = checkpoint_lock.TryGetExclusiveLock();
		}
	}
	CheckpointOptions options;
	if (GetLastCommit() >= LowestVisibilityBound()) {
		// we cannot do a full checkpoint if any transaction needs to read old data
		options.type = CheckpointType::CONCURRENT_CHECKPOINT;
	}

	storage_manager.CreateCheckpoint(context, options);
}

unique_ptr<StorageLockKey> DuckTransactionManager::SharedCheckpointLock() {
	return checkpoint_lock.GetSharedLock();
}

unique_ptr<StorageLockKey> DuckTransactionManager::TryUpgradeCheckpointLock(StorageLockKey &lock) {
	return checkpoint_lock.TryUpgradeCheckpointLock(lock);
}

unique_ptr<StorageLockKey> DuckTransactionManager::TryGetCheckpointLock() {
	return checkpoint_lock.TryGetExclusiveLock();
}

unique_ptr<StorageLockKey> DuckTransactionManager::SharedVacuumLock() {
	return vacuum_lock.GetSharedLock();
}

unique_ptr<StorageLockKey> DuckTransactionManager::TryGetVacuumLock() {
	return vacuum_lock.TryGetExclusiveLock();
}

transaction_t DuckTransactionManager::GetCommitTimestamp() {
	return current_start_timestamp++;
}

bool DuckTransactionManager::HasUnsyncedCommits() {
	for (auto &active_transaction : active_transactions) {
		if (active_transaction->wal_sync_offset != 0) {
			return true;
		}
	}
	return false;
}

DuckTransactionManager::DurableSnapshot DuckTransactionManager::GetDurableSnapshot() {
	DurableSnapshot durable;
	optional_ptr<DuckTransaction> first_unsynced;
	for (auto &active_transaction : active_transactions) {
		if (active_transaction->wal_sync_offset == 0 || active_transaction->commit_id < durable_bound) {
			// not committed, or durable already - its own thread has not removed it yet
			continue;
		}
		if (!first_unsynced || active_transaction->commit_id < first_unsynced->commit_id) {
			first_unsynced = active_transaction.get();
		}
	}
	if (first_unsynced) {
		// a snapshot stops below the first commit that is not durable and sees the catalog version
		// recorded just before that commit
		durable.visibility_bound = VisibilityBound::Before(first_unsynced->commit_id);
		durable.catalog_version = first_unsynced->catalog_version_before_commit;
	}
	return durable;
}

void DuckTransactionManager::WaitForDurability() {
	unique_lock<mutex> guard(transaction_lock);
	durability_cv.wait(guard, [&]() { return !HasUnsyncedCommits(); });
}

void DuckTransactionManager::CleanupTransactions() {
	lock_guard<mutex> c_lock(cleanup_lock);
	while (true) {
		DuckCleanupInfo *top_cleanup_info = nullptr;
		{
			lock_guard<mutex> q_lock(cleanup_queue_lock);
			if (cleanup_queue.empty()) {
				// all transactions have been cleaned up - done
				return;
			}
			top_cleanup_info = cleanup_queue.front().get();
		}
		try {
			top_cleanup_info->Cleanup();
		} catch (FatalException &) {
			throw;
		} catch (std::exception &) {
			return;
		}
		{
			lock_guard<mutex> q_lock(cleanup_queue_lock);
			cleanup_queue.pop();
		}
	}
}

ErrorData DuckTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	// flush the transaction-local blocks of bulk appends before taking any commit locks (see PreFlushOptimisticBlocks)
	ErrorData error = transaction.PreFlushOptimisticBlocks(db);
	unique_lock<mutex> t_lock(transaction_lock);
	if (!db.IsSystem() && !db.IsTemporary()) {
		if (transaction.ChangesMade()) {
			if (transaction.IsReadOnly()) {
				throw InternalException("Attempting to commit a transaction that is read-only but has made changes - "
				                        "this should not be possible");
			}
		}
	}

	// check if we can checkpoint
	unique_ptr<StorageLockKey> lock;
	auto undo_properties = transaction.GetUndoProperties();
	auto checkpoint_decision = CanCheckpoint(transaction, lock, undo_properties);
	// orders this commit's append and commit or revert against checkpoints; read-only transactions commit without it
	unique_lock<mutex> held_commit_lock;
	unique_ptr<StorageCommitState> commit_state;
	optional_ptr<WriteAheadLog> commit_wal;
	bool skip_wal_write_due_to_checkpoint = false;
	bool wal_written = false;
	if (checkpoint_decision.can_checkpoint) {
		// we can perform an automatic checkpoint
		// we have two options:
		// either we write to the WAL, in which case we can perform concurrent commits while running
		// OR we skip writing to the WAL, in which case we cannot perform concurrent commits
		// the reason for this is that if we don't write this transactions' changes to the WAL
		// any failure during checkpoint will cause this transactions' changes to be lost,
		// while later concurrent commits will not be
		// this can cause undefined state, as those commits were made assuming this one was already committed
		if (undo_properties.estimated_size >= Settings::Get<AutoCheckpointSkipWalThresholdSetting>(context)) {
			skip_wal_write_due_to_checkpoint = true;
		}
	}
	bool has_changes = !error.HasError() && transaction.ChangesMade() && db.HasStorageManager();
	bool should_write_to_wal = has_changes && transaction.ShouldWriteToWAL(db);
	if (has_changes) {
		// appending the local storage and writing the WAL can take long: other transactions run meanwhile
		// the appended rows stay invisible until the commit below
		// note: if we are checkpointing, we have already made certain decisions (e.g. the CheckpointType)
		t_lock.unlock();
		// grab the commit lock and hold it until the entire commit is finished
		held_commit_lock = db.GetStorageManager().GetCommitLock();

		if (!skip_wal_write_due_to_checkpoint) {
			error = transaction.AppendLocalStorage(context, db, commit_state);
			if (!error.HasError() && should_write_to_wal) {
				// Commit the changes to the WAL.
				error = transaction.WriteToWAL(context, db, commit_state);
				wal_written = true;
			}
		}

		// after we finish writing we grab the transaction lock again
		t_lock.lock();
	}
	if (!error.HasError() && checkpoint_decision.can_checkpoint) {
		// now that we have the transaction lock again, new transactions can't start
		// figure out the checkpoint type now
		checkpoint_decision = GetCheckpointType(transaction, undo_properties);
		if (should_write_to_wal && skip_wal_write_due_to_checkpoint && !checkpoint_decision.can_checkpoint) {
			// we have not written to the WAL but we have now realized we can't checkpoint after all
			// in order to commit we need backpeddle and write to the WAL after all
			D_ASSERT(held_commit_lock.owns_lock());
			// unlock the transaction lock while we are writing to the WAL
			t_lock.unlock();
			error = transaction.AppendLocalStorage(context, db, commit_state);
			if (!error.HasError()) {
				error = transaction.WriteToWAL(context, db, commit_state);
				wal_written = true;
			}
			t_lock.lock();
			skip_wal_write_due_to_checkpoint = false;
		}
	}
	// in-memory databases don't have a WAL - we estimate how large their changeset is based on the undo properties
	if (!db.IsSystem()) {
		auto &storage_manager = db.GetStorageManager();
		if (storage_manager.InMemory() || db.GetRecoveryMode() == RecoveryMode::NO_WAL_WRITES) {
			storage_manager.AddWALSize(undo_properties.estimated_size);
		}
	}
	// obtain a commit id for the transaction
	CommitInfo info;
	info.commit_id = GetCommitTimestamp();

	// commit the UndoBuffer of the transaction
	if (!error.HasError()) {
		if (HasOtherTransactions(transaction)) {
			info.active_transactions = ActiveTransactionState::OTHER_TRANSACTIONS;
		} else {
			info.active_transactions = ActiveTransactionState::NO_OTHER_TRANSACTIONS;
		}
		error = transaction.Commit(db, info, std::move(commit_state));
	}

	if (error.HasError()) {
		DUCKDB_LOG(context, TransactionLogType, db, "Rollback (after failed commit)", info.commit_id);

		// COMMIT not successful: ROLLBACK.
		checkpoint_decision = CheckpointDecision(error.Message());
		transaction.commit_id = 0;

		auto rollback_error = transaction.Rollback();
		if (rollback_error.HasError()) {
			throw FatalException(
			    "Failed to rollback transaction. Cannot continue operation.\nOriginal Error: %s\nRollback Error: %s",
			    error.Message(), rollback_error.Message());
		}
	} else {
		DUCKDB_LOG(context, TransactionLogType, db, "Commit", info.commit_id);
		last_commit = info.commit_id;
		if (wal_written && info.wal_sync_offset > 0) {
			// published but not yet durable: the transaction stays active until the sync below. An offset
			// of 0 means nothing reached the WAL, or the commit synced under the lock already
			commit_wal = db.GetStorageManager().GetWAL();
			if (commit_wal) {
				// the catalog version is recorded before this commit's own bump below
				D_ASSERT(info.commit_id >= durable_bound);
				transaction.wal_sync_offset = info.wal_sync_offset;
				transaction.catalog_version_before_commit = last_committed_version;
			}
		}

		// check if catalog changes were made
		if (transaction.catalog_version >= TRANSACTION_ID_START) {
			transaction.catalog_version = ++last_committed_version;
		}
	}
	OnCommitCheckpointDecision(checkpoint_decision, transaction);

	if (!checkpoint_decision.can_checkpoint && lock) {
		// we won't checkpoint after all due to an error during commit: unlock the checkpoint lock again
		skip_wal_write_due_to_checkpoint = false;
		lock.reset();
	}

	// commit successful: remove the transaction id from the list of active transactions
	// potentially resulting in garbage collection
	bool store_transaction = undo_properties.has_updates || undo_properties.has_index_deletes ||
	                         undo_properties.has_catalog_changes || error.HasError();

	if (!commit_wal) {
		// Remove the transaction from the list of active transactions and gather cleanup information.
		// A commit that needs a WAL sync stays active until the sync below has completed.
		QueueCleanup(RemoveTransaction(transaction, store_transaction, CreateCleanupInfo()));
	}

	// We do not need to hold the transaction lock during cleanup of transactions,
	// as they (1) have been removed, or (2) enter cleanup_info.
	t_lock.unlock();
	// if we have skipped the WAL write due to checkpoint, we keep the commit lock while checkpointing
	// this prevents any concurrent transactions from happening during this time
	if (!skip_wal_write_due_to_checkpoint && held_commit_lock.owns_lock()) {
		held_commit_lock.unlock();
	}

	if (commit_wal) {
		// make the commit durable before acknowledging it; one fsync can cover many commits
		D_ASSERT(!error.HasError());
		bool synced = false;
		try {
			commit_wal->SyncUpTo(info.wal_sync_offset);
			synced = true;
		} catch (std::exception &ex) {
			// published and no longer revertable, but not durable: invalidate. The WAL keeps the
			// bytes; whether a restart replays them is in doubt
			error = ErrorData(ex);
			ValidChecker::Invalidate(db, "Failed to sync the WAL after committing: " + error.Message());
			// no checkpoint after a failed commit, as on the rollback path above
			checkpoint_decision = CheckpointDecision(error.Message());
			lock.reset();
		}
		// durable, or durability has failed: now leave the list of active transactions
		t_lock.lock();
		if (synced) {
			// advance the durable bound over every commit the sync covered
			for (auto &active_transaction : active_transactions) {
				if (active_transaction->wal_sync_offset != 0 &&
				    active_transaction->wal_sync_offset <= info.wal_sync_offset &&
				    active_transaction->commit_id >= durable_bound) {
					durable_bound = VisibilityBound::Through(active_transaction->commit_id);
				}
			}
		}
		QueueCleanup(RemoveTransaction(transaction, store_transaction, CreateCleanupInfo()));
		bool notify_others = !HasUnsyncedCommits();
		t_lock.unlock();
		if (notify_others) {
			durability_cv.notify_all();
		}
	}

	CleanupTransactions();

	if (checkpoint_decision.can_checkpoint && undo_properties.has_updates &&
	    GetLastCommit() >= LowestVisibilityBound()) {
		// GetCheckpointType does not checkpoint while another transaction might still need the state
		// from before this commit. That check ran before the sync; transactions that started during
		// the sync are bounded below this commit and need that state too, so check again here
		D_ASSERT(!skip_wal_write_due_to_checkpoint);
		checkpoint_decision = CheckpointDecision("snapshots bounded below this commit need its pre-commit state");
		lock.reset();
	}

	// A failed commit rolls back inline, queueing teardowns as RollbackTransaction does. Drain here too,
	// so the call that fills the queue is the one that empties it.
	DatabaseManager::Get(db.GetDatabase()).DrainPendingTeardowns();

	// now perform a checkpoint if (1) we are able to checkpoint, and (2) the WAL has reached sufficient size to
	// checkpoint
	if (checkpoint_decision.can_checkpoint) {
		if (!lock || lock->GetType() != StorageLockType::EXCLUSIVE) {
			throw InternalException("Checkpointing requires an exclusive lock to be held");
		}
		// we can unlock the transaction lock while checkpointing
		// checkpoint the database to disk
		CheckpointOptions options;
		options.action = CheckpointAction::ALWAYS_CHECKPOINT;
		options.type = checkpoint_decision.type;
		options.commit_lock = held_commit_lock.owns_lock() ? &held_commit_lock : nullptr;
		auto &storage_manager = db.GetStorageManager();
		try {
			storage_manager.CreateCheckpoint(context, options);
		} catch (std::exception &ex) {
			if (wal_written) {
				context.transaction.SetAutocheckpointError(BuildAutocheckpointError(db, ex));
			} else {
				error.Merge(ErrorData(ex));
			}
		}
	}

	return error;
}

void DuckTransactionManager::RollbackTransaction(Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();

	DUCKDB_LOG(db.GetDatabase(), TransactionLogType, db, "Rollback", transaction.GetTransactionId());

	ErrorData error;
	{
		// Obtain the transaction lock and roll back.
		lock_guard<mutex> t_lock(transaction_lock);
		error = transaction.Rollback();

		// Remove the transaction from the list of active transactions and gather cleanup information.
		QueueCleanup(RemoveTransaction(transaction, CreateCleanupInfo()));
	}

	CleanupTransactions();

	// Run external-resource teardowns the rollback queued (see RollbackState): they execute SQL, so
	// they can only run here, after the transaction lock is released.
	DatabaseManager::Get(db.GetDatabase()).DrainPendingTeardowns();

	if (error.HasError()) {
		throw FatalException("Failed to rollback transaction. Cannot continue operation.\nError: %s", error.Message());
	}
}

unique_ptr<DuckCleanupInfo> DuckTransactionManager::CreateCleanupInfo() {
	auto cleanup_info = make_uniq<DuckCleanupInfo>();
	// RemoveTransaction moves the transaction out of active_transactions before re-homing it in one of these
	// lists. It is noexcept, so a failure to allocate there would terminate the process and lose the transaction;
	// reserve everything it can need here instead, where throwing is safe and modifies nothing.
	cleanup_info->transactions.reserve(recently_committed_transactions.size() + 1);
	recently_committed_transactions.reserve(recently_committed_transactions.size() + 1);
	return cleanup_info;
}

unique_ptr<DuckCleanupInfo>
DuckTransactionManager::RemoveTransaction(DuckTransaction &transaction,
                                          unique_ptr<DuckCleanupInfo> cleanup_info) noexcept {
	return RemoveTransaction(transaction, transaction.ChangesMade(), std::move(cleanup_info));
}

unique_ptr<DuckCleanupInfo>
DuckTransactionManager::RemoveTransaction(DuckTransaction &transaction, bool store_transaction,
                                          unique_ptr<DuckCleanupInfo> cleanup_info) noexcept {
	// Nothing here may allocate: CreateCleanupInfo has reserved everything this needs (see its comment).

	// Find the transaction in the active transactions and recompute the lowest bound without it.
	auto t_index = UpdateLowestVisibilityBound(&transaction);
	D_ASSERT(t_index != active_transactions.size());

	// Decide if we need to store the transaction, or if we can schedule it for cleanup.
	auto current_transaction = std::move(active_transactions[t_index]);
	if (store_transaction) {
		// If the transaction made any changes, we need to keep it around.
		if (transaction.commit_id != 0) {
			// The transaction was committed.
			// We add it to the list of recently committed transactions, which is ordered on commit_id.
			// Commits that awaited a WAL sync leave the active set in the order their threads wake up, so
			// the position is not necessarily the end.
			auto position = std::upper_bound(recently_committed_transactions.begin(),
			                                 recently_committed_transactions.end(), transaction.commit_id,
			                                 [](transaction_t commit_id, const unique_ptr<DuckTransaction> &entry) {
				                                 return commit_id < entry->commit_id;
			                                 });
			recently_committed_transactions.insert(position, std::move(current_transaction));
		} else {
			// The transaction was aborted.
			cleanup_info->transactions.push_back(std::move(current_transaction));
		}
	} else if (transaction.ChangesMade()) {
		// We do not need to store the transaction, directly schedule it for cleanup.
		current_transaction->awaiting_cleanup = true;
		cleanup_info->transactions.push_back(std::move(current_transaction));
	}
	cleanup_info->lowest_visibility_bound = LowestVisibilityBound();

	// Remove the transaction from the list of active transactions.
	active_transactions.unsafe_erase_at(t_index);

	// Move the committed transactions that no snapshot can still see to the cleanup info.
	SweepCommittedTransactions(*cleanup_info);

	return cleanup_info;
}

idx_t DuckTransactionManager::UpdateLowestVisibilityBound(optional_ptr<DuckTransaction> exclude) noexcept {
	idx_t exclude_index = active_transactions.size();
	auto computed_lowest_visibility_bound = VisibilityBound::AllCommitted();
	for (idx_t i = 0; i < active_transactions.size(); i++) {
		if (exclude && active_transactions[i].get() == exclude.get()) {
			exclude_index = i;
			continue;
		}
		computed_lowest_visibility_bound =
		    VisibilityBound::Min(computed_lowest_visibility_bound, active_transactions[i]->view.visibility_bound);
	}
	lowest_visibility_bound = computed_lowest_visibility_bound;
	return exclude_index;
}

void DuckTransactionManager::SweepCommittedTransactions(DuckCleanupInfo &cleanup_info) noexcept {
#ifdef DEBUG
	// the early break below relies on this order
	for (idx_t k = 1; k < recently_committed_transactions.size(); k++) {
		D_ASSERT(recently_committed_transactions[k - 1]->commit_id < recently_committed_transactions[k]->commit_id);
	}
#endif
	idx_t i = 0;
	for (; i < recently_committed_transactions.size(); i++) {
		D_ASSERT(recently_committed_transactions[i]);
		if (recently_committed_transactions[i]->commit_id >= cleanup_info.lowest_visibility_bound) {
			// recently_committed_transactions is ordered on commit_id.
			// Thus, if the current commit_id is greater than
			// lowest_visibility_bound, any subsequent commit IDs are also greater.
			break;
		}

		recently_committed_transactions[i]->awaiting_cleanup = true;
		cleanup_info.transactions.push_back(std::move(recently_committed_transactions[i]));
	}

	if (i > 0) {
		// We moved these transactions to the list of transactions awaiting GC.
		auto start = recently_committed_transactions.begin();
		auto end = recently_committed_transactions.begin() + static_cast<int64_t>(i);
		recently_committed_transactions.erase(start, end);
	}
}

void DuckTransactionManager::QueueCleanup(unique_ptr<DuckCleanupInfo> cleanup_info) {
	if (cleanup_info->ScheduleCleanup()) {
		lock_guard<mutex> q_lock(cleanup_queue_lock);
		cleanup_queue.emplace(std::move(cleanup_info));
	}
}

idx_t DuckTransactionManager::GetCatalogVersion(Transaction &transaction_p) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	return transaction.catalog_version;
}

void DuckTransactionManager::PushCatalogEntry(Transaction &transaction_p, duckdb::CatalogEntry &entry,
                                              duckdb::data_ptr_t extra_data, duckdb::idx_t extra_data_size) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	if (!db.IsSystem() && !db.IsTemporary() && transaction.IsReadOnly()) {
		throw InternalException("Attempting to do catalog changes on a transaction that is read-only - "
		                        "this should not be possible");
	}
	transaction.catalog_version = ++last_uncommitted_catalog_version;
	transaction.PushCatalogEntry(entry, extra_data, extra_data_size);
}

void DuckTransactionManager::PushAttach(Transaction &transaction_p, AttachedDatabase &attached_db) {
	auto &transaction = transaction_p.Cast<DuckTransaction>();
	if (!db.IsSystem()) {
		throw InternalException("Can only ATTACH in the system catalog");
	}
	transaction.catalog_version = ++last_uncommitted_catalog_version;
	transaction.PushAttach(attached_db);
}

} // namespace duckdb
