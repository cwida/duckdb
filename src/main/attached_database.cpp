#include "duckdb/main/attached_database.hpp"
#include "duckdb/logging/log_manager.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/checkpoint_on_detach.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/main/database_path_and_type.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

StoredDatabasePath::StoredDatabasePath(DatabaseManager &db_manager, DatabaseFilePathManager &manager, string path_p,
                                       const Identifier &name)
    : db_manager(db_manager), manager(manager), path(std::move(path_p)) {
}

StoredDatabasePath::~StoredDatabasePath() {
	manager.EraseDatabasePath(path);
}

void StoredDatabasePath::OnDetach() {
	manager.DetachDatabase(db_manager, path);
}

//===--------------------------------------------------------------------===//
// Attach Options
//===--------------------------------------------------------------------===//
AttachOptions::AttachOptions(const DBConfigOptions &options)
    : access_mode(options.access_mode), db_type(options.database_type) {
}

//! The spellings of the access mode option, and whether `true` means read-only for each.
static const unordered_map<string, bool> ACCESS_MODE_OPTIONS = {
    {"readonly", true}, {"read_only", true}, {"readwrite", false}, {"read_write", false}};

string AttachOptions::OptionSetting(const string &name) {
	auto lower = StringUtil::Lower(name);
	if (ACCESS_MODE_OPTIONS.find(lower) != ACCESS_MODE_OPTIONS.end()) {
		return "access_mode";
	}
	return lower;
}

AttachOptions::AttachOptions(const unordered_map<string, Value> &attach_options, const AccessMode default_access_mode)
    : access_mode(default_access_mode) {
	for (auto &entry : attach_options) {
		auto access_mode_option = ACCESS_MODE_OPTIONS.find(entry.first);
		if (access_mode_option != ACCESS_MODE_OPTIONS.end()) {
			// Extract the access mode; the option reads inverted for the readwrite spellings.
			auto value = BooleanValue::Get(entry.second.DefaultCastAs(LogicalType::BOOLEAN));
			access_mode = value == access_mode_option->second ? AccessMode::READ_ONLY : AccessMode::READ_WRITE;
			continue;
		}

		if (entry.first == "recovery_mode") {
			// Extract the recovery mode.
			auto mode_str = StringValue::Get(entry.second.DefaultCastAs(LogicalType::VARCHAR));
			recovery_mode = EnumUtil::FromString<RecoveryMode>(mode_str);
			continue;
		}

		if (entry.first == "type") {
			// Extract the database type. Normalize case so that
			// `TYPE sqlite` and `TYPE 'SQLite'` are equivalent.
			// `TYPE sqlite` and `TYPE 'sqlite3'` are NOT equivalent, aliasing to be applied on comparison
			db_type = StringUtil::Lower(StringValue::Get(entry.second.DefaultCastAs(LogicalType::VARCHAR)));
			continue;
		}

		if (entry.first == "default_table") {
			default_table = QualifiedName::Parse(StringValue::Get(entry.second.DefaultCastAs(LogicalType::VARCHAR)));
			continue;
		}

		if (entry.first == "hidden") {
			auto is_hidden = BooleanValue::Get(entry.second.DefaultCastAs(LogicalType::BOOLEAN));
			if (is_hidden) {
				visibility = AttachVisibility::HIDDEN;
			}
			continue;
		}

		if (entry.first == "vacuum_rebuild_indexes") {
			const auto threshold = UBigIntValue::Get(entry.second.DefaultCastAs(LogicalType::UBIGINT));
			try {
				vacuum_rebuild_indexes_threshold = threshold;
			} catch (InternalException &e) {
				throw InvalidInputException("Invalid setting for vacuum_rebuild_indexes: %d (valid range is 0 - %d)",
				                            threshold,
				                            UBigIntValue::Get(Value::MaximumValue(LogicalType::UBIGINT)) - 1);
			}
			continue;
		}
		options.emplace(entry.first, entry.second);
	}
}

//===--------------------------------------------------------------------===//
// Attached Database
//===--------------------------------------------------------------------===//
ValidChecker &ValidChecker::Get(AttachedDatabase &db) {
	return db.GetValidChecker();
}

