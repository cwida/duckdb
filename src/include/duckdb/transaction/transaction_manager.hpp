//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/transaction_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"

#include "duckdb/common/atomic.hpp"

namespace duckdb {

class ClientContext;
class Catalog;
struct ClientLockWrapper;
class DatabaseInstance;
class Transaction;

struct StoredCatalogSet {
	//! Stored catalog set
	unique_ptr<CatalogSet> stored_set;
	//! The highest active query number when the catalog set was stored; used for cleaning up
	transaction_t highest_active_query;
};

//! The Transaction Manager is responsible for creating and managing
//! transactions
class TransactionManager {
	friend struct CheckpointLock;

public:
	explicit TransactionManager(DatabaseInstance &db);
	~TransactionManager();

	//! Start a new transaction
	Transaction *StartTransaction(ClientContext &context);
	//! Commit the given transaction
	string CommitTransaction(ClientContext &context, Transaction *transaction);
	//! Rollback the given transaction
	void RollbackTransaction(Transaction *transaction);

	transaction_t GetQueryNumber() {
		return current_query_number++;
	}

	void Checkpoint(ClientContext &context, bool force = false);

	static TransactionManager &Get(ClientContext &context);
	static TransactionManager &Get(DatabaseInstance &db);

private:
	bool CanCheckpoint(Transaction *current = nullptr);
	//! Remove the given transaction from the list of active transactions
	void RemoveTransaction(Transaction *transaction) noexcept;
	void LockClients(vector<ClientLockWrapper> &client_locks, ClientContext &context);

	//! The database instance
	DatabaseInstance &db;
	//! The current query number
	atomic<transaction_t> current_query_number;
	//! The current start timestamp used by transactions
	transaction_t current_start_timestamp;
	//! The current transaction ID used by transactions
	transaction_t current_transaction_id;
	//! Set of currently running transactions
	vector<unique_ptr<Transaction>> active_transactions;
	//! Set of recently committed transactions
	vector<unique_ptr<Transaction>> recently_committed_transactions;
	//! Transactions awaiting GC
	vector<unique_ptr<Transaction>> old_transactions;
	//! Catalog sets
	vector<StoredCatalogSet> old_catalog_sets;
	//! The lock used for transaction operations
	mutex transaction_lock;

	bool thread_is_checkpointing;
};

} // namespace duckdb
