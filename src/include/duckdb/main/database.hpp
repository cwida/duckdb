//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/main/capi/extension_api.hpp"
#include "duckdb/main/capi_v2/extension_load_v2.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb_static_extension.h"

namespace duckdb {
class LocalDatabaseFileSystem;

class BufferManager;
class DatabaseManager;
class ExternalResourceTypeRegistry;
class ExternalResourcesManager;
class StorageManager;
class Catalog;
class TransactionManager;
class ConnectionManager;
class ExtensionManager;
class FileSystem;
class TaskScheduler;
class ObjectCache;
struct AttachInfo;
struct AttachOptions;
class DatabaseFileSystem;
struct DatabaseCacheEntry;
class LogManager;
class OSUtil;
class MetricsManager;
class ExternalFileCache;
class ResultSetManager;
struct ParserCache;

class DatabaseInstance : public enable_shared_from_this<DatabaseInstance> {
	friend class DuckDB;

public:
	DUCKDB_API DatabaseInstance();
	DUCKDB_API ~DatabaseInstance();

	DBConfig config;

public:
	BufferPool &GetBufferPool() const;
	DUCKDB_API SecretManager &GetSecretManager();
	DUCKDB_API BufferManager &GetBufferManager();
	DUCKDB_API const BufferManager &GetBufferManager() const;
	DUCKDB_API DatabaseManager &GetDatabaseManager();
	DUCKDB_API ExternalResourceTypeRegistry &GetExternalResourceTypeRegistry();
	DUCKDB_API ExternalResourcesManager &GetExternalResourcesManager();
	DUCKDB_API FileSystem &GetFileSystem();
	DUCKDB_API FileSystem &GetLocalFileSystem();
	DUCKDB_API ExternalFileCache &GetExternalFileCache();
	DUCKDB_API ResultSetManager &GetResultSetManager();
	DUCKDB_API TaskScheduler &GetScheduler();
	DUCKDB_API ObjectCache &GetObjectCache();
	DUCKDB_API ConnectionManager &GetConnectionManager();
	DUCKDB_API ExtensionManager &GetExtensionManager();
	DUCKDB_API ValidChecker &GetValidChecker();
	DUCKDB_API LogManager &GetLogManager() const;
	DUCKDB_API OSUtil &GetOSUtil();
	DUCKDB_API MetricsManager &GetMetricsManager();
	DUCKDB_API ParserCache &GetParserCache();

	DUCKDB_API const duckdb_ext_api_v1 GetExtensionAPIV1();
	//! Runs a V2 C API extension entrypoint, see invoke_capi_v2
	DUCKDB_API void InvokeExtensionEntrypointV2(const ExtensionInitResult &init_result, const string &extension_name,
	                                            ext_init_c_api_v2_fun_t init_fun, optional_ptr<ClientContext> context,
	                                            bool statically_linked);

	idx_t NumberOfThreads();

	DUCKDB_API static DatabaseInstance &GetDatabase(ClientContext &context);
	DUCKDB_API static const DatabaseInstance &GetDatabase(const ClientContext &context);

	DUCKDB_API bool ExtensionIsLoaded(const string &name);

	DUCKDB_API SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) const;

	DUCKDB_API shared_ptr<EncryptionUtil> GetEncryptionUtil(bool read_only = false);
	shared_ptr<EncryptionUtil> GetMbedTLSUtil(bool force_mbedtls) const;

	shared_ptr<AttachedDatabase> CreateAttachedDatabase(ClientContext &context, AttachInfo &info,
	                                                    AttachOptions &options);

private:
	//! Initializes the instance and attaches the main database at `path` (in-memory when null).
	void Initialize(const char *path, DBConfig *config);
	//! Initializes the instance without attaching a database: the system catalog is the only catalog until one is
	//! attached (ATTACH, or DatabaseManager::AttachDatabase).
	void InitializeEmpty(DBConfig *config);
	//! The part of initialization shared by both, up to the main database.
	void InitializeInstance(const char *path, DBConfig *config);
	//! Launches the scheduler threads; last, since storage init races on the catalog otherwise.
	void StartScheduler();
	void LoadExtensionSettings();
	void CreateMainDatabase();

