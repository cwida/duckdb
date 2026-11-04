#include "duckdb/main/database_manager.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_path_and_type.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/alter_database_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

// Oids are started at 20000 to avoid colliding with Postgres builtin types, which end at 16383:
// https://github.com/postgres/postgres/blob/db93988ab0e78396f2ed9e96c826ff988d12b9f2/src/include/access/transam.h#L156-L197
DatabaseManager::DatabaseManager(DatabaseInstance &db)
    : db(db), next_oid(20000), current_query_number(1), current_transaction_id(0), remote_catalog_count(0) {
	system = make_shared_ptr<AttachedDatabase>(db);
	auto &config = DBConfig::GetConfig(db);
	path_manager = config.path_manager;
	if (!path_manager) {
		// no shared path manager
		path_manager = make_shared_ptr<DatabaseFilePathManager>();
	}
}

DatabaseManager::~DatabaseManager() {
}

DatabaseManager &DatabaseManager::Get(AttachedDatabase &db) {
	return DatabaseManager::Get(db.GetDatabase());
}

void DatabaseManager::InitializeSystemCatalog() {
	// The SYSTEM_DATABASE has no persistent storage.
	system->Initialize();
}

void DatabaseManager::FinalizeStartup() {
	auto dbs = GetDatabases();
	for (auto &db : dbs) {
		db->FinalizeLoad(nullptr);
	}
}

optional_ptr<AttachedDatabase> DatabaseManager::GetDatabase(ClientContext &context, const Identifier &name) {
	auto &meta_transaction = MetaTransaction::Get(context);
	// first check if we have a local reference to this database already
	auto database = meta_transaction.GetReferencedDatabase(name);
	if (database) {
		// we do! return it
		return database;
	}
	lock_guard<mutex> guard(databases_lock);
	shared_ptr<AttachedDatabase> db;
	if (name == TEMP_CATALOG) {
		db = context.client_data->temporary_objects;
	} else {
		db = GetDatabaseInternal(guard, name);
	}
	if (!db) {
		return nullptr;
	}
	return meta_transaction.UseDatabase(db);
}

shared_ptr<AttachedDatabase> DatabaseManager::GetDatabase(const Identifier &name) {
	lock_guard<mutex> guard(databases_lock);
	return GetDatabaseInternal(guard, name);
}

shared_ptr<AttachedDatabase> DatabaseManager::GetDatabaseInternal(const lock_guard<mutex> &, const Identifier &name) {
	if (name == SYSTEM_CATALOG) {
		return system;
	}
	auto entry = databases.find(name);
	if (entry == databases.end()) {
		// not found
		return nullptr;
	}
	return entry->second;
}

bool RequiresTrackingAttaches(const string &path, const string &db_type) {
	// we need to track attaches for file-based duckdb databases
	if (!db_type.empty() && !StringUtil::CIEquals(db_type, "duckdb")) {
		// not duckdb - don't track
		return false;
	}
	if (path.empty() || path == IN_MEMORY_PATH) {
		// in-memory - don't track
		return false;
	}
	// file-based duckdb - track
	return true;
}