AttachedDatabase::AttachedDatabase(DatabaseInstance &db, AttachedDatabaseType type)
    : CatalogEntry(CatalogType::DATABASE_ENTRY,
                   Identifier(type == AttachedDatabaseType::SYSTEM_DATABASE ? SYSTEM_CATALOG : TEMP_CATALOG), 0),
      db(db), validity(db), type(type), close_lock(make_shared_ptr<mutex>()) {
	// This database does not have storage, or uses temporary_objects for in-memory storage.
	D_ASSERT(type == AttachedDatabaseType::TEMP_DATABASE || type == AttachedDatabaseType::SYSTEM_DATABASE);
	if (type == AttachedDatabaseType::TEMP_DATABASE) {
		unordered_map<string, Value> options;
		AttachOptions attach_options(options, AccessMode::READ_WRITE);
		options["storage_version"] = "latest";
		storage = make_uniq<SingleFileStorageManager>(*this, string(IN_MEMORY_PATH), attach_options);
	}

	catalog = make_uniq<DuckCatalog>(*this);
	transaction_manager = make_uniq<DuckTransactionManager>(*this);
	internal = true;
}

AttachedDatabase::AttachedDatabase(DatabaseInstance &db, Catalog &catalog_p, Identifier name_p, string file_path_p,
                                   AttachOptions &options)
    : CatalogEntry(CatalogType::DATABASE_ENTRY, catalog_p, std::move(name_p)), db(db), validity(db),
      parent_catalog(&catalog_p), close_lock(make_shared_ptr<mutex>()) {
	if (options.access_mode == AccessMode::READ_ONLY) {
		type = AttachedDatabaseType::READ_ONLY_DATABASE;
	} else {
		type = AttachedDatabaseType::READ_WRITE_DATABASE;
	}
	recovery_mode = options.recovery_mode;
	visibility = options.visibility;
	ephemeral = options.ephemeral;
	vacuum_rebuild_threshold = options.vacuum_rebuild_indexes_threshold;
	original_path = options.original_path;
	deleter_function = options.deleter_function;
	deleter_payload = options.deleter_payload;
	deleter_resource_type = options.deleter_resource_type;
	deleter_resource_name = options.deleter_resource_name;
	borrowed_resource_name = options.borrowed_resource_name;

	// We create the storage after the catalog to guarantee we allow extensions to instantiate the DuckCatalog.
	catalog = make_uniq<DuckCatalog>(*this);
	stored_database_path = std::move(options.stored_database_path);
	storage = make_uniq<SingleFileStorageManager>(*this, std::move(file_path_p), options);
	transaction_manager = make_uniq<DuckTransactionManager>(*this);
	attach_options = options.options;
	internal = true;
}

AttachedDatabase::AttachedDatabase(DatabaseInstance &db, Catalog &catalog_p, StorageExtension &storage_extension_p,
                                   ClientContext &context, Identifier name_p, AttachInfo &info, AttachOptions &options)
    : CatalogEntry(CatalogType::DATABASE_ENTRY, catalog_p, std::move(name_p)), db(db), validity(db),
      parent_catalog(&catalog_p), storage_extension(&storage_extension_p), close_lock(make_shared_ptr<mutex>()) {
	if (options.access_mode == AccessMode::READ_ONLY) {
		type = AttachedDatabaseType::READ_ONLY_DATABASE;
	} else {
		type = AttachedDatabaseType::READ_WRITE_DATABASE;
	}
	recovery_mode = options.recovery_mode;
	visibility = options.visibility;
	ephemeral = options.ephemeral;
	vacuum_rebuild_threshold = options.vacuum_rebuild_indexes_threshold;
	original_path = options.original_path;
	deleter_function = options.deleter_function;
	deleter_payload = options.deleter_payload;
	deleter_resource_type = options.deleter_resource_type;
	deleter_resource_name = options.deleter_resource_name;
	borrowed_resource_name = options.borrowed_resource_name;

	optional_ptr<StorageExtensionInfo> storage_info = storage_extension->storage_info.get();
	catalog = storage_extension->attach(storage_info, context, *this, name.GetIdentifierName(), info, options);
	stored_database_path = std::move(options.stored_database_path);
	if (!catalog) {
		throw InternalException("AttachedDatabase - attach function did not return a catalog");
	}
	if (catalog->IsDuckCatalog()) {
		// The attached database uses the DuckCatalog.
		storage = make_uniq<SingleFileStorageManager>(*this, info.path, options);
	}
	transaction_manager = storage_extension->create_transaction_manager(storage_info, *this, *catalog);
	if (!transaction_manager) {
		throw InternalException(
		    "AttachedDatabase - create_transaction_manager function did not return a transaction manager");
	}
	attach_options = options.options;
	internal = true;
}

