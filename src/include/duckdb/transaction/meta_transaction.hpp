//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/meta_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/condition_variable.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {
class AttachedDatabase;
class ClientContext;
class SecretManager;
class SecretStorage;
struct DatabaseModificationType;
class Transaction;

enum class TransactionState { UNCOMMITTED, COMMITTED, ROLLED_BACK };

struct TransactionReference {
	explicit TransactionReference(Transaction &transaction_p)
	    : state(TransactionState::UNCOMMITTED), transaction(transaction_p) {
	}

	TransactionState state;
	Transaction &transaction;
};

//! The MetaTransaction manages multiple transactions for different attached databases
class MetaTransaction {
public:
	DUCKDB_API MetaTransaction(ClientContext &context, timestamp_t start_timestamp,
	                           transaction_t global_transaction_id);
	DUCKDB_API ~MetaTransaction();

	ClientContext &context;
	//! The timestamp when the transaction started
	timestamp_t start_timestamp;
	//! The global identifier of the transaction
	transaction_t global_transaction_id;
	//! The validity checker of the transaction
	ValidChecker transaction_validity;
	//! The active query number
	atomic<transaction_t> active_query;

public:
	DUCKDB_API static MetaTransaction &Get(ClientContext &context);
	timestamp_t GetCurrentTransactionStartTimestamp() const {
		return start_timestamp;
	}

	Transaction &GetTransaction(AttachedDatabase &db);
	optional_ptr<Transaction> TryGetTransaction(AttachedDatabase &db);
	void RemoveTransaction(AttachedDatabase &db);

	ErrorData Commit();
	void Rollback();
	// Finalize the transaction after a COMMIT of ROLLBACK.
	void Finalize();

	idx_t GetActiveQuery();
	void SetActiveQuery(transaction_t query_number);

	void SetReadOnly();
	bool IsReadOnly() const;
	void ModifyDatabase(AttachedDatabase &db, DatabaseModificationType modification);
	optional_ptr<AttachedDatabase> ModifiedDatabase() {
		return modified_database;
	}
	const vector<reference<AttachedDatabase>> &OpenedTransactions() const {
		return all_transactions;
	}
	optional_ptr<AttachedDatabase> GetReferencedDatabase(const Identifier &name);
	shared_ptr<AttachedDatabase> GetReferencedDatabaseOwning(const Identifier &name);
	AttachedDatabase &UseDatabase(shared_ptr<AttachedDatabase> &database);
	void DetachDatabase(AttachedDatabase &database);

private:
	friend class SecretManager;

	//! Lock to prevent all_transactions and transactions from getting out of sync.
	mutex lock;
	//! The set of active transactions for each database.
	reference_map_t<AttachedDatabase, TransactionReference> transactions;
	//! Databases for which a transaction is currently being started, and by which thread
	reference_map_t<AttachedDatabase, thread_id> starting_transactions;
	//! Signalled whenever a transaction start completes.
	condition_variable transaction_started;
	//! The set of referenced databases in invocation order.
	vector<reference<AttachedDatabase>> all_transactions;
	//! The database we are modifying. We can only modify one database per meta transaction.
	optional_ptr<AttachedDatabase> modified_database;
	//! Whether the meta transaction is marked as read only.
	bool is_read_only;
	//! Lock for referenced_databases.
	mutex referenced_database_lock;
	//! The set of used (referenced) databases.
	reference_map_t<AttachedDatabase, shared_ptr<AttachedDatabase>> referenced_databases;
	//! Map of name -> database for databases that are in-use by this transaction.
	identifier_map_t<reference<AttachedDatabase>> used_databases;
	//! Secrets that only live for the duration of this transaction.
	unique_ptr<SecretStorage> transaction_secret_storage;
};

} // namespace duckdb