shared_ptr<AttachedDatabase> DatabaseManager::AttachDatabase(ClientContext &context, AttachInfo &info,
                                                             AttachOptions &options) {
	string extension = "";
	// a storage extension named by an explicit TYPE interprets the path itself (e.g. as a connection URI)
	// so a remote-looking path only implies a remote file (read-only, requiring e.g. httpfs) otherwise
	bool explicit_storage_type = !options.db_type.empty() && !StringUtil::CIEquals(options.db_type, "duckdb");
	if (!explicit_storage_type && FileSystem::IsRemoteFile(info.path, extension)) {
		if (options.access_mode == AccessMode::AUTOMATIC) {
			// Attaching of remote files gets bumped to READ_ONLY
			// This is due to the fact that on most (all?) remote files writes to DB are not available
			// and having this raised later is not super helpful
			options.access_mode = AccessMode::READ_ONLY;
		}
	}
	bool requires_tracking_attaches = RequiresTrackingAttaches(info.path, options.db_type);
	if (requires_tracking_attaches) {
		// canonicalize the path to the database
		auto &fs = FileSystem::GetFileSystem(context);
		info.path = fs.CanonicalizePath(info.path);
	}

	// for IGNORE / REPLACE ON CONFLICT - first look for an existing entry
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT ||
	    info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// constant-time lookup in the catalog for the db name
		auto existing_db = GetDatabase(info.name);
		if (existing_db) {
			if ((existing_db->IsReadOnly() && options.access_mode == AccessMode::READ_WRITE) ||
			    (!existing_db->IsReadOnly() && options.access_mode == AccessMode::READ_ONLY)) {
				auto existing_mode = existing_db->IsReadOnly() ? AccessMode::READ_ONLY : AccessMode::READ_WRITE;
				auto existing_mode_str = EnumUtil::ToString(existing_mode);
				auto attached_mode = EnumUtil::ToString(options.access_mode);
				throw BinderException("Database %s is already attached in %s mode, cannot re-attach in %s mode",
				                      info.name, existing_mode_str, attached_mode);
			}
			if (!options.default_table.Name().empty()) {
				existing_db->GetCatalog().SetDefaultTable(options.default_table.Schema(), options.default_table.Name());
			}
			if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
				// we require the vacuuming threshold for indexed tables to be the same as the already attached db
				if (options.vacuum_rebuild_indexes_threshold.IsValid()) {
					auto previous_setting = existing_db->GetVacuumRebuildIndexThreshold();
					auto new_setting = options.vacuum_rebuild_indexes_threshold.GetIndex();
					if (previous_setting != new_setting) {
						throw BinderException("Cannot re-attach with a different vacuum_rebuild_indexes setting "
						                      "(previous: %d, new: %d)",
						                      previous_setting, new_setting);
					}
				}
				// allow custom catalogs to override this behavior
				if (!existing_db->GetCatalog().HasConflictingAttachOptions(info.path, options)) {
					return existing_db;
				}
			} else {
				return existing_db;
			}
		}
	}

	if (requires_tracking_attaches) {
		// Start timing the ATTACH-delay step.
		auto timer = context.client_data->profiler->StartTimer<MetricStorageWaitingToAttachLatency>();
		// Start trying to attach.
		while (InsertDatabasePath(info, options) == InsertDatabasePathResult::ALREADY_EXISTS) {
			// database with this name and path already exists
			// first check if it exists within this transaction
			auto &meta_transaction = MetaTransaction::Get(context);
			if (auto existing_db = meta_transaction.GetReferencedDatabaseOwning(info.name)) {
				// it does! return it
				return existing_db;
			}

			// ... but it might not be done attaching yet!
			// verify the database has actually finished attaching prior to returning
			lock_guard<mutex> guard(databases_lock);
			auto entry = databases.find(info.name);
			if (entry != databases.end()) {
				// The database ACTUALLY exists, so we return it.
				return entry->second;
			}
			context.InterruptCheck();
		}
		// Returning in the loop above will also end the timer, otherwise, do it explicitly here.
		timer.EndTimer();
	}
	auto &config = DBConfig::GetConfig(context);
	GetDatabaseType(context, info, config, options);
	if (!options.db_type.empty()) {
		// we only need to prevent duplicate opening of DuckDB files
		// if this is not a DuckDB file but e.g. a CSV or Parquet file, we don't need to do this duplicate protection
		options.stored_database_path.reset();
	}
	if (AttachedDatabase::NameIsReserved(info.name)) {
		throw BinderException("Attached database name %s cannot be used because it is a reserved name", info.name);
	}
	if (!extension.empty()) {
		if (!ExtensionHelper::TryAutoLoadExtension(context, extension)) {
			throw MissingExtensionException("Attaching path '%s' requires extension '%s' to be loaded", info.path,
			                                extension);
		}
	}

	// now create the attached database
	auto &db = DatabaseInstance::GetDatabase(context);
	auto attached_db = db.CreateAttachedDatabase(context, info, options);

	//! Initialize the database.
	if (options.is_main_database) {
		attached_db->SetInitialDatabase();
		attached_db->Initialize(context);
	} else {
		attached_db->Initialize(context);
		if (!options.default_table.Name().empty()) {
			attached_db->GetCatalog().SetDefaultTable(options.default_table.Schema(), options.default_table.Name());
		}
		attached_db->FinalizeLoad(context);
	}

	FinalizeAttach(context, info, attached_db);
	return attached_db;
}