AttachedDatabase::~AttachedDatabase() {
	// FIXME: Theoretically, we should catch all places higher up the call stack where the
	// FIXME: shared pointer goes out of scope and we need to invoke a checkpoint.
	// FIXME: However, for now, we keep this to avoid unintentional behavior / oversights.
	Close(DatabaseCloseAction::TRY_CHECKPOINT);
}

bool AttachedDatabase::IsSystem() const {
	D_ASSERT(!storage || type != AttachedDatabaseType::SYSTEM_DATABASE);
	return type == AttachedDatabaseType::SYSTEM_DATABASE;
}

bool AttachedDatabase::IsTemporary() const {
	return type == AttachedDatabaseType::TEMP_DATABASE;
}
bool AttachedDatabase::IsReadOnly() const {
	return type == AttachedDatabaseType::READ_ONLY_DATABASE;
}

bool AttachedDatabase::NameIsReserved(const Identifier &name) {
	return name == DEFAULT_SCHEMA || name == TEMP_CATALOG || name == SYSTEM_CATALOG;
}

idx_t AttachedDatabase::GetVacuumRebuildIndexThreshold() const {
	if (vacuum_rebuild_threshold.IsValid()) {
		return vacuum_rebuild_threshold.GetIndex();
	}
	return Settings::Get<VacuumRebuildIndexesSetting>(db);
}

shared_ptr<CompiledGrammar> AttachedDatabase::GetConnectedGrammar(const ClientContext &context) {
	if (connected_grammar) {
		return connected_grammar;
	}
	return db.GetParserCache().GetPassthroughMatcher(context);
}

string AttachedDatabase::StoredPath() const {
	if (stored_database_path) {
		return stored_database_path->path;
	}
	return string();
}

static string RemoveQueryParams(const string &name) {
	auto vec = StringUtil::Split(name, "?");
	D_ASSERT(!vec.empty());
	return vec[0];
}

Identifier AttachedDatabase::ExtractDatabaseName(const string &dbpath, FileSystem &fs) {
	if (dbpath.empty() || dbpath == IN_MEMORY_PATH) {
		return Identifier("memory");
	}
	auto name = RemoveQueryParams(fs.ExtractBaseName(dbpath));
	if (NameIsReserved(Identifier(name))) {
		name += "_db";
	}
	return Identifier(name);
}

void AttachedDatabase::InvokeCloseIfLastReference(shared_ptr<AttachedDatabase> &attached_db, ClientContext &context) {
	auto close_action = DatabaseCloseAction::CHECKPOINT;
	auto checkpoint_on_detach = Settings::Get<CheckpointOnDetachSetting>(context);
	if (checkpoint_on_detach == CheckpointOnDetach::ENABLED) {
		close_action = DatabaseCloseAction::CHECKPOINT;
	} else if (checkpoint_on_detach == CheckpointOnDetach::DISABLED) {
		close_action = DatabaseCloseAction::SKIP_CHECKPOINT;
	} else if (!DBConfig::GetConfig(context).options.checkpoint_on_shutdown) {
		close_action = DatabaseCloseAction::SKIP_CHECKPOINT;
	}

	auto close_lock = attached_db->close_lock;
	{
		lock_guard<mutex> guard(*close_lock);
		if (attached_db.use_count() != 1) {
			attached_db.reset();
			return;
		}
		attached_db->is_closing = true;
	}
	attached_db->Close(close_action);
	attached_db.reset();
}

shared_ptr<AttachedDatabase> AttachedDatabase::TryGetReference(const weak_ptr<AttachedDatabase> &attached_db) {
	auto result = attached_db.lock();
	if (!result) {
		return nullptr;
	}
	auto close_lock = result->close_lock;
	lock_guard<mutex> guard(*close_lock);
	if (result->is_closing) {
		result.reset();
	}
	return result;
}

void AttachedDatabase::Initialize(optional_ptr<ClientContext> context) {
	if (IsSystem()) {
		catalog->Initialize(context, true);
	} else {
		catalog->Initialize(context, false);
	}
	if (storage) {
		storage->Initialize(context);
	}
}

