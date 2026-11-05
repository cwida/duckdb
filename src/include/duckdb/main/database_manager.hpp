//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/checked_integer.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {
class AttachedDatabase;
class Catalog;
class CatalogEntryRetriever;
class CatalogSet;
class ClientContext;
class DatabaseInstance;
class ResourceDeleter;
class TaskScheduler;
struct AttachOptions;
struct AlterInfo;

//! The DatabaseManager is a class that sits at the root of all attached databases
class DatabaseManager {
	friend class Catalog;

public:
	explicit DatabaseManager(DatabaseInstance &db);
	~DatabaseManager();

public:
	static DatabaseManager &Get(DatabaseInstance &db);
	static DatabaseManager &Get(ClientContext &db);
	static DatabaseManager &Get(AttachedDatabase &db);

	DatabaseInstance &GetInstance() {
		return db;
	}
	//! Initializes the system catalog of the attached SYSTEM_DATABASE.
	void InitializeSystemCatalog();
	//! Finalize starting up the system
	void FinalizeStartup();
	//! Get an attached database by its name
	optional_ptr<AttachedDatabase> GetDatabase(ClientContext &context, const Identifier &name);
	shared_ptr<AttachedDatabase> GetDatabase(const Identifier &name);
	//! Attach a new database
	shared_ptr<AttachedDatabase> AttachDatabase(ClientContext &context, AttachInfo &info, AttachOptions &options);

	//! Detach an existing database. SQL DETACH refuses the connection's default database, which would leave it
	//! without one; a host closing a database it opened passes `allow_default_database` to detach it regardless.
	void DetachDatabase(ClientContext &context, const Identifier &name, OnEntryNotFound if_not_found,
	                    bool allow_default_database = false);
	//! Queue the teardown of an external resource from a context that cannot run SQL (e.g. transaction
	//! rollback, under the transaction lock).
	void AddPendingTeardown(unique_ptr<ResourceDeleter> deleter);
	//! Run queued teardowns, best-effort. Called once no transaction locks are held.
	void DrainPendingTeardowns();
	//! Detach every attachment borrowing the named resource, once it has been destroyed. Best-effort:
	//! DETACH may refuse (the default database) and DESTROY goes through regardless. Scans rather than
	//! keeping a refcount -- the attachment list cannot drift, and this follows a network round-trip.
	void DetachResourceBorrowers(ClientContext &context, const string &resource_name);
	//! Alter operation dispatcher
	void Alter(ClientContext &context, AlterInfo &info);
	//! Rollback the attach of a database, given the database only when its name still refers to it
	shared_ptr<AttachedDatabase> DetachInternal(const Identifier &name,
	                                            optional_ptr<AttachedDatabase> database = nullptr);
	//! Returns a reference to the system catalog
	Catalog &GetSystemCatalog();

	//! The default database of the connection: its USE'd catalog, else the default database it connected with, as long
	//! as that is still attached. Throws when there is none; TryGetDefaultDatabase returns the empty identifier
	//! instead, for lookups that can skip it.
	static Identifier GetDefaultDatabase(ClientContext &context);
	static Identifier TryGetDefaultDatabase(ClientContext &context);
	//! The default database new connections start with; empty when none is set.
	Identifier GetDefaultDatabase();
	//! Sets the default database for new connections, which must be attached; the empty identifier clears it. Set to
	//! the main database at startup and never set implicitly by an attach; detaching it falls back to the oldest
	//! remaining database, if any.
	void SetDefaultDatabase(const Identifier &name);

	//! Inserts a path to name mapping to the database paths map
	InsertDatabasePathResult InsertDatabasePath(const AttachInfo &info, AttachOptions &options);

	//! Returns the database type. This might require checking the header of the file, in which case the file handle is
	//! necessary. We can only grab the file handle, if it is not yet held, even for uncommitted changes. Thus, we have
	//! to lock for this operation.
	void GetDatabaseType(ClientContext &context, AttachInfo &info, const DBConfig &config, AttachOptions &options);
	//! Scans the catalog set and adds each committed database entry, and each database entry of the current
	//! transaction, to a vector holding AttachedDatabase references
	vector<shared_ptr<AttachedDatabase>> GetDatabases(ClientContext &context,
	                                                  const optional_idx max_db_count = optional_idx());
	//! Scans the catalog set and returns each committed database entry
	vector<shared_ptr<AttachedDatabase>> GetDatabases();
	//! Returns the approximate count of attached databases.
	idx_t ApproxDatabaseCount();
	//! Returns the number of remote catalogs currently attached.
	idx_t GetRemoteCatalogCount() const {
		return remote_catalog_count.load();
	}
	//! Removes all databases from the catalog set. This is necessary for the database instance's destructor,
	//! as the database manager has to be alive when destroying the catalog set objects.
	void ResetDatabases();

	transaction_t GetNewQueryNumber() {
		return current_query_number++;
	}
	transaction_t ActiveQueryNumber() const {
		return current_query_number;
	}
	transaction_t GetNewTransactionNumber() {
		return current_transaction_id++;
	}
	transaction_t ActiveTransactionNumber() const {
		return current_transaction_id;
	}
	idx_t NextOid() {
		return next_oid++;
	}
	bool HasAttachedDatabase() {
		lock_guard<mutex> guard(databases_lock);
		return !databases.empty();
	}
	//! Gets a list of all attached database paths
	vector<string> GetAttachedDatabasePaths();
	//! Whether the file at the path (or its WAL) belongs to an attached database
	bool IsAttachedDatabaseFile(ClientContext &context, const string &path);

	shared_ptr<AttachedDatabase> GetDatabaseInternal(const lock_guard<mutex> &, const Identifier &name);

private:
	optional_ptr<AttachedDatabase> FinalizeAttach(ClientContext &context, AttachInfo &info,
	                                              shared_ptr<AttachedDatabase> database);

private:
	DatabaseInstance &db;
	//! The system database is a special database that holds system entries (e.g. functions)
	shared_ptr<AttachedDatabase> system;
	//! Lock for databases
	mutex databases_lock;
	//! The set of attached databases
	identifier_map_t<shared_ptr<AttachedDatabase>> databases;
	//! The default database for new connections; empty when none is set (guarded by databases_lock)
	Identifier default_database;
	//! The next object id handed out by the NextOid method
	atomic<idx_t> next_oid;
	//! The current query number
	atomic<transaction_t> current_query_number;
	//! The current transaction number
	atomic<transaction_t> current_transaction_id;
	//! Count of remote catalogs currently attached; used to skip the remote pushdown optimizer when zero
	atomic<CheckedInteger<idx_t, InternalException>> remote_catalog_count;
	//! Lock for pending_teardowns
	mutex pending_teardowns_lock;
	//! External-resource teardowns queued from contexts that cannot run SQL (see AddPendingTeardown);
	//! drained best-effort once no transaction locks are held.
	vector<unique_ptr<ResourceDeleter>> pending_teardowns;
	//! Manager for ensuring we never open the same database file twice in the same program
	shared_ptr<DatabaseFilePathManager> path_manager;

private:
	//! Rename an existing database
	void RenameDatabase(ClientContext &context, const Identifier &old_name, const Identifier &new_name,
	                    OnEntryNotFound if_not_found);
};

} // namespace duckdb