optional_ptr<AttachedDatabase> DatabaseManager::FinalizeAttach(ClientContext &context, AttachInfo &info,
                                                               shared_ptr<AttachedDatabase> attached_db) {
	const auto name = attached_db->GetName();
	attached_db->oid = NextOid();
	shared_ptr<AttachedDatabase> detached_db;
	{
		lock_guard<mutex> guard(databases_lock);
		auto entry = databases.emplace(name, attached_db);
		if (!entry.second) {
			if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
				// override existing entry
				detached_db = std::move(entry.first->second);
				databases[name] = attached_db;
			} else {
				throw BinderException("Failed to attach database: database with name %s already exists", name);
			}
		}
	}
	auto &meta_transaction = MetaTransaction::Get(context);
	if (detached_db) {
		if (detached_db->GetCatalog().Supports(RemoteCapability::IS_REMOTE)) {
			--remote_catalog_count;
		}
		auto deleter = detached_db->ExtractDeleter();
		meta_transaction.DetachDatabase(*detached_db);
		detached_db->OnDetach(context);
		detached_db.reset();
		// Best-effort teardown: the replacement is already in effect, so a deleter failure must not
		// abort the statement mid-swap; TryDelete logs it as a warning instead.
		if (deleter) {
			deleter->TryDelete();
		}
	}
	if (attached_db->GetCatalog().Supports(RemoteCapability::IS_REMOTE)) {
		++remote_catalog_count;
	}
	auto &db_ref = meta_transaction.UseDatabase(attached_db);
	auto &transaction = DuckTransaction::Get(context, *system);
	auto &transaction_manager = DuckTransactionManager::Get(*system);
	transaction_manager.PushAttach(transaction, db_ref);
	return db_ref;
}

void DatabaseManager::DetachDatabase(ClientContext &context, const Identifier &name, OnEntryNotFound if_not_found,
                                     bool allow_default_database) {
	if (!allow_default_database && TryGetDefaultDatabase(context) == name) {
		throw BinderException("Cannot detach database %s because it is the default database. Select a different "
		                      "database using `USE` to allow detaching this database",
		                      name);
	}

	auto attached_db = DetachInternal(name);
	if (!attached_db) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw BinderException("Failed to detach database with name %s: database not found", name);
		}
		return;
	}

	auto deleter = attached_db->ExtractDeleter();
	MetaTransaction::Get(context).DetachDatabase(*attached_db);
	attached_db->OnDetach(context);

	// DetachInternal removes the AttachedDatabase from the list of databases that can be referenced.
	AttachedDatabase::InvokeCloseIfLastReference(attached_db, context);

	// Tear down the owned external resource last (runs SQL): the detach itself is complete either way,
	// and a teardown failure surfaces with instructions to retry it manually.
	if (deleter) {
		deleter->Delete();
	}
}

void DatabaseManager::AddPendingTeardown(unique_ptr<ResourceDeleter> deleter) {
	lock_guard<mutex> guard(pending_teardowns_lock);
	pending_teardowns.push_back(std::move(deleter));
}

void DatabaseManager::DrainPendingTeardowns() {
	vector<unique_ptr<ResourceDeleter>> to_run;
	{
		lock_guard<mutex> guard(pending_teardowns_lock);
		to_run = std::move(pending_teardowns);
		pending_teardowns.clear();
	}
	// Best-effort: the statement that owned these resources is gone, so a failure can only be logged.
	for (auto &deleter : to_run) {
		deleter->TryDelete();
	}
}

void DatabaseManager::DetachResourceBorrowers(ClientContext &context, const string &resource_name) {
	vector<Identifier> to_detach;
	{
		lock_guard<mutex> guard(databases_lock);
		for (auto &entry : databases) {
			// Both sides come from ColId -> Identifier::GetIdentifierName(), matching how the manager keys names.
			if (entry.second->GetBorrowedResourceName() == resource_name) {
				to_detach.push_back(entry.second->GetName());
			}
		}
	}
	// Detach outside the lock: DetachDatabase takes databases_lock itself. A borrowed attachment binds no
	// deleter, so this runs no teardown -- the resource is already gone.
	for (auto &name : to_detach) {
		try {
			DetachDatabase(context, name, OnEntryNotFound::RETURN_NULL);
		} catch (std::exception &) {
			// Best-effort: DETACH refuses a connection's default database, and DESTROY goes through anyway.
			// Deliberately not marked invalid -- ValidChecker would then throw for every statement
			// resolving through it, including the USE and DETACH needed to recover.
		}
	}
}

void DatabaseManager::Alter(ClientContext &context, AlterInfo &info) {
	auto &db_info = info.Cast<AlterDatabaseInfo>();

	switch (db_info.alter_database_type) {
	case AlterDatabaseType::RENAME_DATABASE: {
		auto &rename_info = db_info.Cast<RenameDatabaseInfo>();
		RenameDatabase(context, db_info.GetQualifiedName().Catalog(), rename_info.new_name, db_info.if_not_found);
		break;
	}
	default:
		throw InternalException("Unsupported ALTER DATABASE operation");
	}
}

