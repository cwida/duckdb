#include "duckdb/transaction/transaction_manager.hpp"

#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection_manager.hpp"

namespace duckdb {

struct CheckpointLock {
	explicit CheckpointLock(TransactionManager &manager) : manager(manager), is_locked(false) {
	}
	~CheckpointLock() {
		Unlock();
	}

	TransactionManager &manager;
	bool is_locked;

	void Lock() {
		D_ASSERT(!manager.thread_is_checkpointing);
		manager.thread_is_checkpointing = true;
		is_locked = true;
	}
	void Unlock() {
		if (!is_locked) {
			return;
		}
		D_ASSERT(manager.thread_is_checkpointing);
		manager.thread_is_checkpointing = false;
		is_locked = false;
	}
};

TransactionManager::TransactionManager(DatabaseInstance &db) : db(db), thread_is_checkpointing(false) {
	// start timestamp starts at zero
	current_start_timestamp = 0;
	// transaction ID starts very high:
	// it should be much higher than the current start timestamp
	// if transaction_id < start_timestamp for any set of active transactions
	// uncommited data could be read by
	current_transaction_id = TRANSACTION_ID_START;
	// the current active query id
	current_query_number = 1;
}

TransactionManager::~TransactionManager() {
}

Transaction *TransactionManager::StartTransaction(ClientContext &context) {
	// obtain the transaction lock during this function
	lock_guard<mutex> lock(transaction_lock);
	if (current_start_timestamp >= TRANSACTION_ID_START) { // LCOV_EXCL_START
		throw InternalException("Cannot start more transactions, ran out of "
		                        "transaction identifiers!");
	} // LCOV_EXCL_STOP

	// obtain the start time and transaction ID of this transaction
	transaction_t start_time = current_start_timestamp++;
	transaction_t transaction_id = current_transaction_id++;
	timestamp_t start_timestamp = Timestamp::GetCurrentTimestamp();

	// create the actual transaction
	auto &catalog = Catalog::GetCatalog(db);
	auto transaction = make_unique<Transaction>(weak_ptr<ClientContext>(context.shared_from_this()), start_time,
	                                            transaction_id, start_timestamp, catalog.GetCatalogVersion());
	auto transaction_ptr = transaction.get();

	// store it in the set of active transactions
	active_transactions.push_back(move(transaction));
	return transaction_ptr;
}

struct ClientLockWrapper {
	ClientLockWrapper(mutex &client_lock, shared_ptr<ClientContext> connection)
	    : connection(move(connection)), connection_lock(make_unique<lock_guard<mutex>>(client_lock)) {
	}