void AttachedDatabase::FinalizeLoad(optional_ptr<ClientContext> context) {
	catalog->FinalizeLoad(context);
}

bool AttachedDatabase::HasStorageManager() const {
	return storage.get();
}

StorageManager &AttachedDatabase::GetStorageManager() {
	if (!storage) {
		throw InternalException("Internal system catalog does not have storage");
	}
	return *storage;
}

const StorageManager &AttachedDatabase::GetStorageManager() const {
	if (!storage) {
		throw InternalException("Internal system catalog does not have storage");
	}
	return *storage;
}

Catalog &AttachedDatabase::GetCatalog() {
	return *catalog;
}

TransactionManager &AttachedDatabase::GetTransactionManager() {
	return *transaction_manager;
}

Catalog &AttachedDatabase::ParentCatalog() {
	return *parent_catalog;
}

const Catalog &AttachedDatabase::ParentCatalog() const {
	return *parent_catalog;
}

bool AttachedDatabase::IsInitialDatabase() const {
	return is_initial_database;
}

void AttachedDatabase::Invalidate(const string &reason) {
	string recovery = HasStorageManager() && GetStorageManager().InMemory()
	                      ? "It is an in-memory database, so its data cannot be recovered."
	                      : "Detach and reattach it before using it again.";
	ValidChecker::Invalidate(*this, StringUtil::Format("Database %s has been invalidated because checkpointing "
	                                                   "failed. %s Original error: %s",
	                                                   GetName(), recovery, reason));
}

void AttachedDatabase::SetInitialDatabase() {
	is_initial_database = true;
}

void AttachedDatabase::SetReadOnlyDatabase() {
	type = AttachedDatabaseType::READ_ONLY_DATABASE;
}

void AttachedDatabase::OnDetach(ClientContext &context) {
	if (catalog) {
		catalog->OnDetach(context);
	}
	if (stored_database_path && visibility != AttachVisibility::HIDDEN) {
		stored_database_path->OnDetach();
	}
}

unique_ptr<ResourceDeleter> AttachedDatabase::ExtractDeleter() {
	if (deleter_function.empty()) {
		return nullptr;
	}
	auto result = make_uniq<ResourceDeleter>(db, std::move(deleter_function), std::move(deleter_payload),
	                                         std::move(deleter_resource_type), std::move(deleter_resource_name));
	deleter_function.clear();
	return result;
}

void AttachedDatabase::Close(const DatabaseCloseAction action) {
	if (is_closed) {
		return;
	}
	D_ASSERT(catalog);
	is_closed = true;

	try {
		auto create_checkpoint = true;
		if (action == DatabaseCloseAction::TRY_CHECKPOINT && Exception::UncaughtException()) {
			create_checkpoint = false;
		} else if (!storage || storage->InMemory() || ValidChecker::IsInvalidated(db) ||
		           ValidChecker::IsInvalidated(*this)) {
			create_checkpoint = false;
		}

		if (create_checkpoint) {
			auto should_checkpoint = false;
			if (action == DatabaseCloseAction::CHECKPOINT) {
				should_checkpoint = true;
			} else if (action == DatabaseCloseAction::TRY_CHECKPOINT) {
				auto &config = DBConfig::GetConfig(db);
				should_checkpoint = config.options.checkpoint_on_shutdown;
			}
			if (should_checkpoint) {
				CheckpointOptions options;
				options.wal_action = CheckpointWALAction::DELETE_WAL;
				storage->CreateCheckpoint(QueryContext(), options);
			}
		}
	} catch (std::exception &ex) {
		ErrorData data(ex);
		if (action == DatabaseCloseAction::TRY_CHECKPOINT) {
			try {
				DUCKDB_LOG_ERROR(db, "Silent exception in AttachedDatabase::Close():\t" + data.Message());
			} catch (...) { // NOLINT
			}
		} else {
			Cleanup();
			data.Throw("Detached database '" + name + "', but CHECKPOINT during DETACH failed. \n");
		}
	} catch (...) { // NOLINT
	}

	Cleanup();
}

void AttachedDatabase::Cleanup() {
	transaction_manager.reset();
	catalog.reset();
	storage.reset();
	stored_database_path.reset();
}

} // namespace duckdb