void DatabaseManager::RenameDatabase(ClientContext &context, const Identifier &old_name, const Identifier &new_name,
                                     OnEntryNotFound if_not_found) {
	if (AttachedDatabase::NameIsReserved(new_name)) {
		throw BinderException("Database name %s cannot be used because it is a reserved name", new_name);
	}

	shared_ptr<AttachedDatabase> attached_db;
	{
		lock_guard<mutex> guard(databases_lock);
		auto old_entry = databases.find(old_name);
		if (old_entry == databases.end()) {
			if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw BinderException("Failed to rename database %s: database not found", old_name);
			}
			return;
		}

		auto new_entry = databases.find(new_name);
		if (new_entry != databases.end()) {
			throw BinderException("Failed to rename database %s to %s: database with new name already exists", old_name,
			                      new_name);
		}

		attached_db = old_entry->second;
		databases.erase(old_entry);
		attached_db->SetName(new_name);
		databases[new_name] = attached_db;
		if (default_database == old_name) {
			default_database = new_name;
		}
	}
}

shared_ptr<AttachedDatabase> DatabaseManager::DetachInternal(const Identifier &name,
                                                             optional_ptr<AttachedDatabase> database) {
	shared_ptr<AttachedDatabase> attached_db;
	{
		lock_guard<mutex> guard(databases_lock);
		auto entry = databases.find(name);
		if (entry == databases.end() || (database && entry->second.get() != database.get())) {
			return nullptr;
		}
		attached_db = std::move(entry->second);
		databases.erase(entry);
		if (default_database == name) {
			// Fall back to the oldest remaining database (OIDs are assigned in attach order)
			// Arguably, this should not be required - we should just leave the default database unset.
			// The default used to be defined as the oldest attached database, so detaching the current default changed
			// the new default to "unset". Keep the previous observable behavior rather than leave new connections
			// without a default db, since SQL has no way to set the default database at the instance level.
			default_database = Identifier();
			idx_t oldest_oid = DConstants::INVALID_INDEX;
			for (auto &other : databases) {
				if (other.second->oid < oldest_oid) {
					oldest_oid = other.second->oid;
					default_database = other.first;
				}
			}
		}
	}
	if (attached_db && attached_db->GetCatalog().Supports(RemoteCapability::IS_REMOTE)) {
		--remote_catalog_count;
	}
	return attached_db;
}

idx_t DatabaseManager::ApproxDatabaseCount() {
	return path_manager->ApproxDatabaseCount();
}

InsertDatabasePathResult DatabaseManager::InsertDatabasePath(const AttachInfo &info, AttachOptions &options) {
	return path_manager->InsertDatabasePath(*this, info.path, info.name, info.on_conflict, options);
}

bool DatabaseManager::IsAttachedDatabaseFile(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	// attached database paths are canonicalized when the database is attached
	auto canonical_path = fs.CanonicalizePath(path);
	if (path_manager->IsDatabasePath(canonical_path)) {
		return true;
	}
	// the WAL files of the database
	for (auto &wal_suffix : {".wal", ".wal.checkpoint", ".wal.recovery"}) {
		if (StringUtil::EndsWith(canonical_path, wal_suffix)) {
			auto database_path = canonical_path.substr(0, canonical_path.size() - strlen(wal_suffix));
			if (path_manager->IsDatabasePath(database_path)) {
				return true;
			}
		}
	}
	return false;
}

vector<string> DatabaseManager::GetAttachedDatabasePaths() {
	vector<string> result;
	lock_guard<mutex> guard(databases_lock);
	for (auto &entry : databases) {
		auto &db_ref = *entry.second;
		auto &catalog = db_ref.GetCatalog();
		if (catalog.InMemory() || catalog.IsSystemCatalog()) {
			continue;
		}
		auto path = catalog.GetDBPath();
		if (path.empty()) {
			continue;
		}
		result.push_back(std::move(path));
	}
	return result;
}