	void Configure(DBConfig &config, const char *path);

private:
	shared_ptr<BufferManager> buffer_manager;
	unique_ptr<DatabaseManager> db_manager;
	unique_ptr<ExternalResourceTypeRegistry> external_resource_type_registry;
	unique_ptr<ExternalResourcesManager> external_resources_manager;
	unique_ptr<TaskScheduler> scheduler;
	unique_ptr<ObjectCache> object_cache;
	unique_ptr<ConnectionManager> connection_manager;
	unique_ptr<ExtensionManager> extension_manager;
	ValidChecker db_validity;
	unique_ptr<DatabaseFileSystem> db_file_system;
	unique_ptr<LocalDatabaseFileSystem> local_db_file_system;
	unique_ptr<LogManager> log_manager;
	unique_ptr<MetricsManager> metrics_manager;
	unique_ptr<ExternalFileCache> external_file_cache;
	unique_ptr<ResultSetManager> result_set_manager;
	unique_ptr<ParserCache> parser_cache;
	unique_ptr<OSUtil> os_util;

	duckdb_ext_api_v1 (*create_api_v1)();
	//! Set in Initialize. Loading a V2 C API extension builds the C API function table and opens a connection, both of
	//! which reach the entire engine. Naming InvokeCAPIV2Entrypoint from the extension loader - which every extension
	//! links, and which reaches it through autoloading - would therefore keep all of DuckDB alive in extensions that
	//! link it statically. Only Initialize names it, and nothing that fails to open a database can reach that.
	invoke_ext_capi_v2_fun_t invoke_capi_v2;
};

//! A describe function for an extension class, so that loading it by class goes through the same path as linked
//! extensions.
template <class T>
struct StaticExtensionDescriber {
	static void Entry(ExtensionLoader &loader) {
		T extension;
		extension.Load(loader);
	}
	static int32_t Describe(duckdb_extension_descriptor *descriptor) {
		static const std::string name = T().Name();
		static const std::string version = T().Version();
		descriptor->version = 1;
		descriptor->name = name.c_str();
		descriptor->extension_version = version.c_str();
		descriptor->entry_cpp = reinterpret_cast<void (*)(void)>(&Entry);
		return 0;
	}
};

//! The database object. This object holds the catalog and all the
//! database-specific meta information.
class DuckDB {
public:
	DUCKDB_API explicit DuckDB(const char *path = nullptr, DBConfig *config = nullptr);
	DUCKDB_API explicit DuckDB(const string &path, DBConfig *config = nullptr);
	DUCKDB_API explicit DuckDB(DatabaseInstance &instance);

	DUCKDB_API ~DuckDB();

	//! Creates an instance with no database attached. Databases are attached to it later (ATTACH, or
	//! DatabaseManager::AttachDatabase); until then only the system catalog and each connection's temporary catalog
	//! exist, and statements that need a default database fail.
	DUCKDB_API static shared_ptr<DuckDB> CreateEmpty(DBConfig *config = nullptr);

	//! Reference to the actual database instance
	shared_ptr<DatabaseInstance> instance;

public:
	// Load a statically linked extension by its class, through a describe function generated for it
	template <class T>
	void LoadStaticExtension() {
		LoadStaticExtension(&StaticExtensionDescriber<T>::Describe);
	}
	// Load the statically linked extension a describe function describes into this database
	DUCKDB_API void LoadStaticExtension(duckdb_extension_describe_t describe);

	// Function pointer type for the C++ extension entrypoint, <name>_duckdb_cpp_init
	typedef void (*ext_init_cpp_fun_t)(ExtensionLoader &loader);
	// Function pointer type for the C API extension init function
	typedef bool (*ext_init_c_api_fun_t)(duckdb_extension_info info, duckdb_extension_access *access);
	// Load a statically compiled C API extension by calling its init function directly (no vtable needed)
	DUCKDB_API void LoadStaticCAPIExtension(const string &name, ext_init_c_api_fun_t init_fun);
	// Same, for an extension built against the V2 C API
	DUCKDB_API void LoadStaticCAPIExtensionV2(const string &name, ext_init_c_api_v2_fun_t init_fun);

	DUCKDB_API FileSystem &GetFileSystem();

	DUCKDB_API idx_t NumberOfThreads();
	DUCKDB_API static const char *SourceID();
	DUCKDB_API static const char *LibraryVersion();
	DUCKDB_API static const char *ReleaseCodename();
	DUCKDB_API static idx_t StandardVectorSize();
	DUCKDB_API static string Platform();
	DUCKDB_API bool ExtensionIsLoaded(const string &name);
};

} // namespace duckdb
