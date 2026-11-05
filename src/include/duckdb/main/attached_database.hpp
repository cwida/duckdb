//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/attached_database.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/prefetched_file_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/main/valid_checker.hpp"

namespace duckdb {
struct CompiledGrammar;
class Catalog;
class DatabaseInstance;
class StorageManager;
class TransactionManager;
class StorageExtension;
class DatabaseManager;
class ResourceDeleter;

struct AttachInfo;
struct StoredDatabasePath;

enum class AttachedDatabaseType {
	READ_WRITE_DATABASE,
	READ_ONLY_DATABASE,
	SYSTEM_DATABASE,
	TEMP_DATABASE,
};

enum class AttachVisibility { SHOWN, HIDDEN };

//! DEFAULT is the standard ACID crash recovery mode.
//! NO_WAL_WRITES disables the WAL for the attached database, i.e., disabling the D in ACID.
//! Use this mode with caution, as it disables recovery from crashes for the file.
enum class RecoveryMode : uint8_t { DEFAULT = 0, NO_WAL_WRITES = 1 };

//! CHECKPOINT: Throws if the checkpoint fails.
//! TRY_CHECKPOINT: Does not throw when failing a checkpoint.
//! SKIP_CHECKPOINT: Skips checkpointing entirely.
//! All actions always clean up.
enum class DatabaseCloseAction { CHECKPOINT, TRY_CHECKPOINT, SKIP_CHECKPOINT };

class DatabaseFilePathManager;

struct StoredDatabasePath {
	StoredDatabasePath(DatabaseManager &db_manager, DatabaseFilePathManager &manager, string path,
	                   const Identifier &name);
	~StoredDatabasePath();

	DatabaseManager &db_manager;
	DatabaseFilePathManager &manager;
	string path;

	void OnDetach();
};

//! AttachOptions holds information about a database we plan to attach. These options are generalized, i.e.,
//! they have to apply to any database file type (duckdb, sqlite, etc.).
struct AttachOptions {
	//! Constructor for databases we attach outside of the ATTACH DATABASE statement.
	explicit AttachOptions(const DBConfigOptions &options);
	//! Constructor for databases we attach when using ATTACH DATABASE.
	AttachOptions(const unordered_map<string, Value> &options, const AccessMode default_access_mode);

	//! The setting an attach option controls, lower-cased. All four spellings of the access mode
	//! (readonly, read_only, readwrite, read_write) share one setting, so two of them in the same
	//! statement are a collision rather than two independent options.
	static string OptionSetting(const string &name);

	//! Defaults to the access mode configured in the DBConfig, unless specified otherwise.
	AccessMode access_mode;
	//! The recovery type of the database.
	RecoveryMode recovery_mode = RecoveryMode::DEFAULT;
	//! The file format type. The default type is a duckdb database file, but other file formats are possible.
	string db_type;
	//! The verbatim path before extension-prefix stripping. Unset if not from an ATTACH statement.
	optional<string> original_path;
	//! Set of remaining (key, value) options
	unordered_map<string, Value> options;
	//! (optionally) a catalog can be provided with a default table
	QualifiedName default_table;
	//! Whether this is the main database.
	bool is_main_database = false;
	//! The visibility of the attached database
	AttachVisibility visibility = AttachVisibility::SHOWN;
	//! Whether this attachment is ephemeral: created implicitly by `CONNECT '<uri>'` and detached
	//! again on DISCONNECT. Not settable via SQL; only the connection-string CONNECT path sets it.
	bool ephemeral = false;
	//! The stored database path (in the path manager)
	unique_ptr<StoredDatabasePath> stored_database_path;
	//! Per-database override of vacuum_rebuild_indexes. If not set, the global setting value is used.
	optional_idx vacuum_rebuild_indexes_threshold;
	//! Deleter binding (from ATTACH/CONNECT TO EXTERNAL RESOURCE): on detach, `<deleter_function>(<deleter_payload>)`
	//! runs to tear down the external resource the attachment owns.
	string deleter_function;
	Value deleter_payload;
	//! Type and name of the owned resource, for teardown logging only. The name is the database alias,
	//! or empty where there is none (CONNECT).
	string deleter_resource_type;
	string deleter_resource_name;
	//! Registered resource this attachment BORROWS (from `ATTACH/CONNECT TO EXTERNAL RESOURCE <name>`): it binds
	//! no deleter and does not block DESTROY, it only lets DESTROY report what it invalidated. Empty if none.
	string borrowed_resource_name;
	//! Header prefetched during file-type detection, reused when opening the file. Empty for non-DuckDB files.
	PrefetchedFileData prefetched;
};

//! The AttachedDatabase represents an attached database instance.
class AttachedDatabase : public CatalogEntry, public enable_shared_from_this<AttachedDatabase> {
public:
	//! Create the built-in system database (without storage).
	explicit AttachedDatabase(DatabaseInstance &db, AttachedDatabaseType type = AttachedDatabaseType::SYSTEM_DATABASE);
	//! Create an attached database instance with the specified name and storage.
	AttachedDatabase(DatabaseInstance &db, Catalog &catalog, Identifier name, string file_path, AttachOptions &options);
	//! Create an attached database instance with the specified storage extension.
	AttachedDatabase(DatabaseInstance &db, Catalog &catalog, StorageExtension &ext, ClientContext &context,
	                 Identifier name, AttachInfo &info, AttachOptions &options);
	~AttachedDatabase() override;