	shared_ptr<ClientContext> connection;
	unique_ptr<lock_guard<mutex>> connection_lock;
};

void TransactionManager::LockClients(vector<ClientLockWrapper> &client_locks, ClientContext &context) {
	auto &connection_manager = ConnectionManager::Get(context);
	client_locks.emplace_back(connection_manager.connections_lock, nullptr);
	auto connection_list = connection_manager.GetConnectionList();
	for (auto &con : connection_list) {
		if (con.get() == &context) {
			continue;
		}
		auto &context_lock = con->context_lock;
		client_locks.emplace_back(context_lock, move(con));
	}
}

void TransactionManager::Checkpoint(ClientContext &context, bool force) {
	auto &storage_manager = StorageManager::GetStorageManager(db);
	if (storage_manager.InMemory()) {
		return;
	}

	// first check if no other thread is checkpointing right now
	auto lock = make_unique<lock_guard<mutex>>(transaction_lock);
	if (thread_is_checkpointing) {
		throw TransactionException("Cannot CHECKPOINT: another thread is checkpointing right now");
	}
	CheckpointLock checkpoint_lock(*this);
	checkpoint_lock.Lock();
	lock.reset();

	// lock all the clients AND the connection manager now
	// this ensures no new queries can be started, and no new connections to the database can be made
	// to avoid deadlock we release the transaction lock while locking the clients
	vector<ClientLockWrapper> client_locks;
	LockClients(client_locks, context);

	lock = make_unique<lock_guard<mutex>>(transaction_lock);
	auto current = &Transaction::GetTransaction(context);
	if (current->ChangesMade()) {
		throw TransactionException("Cannot CHECKPOINT: the current transaction has transaction local changes");
	}
	if (!force) {
		if (!CanCheckpoint(current)) {
			throw TransactionException("Cannot CHECKPOINT: there are other transactions. Use FORCE CHECKPOINT to abort "
			                           "the other transactions and force a checkpoint");
		}
	} else {
		if (!CanCheckpoint(current)) {
			for (size_t i = 0; i < active_transactions.size(); i++) {
				auto &transaction = active_transactions[i];
				// rollback the transaction
				transaction->Rollback();
				auto transaction_context = transaction->context.lock();

				// remove the transaction id from the list of active transactions
				// potentially resulting in garbage collection
				RemoveTransaction(transaction.get());
				if (transaction_context) {
					transaction_context->transaction.ClearTransaction();
				}
				i--;
			}
			D_ASSERT(CanCheckpoint(nullptr));
		}
	}
	auto &storage = StorageManager::GetStorageManager(context);
	storage.CreateCheckpoint();
}

bool TransactionManager::CanCheckpoint(Transaction *current) {
	auto &storage_manager = StorageManager::GetStorageManager(db);
	if (storage_manager.InMemory()) {
		return false;
	}
	if (!recently_committed_transactions.empty() || !old_transactions.empty()) {
		return false;
	}
	for (auto &transaction : active_transactions) {
		if (transaction.get() != current) {
			return false;
		}
	}
	return true;
}

string TransactionManager::CommitTransaction(ClientContext &context, Transaction *transaction) {
	vector<ClientLockWrapper> client_locks;
	auto lock = make_unique<lock_guard<mutex>>(transaction_lock);
	CheckpointLock checkpoint_lock(*this);
	// check if we can checkpoint
	bool checkpoint = thread_is_checkpointing ? false : CanCheckpoint(transaction);
	if (checkpoint) {
		if (transaction->AutomaticCheckpoint(db)) {
			checkpoint_lock.Lock();
			// we might be able to checkpoint: lock all clients
			// to avoid deadlock we release the transaction lock while locking the clients
			lock.reset();

			LockClients(client_locks, context);

			lock = make_unique<lock_guard<mutex>>(transaction_lock);
			checkpoint = CanCheckpoint(transaction);
			if (!checkpoint) {
				checkpoint_lock.Unlock();
				client_locks.clear();
			}
		} else {
			checkpoint = false;
		}
	}
	// obtain a commit id for the transaction
	transaction_t commit_id = current_start_timestamp++;
	// commit the UndoBuffer of the transaction
	string error = transaction->Commit(db, commit_id, checkpoint);
	if (!error.empty()) {
		// commit unsuccessful: rollback the transaction instead
		checkpoint = false;
		transaction->commit_id = 0;
		transaction->Rollback();
	}
	if (!checkpoint) {
		// we won't checkpoint after all: unlock the clients again
		checkpoint_lock.Unlock();
		client_locks.clear();
	}

	// commit successful: remove the transaction id from the list of active transactions
	// potentially resulting in garbage collection
	RemoveTransaction(transaction);
	// now perform a checkpoint if (1) we are able to checkpoint, and (2) the WAL has reached sufficient size to
	// checkpoint
	if (checkpoint) {
		// checkpoint the database to disk
		auto &storage_manager = StorageManager::GetStorageManager(db);
		storage_manager.CreateCheckpoint(false, true);
	}
	return error;
}

void TransactionManager::RollbackTransaction(Transaction *transaction) {
	// obtain the transaction lock during this function
	lock_guard<mutex> lock(transaction_lock);

	// rollback the transaction
	transaction->Rollback();

	// remove the transaction id from the list of active transactions
	// potentially resulting in garbage collection
	RemoveTransaction(transaction);
}

void TransactionManager::RemoveTransaction(Transaction *transaction) noexcept {
	// remove the transaction from the list of active transactions
	idx_t t_index = active_transactions.size();
	// check for the lowest and highest start time in the list of transactions
	transaction_t lowest_start_time = TRANSACTION_ID_START;
	transaction_t lowest_active_query = MAXIMUM_QUERY_ID;
	for (idx_t i = 0; i < active_transactions.size(); i++) {
		if (active_transactions[i].get() == transaction) {
			t_index = i;
		} else {
			transaction_t active_query = active_transactions[i]->active_query;
			lowest_start_time = MinValue(lowest_start_time, active_transactions[i]->start_time);
			lowest_active_query = MinValue(lowest_active_query, active_query);
		}
	}
	transaction_t lowest_stored_query = lowest_start_time;
	D_ASSERT(t_index != active_transactions.size());
	auto current_transaction = move(active_transactions[t_index]);
	if (transaction->commit_id != 0) {
		// the transaction was committed, add it to the list of recently
		// committed transactions
		recently_committed_transactions.push_back(move(current_transaction));
	} else {
		// the transaction was aborted, but we might still need its information
		// add it to the set of transactions awaiting GC
		current_transaction->highest_active_query = current_query_number;
		old_transactions.push_back(move(current_transaction));
	}
	// remove the transaction from the set of currently active transactions
	active_transactions.erase(active_transactions.begin() + t_index);
	// traverse the recently_committed transactions to see if we can remove any
	idx_t i = 0;
	for (; i < recently_committed_transactions.size(); i++) {
		D_ASSERT(recently_committed_transactions[i]);
		lowest_stored_query = MinValue(recently_committed_transactions[i]->start_time, lowest_stored_query);
		if (recently_committed_transactions[i]->commit_id < lowest_start_time) {
			// changes made BEFORE this transaction are no longer relevant
			// we can cleanup the undo buffer

			// HOWEVER: any currently running QUERY can still be using
			// the version information after the cleanup!

			// if we remove the UndoBuffer immediately, we have a race
			// condition

			// we can only safely do the actual memory cleanup when all the
			// currently active queries have finished running! (actually,
			// when all the currently active scans have finished running...)
			recently_committed_transactions[i]->Cleanup();
			// store the current highest active query
			recently_committed_transactions[i]->highest_active_query = current_query_number;
			// move it to the list of transactions awaiting GC
			old_transactions.push_back(move(recently_committed_transactions[i]));
		} else {
			// recently_committed_transactions is ordered on commit_id
			// implicitly thus if the current one is bigger than
			// lowest_start_time any subsequent ones are also bigger
			break;
		}
	}
	if (i > 0) {
		// we garbage collected transactions: remove them from the list
		recently_committed_transactions.erase(recently_committed_transactions.begin(),
		                                      recently_committed_transactions.begin() + i);
	}
	// check if we can free the memory of any old transactions
	i = active_transactions.empty() ? old_transactions.size() : 0;
	for (; i < old_transactions.size(); i++) {
		D_ASSERT(old_transactions[i]);
		D_ASSERT(old_transactions[i]->highest_active_query > 0);
		if (old_transactions[i]->highest_active_query >= lowest_active_query) {
			// there is still a query running that could be using
			// this transactions' data
			break;
		}
	}
	if (i > 0) {
		// we garbage collected transactions: remove them from the list
		old_transactions.erase(old_transactions.begin(), old_transactions.begin() + i);
	}
	// check if we can free the memory of any old catalog sets
	for (i = 0; i < old_catalog_sets.size(); i++) {
		D_ASSERT(old_catalog_sets[i].highest_active_query > 0);
		if (old_catalog_sets[i].highest_active_query >= lowest_stored_query) {
			// there is still a query running that could be using
			// this catalog sets' data
			break;
		}
	}
	if (i > 0) {
		// we garbage collected catalog sets: remove them from the list
		old_catalog_sets.erase(old_catalog_sets.begin(), old_catalog_sets.begin() + i);
	}
}

} // namespace duckdb