void DatabaseManager::GetDatabaseType(ClientContext &context, AttachInfo &info, const DBConfig &config,
                                      AttachOptions &options) {
	// Test if the database is a DuckDB database file.
	if (StringUtil::CIEquals(options.db_type, "duckdb")) {
		options.db_type = "";
		return;
	}

	// Try to extract the database type from the path.
	if (options.db_type.empty()) {
		auto &fs = FileSystem::GetFileSystem(context);
		// Prefetch the header for a DuckDB file, reused when opening it (see AttachOptions::prefetched).
		DBPathAndType::CheckMagicBytes(context, fs, info.path, options.db_type, &options.prefetched);
	}

	if (options.db_type.empty()) {
		return;
	}

	auto storage_extension_name = ExtensionHelper::ApplyExtensionAlias(options.db_type);
	if (StorageExtension::Find(config, storage_extension_name)) {
		// If the database type is already registered, we don't need to load it again.
		return;
	}
	auto extension_name =
	    ExtensionHelper::FindExtensionInEntries(Identifier(storage_extension_name), EXTENSION_STORAGE_EXTENSIONS);
	if (extension_name.empty()) {
		//! Couldn't find a mapping, assume that the storage extension name is also the extension name
		extension_name = storage_extension_name;
	}

	// If we are loading a database type from an extension, then we need to check if that extension is loaded.
	if (!Catalog::TryAutoLoad(context, extension_name)) {
		// FIXME: Here it might be preferable to use an AutoLoadOrThrow kind of function
		// so that either there will be success or a message to throw, and load will be
		// attempted only once respecting the auto-loading options
		ExtensionHelper::LoadExternalExtension(context, {extension_name});
	}
}

Identifier DatabaseManager::GetDefaultDatabase(ClientContext &context) {
	auto result = TryGetDefaultDatabase(context);
	if (!IsInvalidCatalog(result)) {
		return result;
	}
	auto &connected_with = ClientData::Get(context).default_database;
	if (!IsInvalidCatalog(connected_with)) {
		throw CatalogException("The default database %s has been detached: select another database using `USE`",
		                       connected_with);
	}
	// Reachable on an instance created without a database (DuckDB::CreateEmpty).
	throw CatalogException("No database is attached: attach one with ATTACH before referring to a database");
}

Identifier DatabaseManager::TryGetDefaultDatabase(ClientContext &context) {
	auto &client_data = ClientData::Get(context);
	auto &default_entry = client_data.catalog_search_path->GetDefault();
	if (!IsInvalidCatalog(default_entry.GetCatalog())) {
		return default_entry.GetCatalog();
	}
	auto &manager = DatabaseManager::Get(context);
	// Callable outside a transaction: only consult the transaction's references when one is active.
	const bool in_transaction = context.transaction.HasActiveTransaction();
	if (!IsInvalidCatalog(client_data.default_database)) {
		// still the default while attached, or while this transaction still references it
		if (manager.GetDatabase(client_data.default_database)) {
			return client_data.default_database;
		}
		if (in_transaction && MetaTransaction::Get(context).GetReferencedDatabase(client_data.default_database)) {
			return client_data.default_database;
		}
		return Identifier();
	}
	if (in_transaction && !manager.HasAttachedDatabase()) {
		auto modified_database = MetaTransaction::Get(context).ModifiedDatabase();
		if (modified_database) {
			return modified_database->GetName();
		}
	}
	return Identifier();
}

Identifier DatabaseManager::GetDefaultDatabase() {
	lock_guard<mutex> guard(databases_lock);
	return default_database;
}

void DatabaseManager::SetDefaultDatabase(const Identifier &name) {
	lock_guard<mutex> guard(databases_lock);
	if (!IsInvalidCatalog(name) && databases.find(name) == databases.end()) {
		throw BinderException("Cannot make database %s the default database: it is not attached", name);
	}
	default_database = name;
}

vector<shared_ptr<AttachedDatabase>> DatabaseManager::GetDatabases(ClientContext &context,
                                                                   const optional_idx max_db_count) {
	vector<shared_ptr<AttachedDatabase>> result;

	lock_guard<mutex> guard(databases_lock);
	idx_t count = 2;
	for (auto &entry : databases) {
		if (max_db_count.IsValid() && count >= max_db_count.GetIndex()) {
			break;
		}
		result.push_back(entry.second);
		count++;
	}
	if (!max_db_count.IsValid() || max_db_count.GetIndex() >= 1) {
		result.push_back(system);
	}
	if (!max_db_count.IsValid() || max_db_count.GetIndex() >= 2) {
		result.push_back(context.client_data->temporary_objects);
	}

	return result;
}

vector<shared_ptr<AttachedDatabase>> DatabaseManager::GetDatabases() {
	vector<shared_ptr<AttachedDatabase>> result;

	lock_guard<mutex> guard(databases_lock);
	for (auto &entry : databases) {
		result.push_back(entry.second);
	}
	result.push_back(system);
	return result;
}

void DatabaseManager::ResetDatabases() {
	auto shared_db_pointers = GetDatabases();
	for (auto &entry : shared_db_pointers) {
		entry->Close(DatabaseCloseAction::TRY_CHECKPOINT);
		entry.reset();
	}
}

Catalog &DatabaseManager::GetSystemCatalog() {
	D_ASSERT(system);
	return system->GetCatalog();
}

} // namespace duckdb