	//! Initializes the catalog and storage of the attached database.
	void Initialize(optional_ptr<ClientContext> context = nullptr);
	void FinalizeLoad(optional_ptr<ClientContext> context);
	//! Close the database before shutting it down.
	void Close(const DatabaseCloseAction action);

	Catalog &ParentCatalog() override;
	const Catalog &ParentCatalog() const override;
	bool HasStorageManager() const;
	StorageManager &GetStorageManager();
	const StorageManager &GetStorageManager() const;
	Catalog &GetCatalog();
	TransactionManager &GetTransactionManager();
	DatabaseInstance &GetDatabase() {
		return db;
	}
	ValidChecker &GetValidChecker() {
		return validity;
	}
	void Invalidate(const string &reason);

	optional_ptr<StorageExtension> GetStorageExtension() {
		return storage_extension;
	}

	const Identifier &GetName() const {
		return name;
	}
	void SetName(const Identifier &new_name) {
		name = new_name;
	}
	//! Move the deleter binding out, if any. Call once the attachment is out of the databases map and
	//! held exclusively, and run the deleter only with no lock held -- it executes SQL.
	unique_ptr<ResourceDeleter> ExtractDeleter();
	bool IsSystem() const;
	bool IsTemporary() const;
	bool IsReadOnly() const;
	bool IsInitialDatabase() const;
	void SetInitialDatabase();
	void SetReadOnlyDatabase();
	void OnDetach(ClientContext &context);
	RecoveryMode GetRecoveryMode() const {
		return recovery_mode;
	}
	AttachVisibility GetVisibility() const {
		return visibility;
	}
	//! True for attachments created implicitly by `CONNECT '<uri>'`; DISCONNECT detaches them.
	bool IsEphemeral() const {
		return ephemeral;
	}
	//! The registered external resource this attachment borrows, or empty if it borrows none.
	const string &GetBorrowedResourceName() const {
		return borrowed_resource_name;
	}
	//! vacuum_rebuild_indexes threshold for this attached database.
	//! Falls back to the global VacuumRebuildIndexesSetting if not overridden.
	idx_t GetVacuumRebuildIndexThreshold() const;
	const unordered_map<string, Value> &GetAttachOptions() const {
		return attach_options;
	}
	//! The grammar used for statements issued while a client is CONNECT-ed to this database. Defaults to the
	//! passthrough grammar, which interprets only DISCONNECT and forwards everything else verbatim; a backend that
	//! speaks DuckDB SQL can override it with the grammar it wants those statements parsed by.
	DUCKDB_API shared_ptr<CompiledGrammar> GetConnectedGrammar(const ClientContext &context);
	void SetConnectedGrammar(shared_ptr<CompiledGrammar> grammar) {
		connected_grammar = std::move(grammar);
	}
	string StoredPath() const;
	//! The verbatim ATTACH path before extension-prefix stripping. Unset if not from an ATTACH statement.
	const optional<string> &GetOriginalPath() const {
		return original_path;
	}
	static bool NameIsReserved(const Identifier &name);
	static Identifier ExtractDatabaseName(const string &dbpath, FileSystem &fs);
	// Invoke Close() on an attached database, if its use count is 1.
	// Only call this in places where you know that the (last) shared pointer is about to go out of scope.
	static void InvokeCloseIfLastReference(shared_ptr<AttachedDatabase> &attached_database, ClientContext &context);
	//! Obtain a reference unless closing the database has already started.
	static shared_ptr<AttachedDatabase> TryGetReference(const weak_ptr<AttachedDatabase> &attached_database);

private:
	DatabaseInstance &db;
	ValidChecker validity;
	unique_ptr<StoredDatabasePath> stored_database_path;
	unique_ptr<StorageManager> storage;
	unique_ptr<Catalog> catalog;
	unique_ptr<TransactionManager> transaction_manager;
	AttachedDatabaseType type;
	optional_ptr<Catalog> parent_catalog;
	optional_ptr<StorageExtension> storage_extension;
	RecoveryMode recovery_mode = RecoveryMode::DEFAULT;
	AttachVisibility visibility = AttachVisibility::SHOWN;
	bool ephemeral = false;
	bool is_initial_database = false;
	bool is_closed = false;
	bool is_closing = false;
	shared_ptr<mutex> close_lock;
	optional_idx vacuum_rebuild_threshold;
	unordered_map<string, Value> attach_options;
	optional<string> original_path;
	//! Deleter binding (from ATTACH/CONNECT TO EXTERNAL RESOURCE): moved out via ExtractDeleter on detach.
	string deleter_function;
	Value deleter_payload;
	string deleter_resource_type;
	string deleter_resource_name;
	//! Registered resource this attachment borrows without owning; see AttachOptions.
	string borrowed_resource_name;
	//! Overrides the default passthrough grammar used while CONNECT-ed; see GetConnectedGrammar.
	shared_ptr<CompiledGrammar> connected_grammar;

private:
	//! Clean any (shared) resources held by the database.
	void Cleanup();
};

} // namespace duckdb
