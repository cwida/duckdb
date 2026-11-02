#include "duckdb/main/database.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/arrow/arrow_type_extension.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/os_util.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/extension/external_extension_provider.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/common/types/type_manager.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/capi/extension_api.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/main/database_path_and_type.hpp"
#include "duckdb/main/db_instance_cache.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/result_set_manager.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/compression/empty_validity.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/standard_buffer_manager.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "mbedtls_wrapper.hpp"

#ifndef DUCKDB_NO_THREADS
#include "duckdb/common/thread.hpp"
#endif

namespace duckdb {

DBConfig::DBConfig() {
	compression_functions = make_uniq<CompressionFunctionSet>();
	encoding_functions = make_uniq<EncodingFunctionSet>();
	encoding_functions->Initialize(*this);
	arrow_extensions = make_uniq<ArrowTypeExtensionSet>();
	arrow_extensions->Initialize(*this);
	type_manager = make_uniq<TypeManager>(*this);
	collation_bindings = make_uniq<CollationBinding>();
	index_types = make_uniq<IndexTypeSet>();
	error_manager = make_uniq<ErrorManager>();
	secret_manager = make_uniq<SecretManager>();
	http_transport_manager = HTTPTransportManager::Create(make_shared_ptr<HTTPUtil>());
	external_extension_provider = make_shared_ptr<ExternalExtensionProvider>();
	callback_manager = make_uniq<ExtensionCallbackManager>();
}

DBConfig::DBConfig(bool read_only) : DBConfig::DBConfig() {
	if (read_only) {
		options.access_mode = AccessMode::READ_ONLY;
	}
}

DBConfig::DBConfig(const identifier_map_t<Value> &config_dict, bool read_only) : DBConfig::DBConfig(read_only) {
	SetOptionsByName(config_dict);
}

DBConfig::~DBConfig() {
}

DatabaseInstance::DatabaseInstance() : db_validity(*this) {
	config.is_user_config = false;
	create_api_v1 = nullptr;
	invoke_capi_v2 = nullptr;
	parser_cache = make_uniq<ParserCache>();
}

ParserCache &DatabaseInstance::GetParserCache() {
	return *parser_cache;
}

DatabaseInstance::~DatabaseInstance() {
	// destroy all attached databases
	if (db_manager) {
		db_manager->ResetDatabases();
	}
	config.http_transport_manager->Close();
	// destroy child elements
	connection_manager.reset();
	object_cache.reset();
	scheduler.reset();
	db_manager.reset();

	// stop the log manager, after this point Logger calls are unsafe.
	log_manager.reset();

	external_file_cache.reset();
	result_set_manager.reset();

	buffer_manager.reset();

	// Flush allocations and disable the background thread.
	config.block_allocator->FlushOnShutdown();
	Allocator::SetBackgroundThreads(false);
	// after all destruction is complete clear the cache entry
	config.db_cache_entry.reset();
}

DatabaseInstance &DatabaseInstance::GetDatabase(ClientContext &context) {
	return *context.db;
}

const DatabaseInstance &DatabaseInstance::GetDatabase(const ClientContext &context) {
	return *context.db;
}

DatabaseManager &DatabaseInstance::GetDatabaseManager() {
	if (!db_manager) {
		throw InternalException("Missing DB manager");
	}
	return *db_manager;
}

ExternalResourceTypeRegistry &DatabaseInstance::GetExternalResourceTypeRegistry() {
	if (!external_resource_type_registry) {
		throw InternalException("Missing external resource type registry");
	}
	return *external_resource_type_registry;
}

ExternalResourcesManager &DatabaseInstance::GetExternalResourcesManager() {
	if (!external_resources_manager) {
		throw InternalException("Missing external resources manager");
	}
	return *external_resources_manager;
}

Catalog &Catalog::GetSystemCatalog(DatabaseInstance &db) {
	return db.GetDatabaseManager().GetSystemCatalog();
}

Catalog &Catalog::GetCatalog(AttachedDatabase &db) {
	return db.GetCatalog();
}

FileSystem &FileSystem::GetFileSystem(DatabaseInstance &db) {
	return db.GetFileSystem();
}

FileSystem &FileSystem::GetLocal(DatabaseInstance &db) {
	return db.GetLocalFileSystem();
}

FileSystem &FileSystem::Get(AttachedDatabase &db) {
	return FileSystem::GetFileSystem(db.GetDatabase());
}

DBConfig &DBConfig::GetConfig(DatabaseInstance &db) {
	return db.config;
}

ClientConfig &ClientConfig::GetConfig(ClientContext &context) {
	return context.config;
}

DBConfig &DBConfig::Get(AttachedDatabase &db) {
	return DBConfig::GetConfig(db.GetDatabase());
}

const DBConfig &DBConfig::GetConfig(const DatabaseInstance &db) {
	return db.config;
}

const ClientConfig &ClientConfig::GetConfig(const ClientContext &context) {
	return context.config;
}

TransactionManager &TransactionManager::Get(AttachedDatabase &db) {
	return db.GetTransactionManager();
}

ConnectionManager &ConnectionManager::Get(DatabaseInstance &db) {
	return db.GetConnectionManager();
}

ConnectionManager &ConnectionManager::Get(ClientContext &context) {
	return ConnectionManager::Get(DatabaseInstance::GetDatabase(context));
}

shared_ptr<AttachedDatabase> DatabaseInstance::CreateAttachedDatabase(ClientContext &context, AttachInfo &info,
                                                                      AttachOptions &options) {
	shared_ptr<AttachedDatabase> attached_database;
	auto &catalog = Catalog::GetSystemCatalog(*this);

	if (!options.db_type.empty()) {
		// Find the storage extension for this database file.
		auto extension_name = ExtensionHelper::ApplyExtensionAlias(options.db_type);
		auto storage_extension = StorageExtension::Find(config, extension_name);
		if (!storage_extension) {
			throw BinderException("Unrecognized storage type \"%s\"", options.db_type);
		}

		if (storage_extension->attach != nullptr && storage_extension->create_transaction_manager != nullptr) {
			// Use the storage extension to create the initial database.
			attached_database = make_shared_ptr<AttachedDatabase>(*this, catalog, *storage_extension, context,
			                                                      info.name, info, options);
			return attached_database;
		}

		attached_database = make_shared_ptr<AttachedDatabase>(*this, catalog, info.name, info.path, options);
		return attached_database;
	}

	// An empty db_type defaults to a duckdb database file.
	attached_database = make_shared_ptr<AttachedDatabase>(*this, catalog, info.name, info.path, options);
	return attached_database;
}

void DatabaseInstance::CreateMainDatabase() {
	AttachInfo info;
	info.name = Identifier(AttachedDatabase::ExtractDatabaseName(config.options.database_path, GetFileSystem()));
	info.path = config.options.database_path;

	Connection con(*this);
	con.BeginTransaction();
	AttachOptions options(config.options);
	options.options = config.options.main_database_options;
	options.is_main_database = true;
	db_manager->AttachDatabase(*con.context, info, options);
	con.Commit();
}

static void ThrowExtensionSetUnrecognizedOptions(const identifier_map_t<Value> &unrecognized_options) {
	D_ASSERT(!unrecognized_options.empty());

	vector<duckdb::Identifier> options;
	for (auto &kv : unrecognized_options) {
		options.push_back(kv.first);
	}
	auto concatenated = StringUtil::Join(options, ", ");
	throw InvalidInputException("The following options were not recognized: " + concatenated);
}

void DatabaseInstance::LoadExtensionSettings() {
	// copy the map, to protect against modifications during
	auto unrecognized_options_copy = config.options.unrecognized_options;

	if (Settings::Get<AutoloadKnownExtensionsSetting>(*this)) {
		if (unrecognized_options_copy.empty()) {
			// Nothing to do
			return;
		}

		Connection con(*this);
		con.BeginTransaction();

		for (auto &option : unrecognized_options_copy) {
			auto &name = option.first;
			auto &value = option.second;

			auto extension_name = ExtensionHelper::FindExtensionInEntries(name, EXTENSION_SETTINGS);
			if (extension_name.empty()) {
				continue;
			}
			if (!ExtensionHelper::TryAutoLoadExtension(*this, extension_name)) {
				throw InvalidInputException(
				    "To set the %s setting, the %s extension needs to be loaded. But it could not be autoloaded.", name,
				    extension_name);
			}
			ExtensionOption extension_option;
			if (!config.TryGetExtensionOption(name, extension_option)) {
				throw InternalException("Extension %s did not provide the %s config setting", extension_name, name);
			}
			// if the extension provided the option, it should no longer be unrecognized.
			D_ASSERT(config.options.unrecognized_options.find(name) == config.options.unrecognized_options.end());
			auto &context = *con.context;
			PhysicalSet::SetExtensionVariable(context, name, extension_option, SetScope::GLOBAL, value);
		}

		con.Commit();
	}
	if (!config.options.unrecognized_options.empty()) {
		ThrowExtensionSetUnrecognizedOptions(config.options.unrecognized_options);
	}
}

static duckdb_ext_api_v1 CreateAPIv1Wrapper() {
	return CreateAPIv1();
}

void DatabaseInstance::Initialize(const char *database_path, DBConfig *user_config) {
	InitializeInstance(database_path, user_config);
	if (!db_manager->HasAttachedDatabase()) {
		CreateMainDatabase();
	}
	// The main database is the default; a storage extension may have attached it during startup instead.
	optional_ptr<AttachedDatabase> main_database;
	for (auto &attached : db_manager->GetDatabases()) {
		if (attached->IsSystem()) {
			continue;
		}
		if (!main_database || attached->oid < main_database->oid) {
			main_database = attached.get();
		}
	}
	db_manager->SetDefaultDatabase(main_database->GetName());
	StartScheduler();
}

void DatabaseInstance::InitializeEmpty(DBConfig *user_config) {
	InitializeInstance(nullptr, user_config);
	StartScheduler();
}

void DatabaseInstance::StartScheduler() {
	scheduler->SetThreads(config.options.maximum_threads, Settings::Get<ExternalThreadsSetting>(config));
	scheduler->SetAsyncThreads(config.options.async_threads);
	scheduler->RelaunchThreads();
}

void DatabaseInstance::InitializeInstance(const char *database_path, DBConfig *user_config) {
	DBConfig default_config;
	DBConfig *config_ptr = &default_config;
	if (user_config) {
		config_ptr = user_config;
	}

	Configure(*config_ptr, database_path);
	// publish what this binary links, unless the config already carries a set handed to us
	ExtensionHelper::RegisterLinkedExtensions(config);
	for (auto &linked : config.linked_extensions) {
		if (linked.database_callback) {
			linked.database_callback(*this);
		}
	}

	create_api_v1 = CreateAPIv1Wrapper;
	invoke_capi_v2 = InvokeCAPIV2Entrypoint;

	db_file_system = make_uniq<DatabaseFileSystem>(*this);
	local_db_file_system = make_uniq<LocalDatabaseFileSystem>(*this);
	db_manager = make_uniq<DatabaseManager>(*this);
	external_resource_type_registry = make_uniq<ExternalResourceTypeRegistry>();
	external_resources_manager = make_uniq<ExternalResourcesManager>();
	if (config.buffer_manager) {
		buffer_manager = config.buffer_manager;
	} else {
		buffer_manager = make_uniq<StandardBufferManager>(*this, config.options.temporary_directory);
	}

	log_manager = make_uniq<LogManager>(*this, LogConfig());
	log_manager->Initialize();

	metrics_manager = make_uniq<MetricsManager>();

	bool enable_external_file_cache = Settings::Get<EnableExternalFileCacheSetting>(config);
	external_file_cache = make_uniq<ExternalFileCache>(*this, enable_external_file_cache);
	result_set_manager = make_uniq<ResultSetManager>(*this);

	scheduler = make_uniq<TaskScheduler>(*this);
	object_cache = make_uniq<ObjectCache>(*config.buffer_pool);
	config.buffer_pool->SetObjectCache(object_cache.get());
	connection_manager = make_uniq<ConnectionManager>();
	extension_manager = make_uniq<ExtensionManager>(*this);

	// initialize the secret manager
	config.secret_manager->Initialize(*this);

	// initialize the system catalog
	db_manager->InitializeSystemCatalog();

	// resolve the type of the database we are opening
	auto &fs = FileSystem::GetFileSystem(*this);
	DBPathAndType::ResolveDatabaseType(fs, config.options.database_path, config.options.database_type);

	if (!config.options.database_type.empty() && !StringUtil::CIEquals(config.options.database_type, "duckdb")) {
		// if we are opening an extension database - load the extension
		if (!config.file_system) {
			throw InternalException("No file system!?");
		}
		auto storage_extension = StorageExtension::Find(config, config.options.database_type);
		if (!storage_extension) {
			ExtensionHelper::LoadExternalExtension(*this, *config.file_system, {config.options.database_type});
		}
	}

	LoadExtensionSettings();
}

DuckDB::DuckDB(const char *path, DBConfig *new_config) : instance(make_shared_ptr<DatabaseInstance>()) {
	instance->Initialize(path, new_config);
	if (instance->config.options.load_extensions) {
		ExtensionHelper::LoadAllExtensions(*this);
	}
	instance->db_manager->FinalizeStartup();
}

DuckDB::DuckDB(const string &path, DBConfig *config) : DuckDB(path.c_str(), config) {
}

shared_ptr<DuckDB> DuckDB::CreateEmpty(DBConfig *config) {
	auto instance = make_shared_ptr<DatabaseInstance>();
	instance->InitializeEmpty(config);
	auto db = make_shared_ptr<DuckDB>(*instance);
	if (instance->config.options.load_extensions) {
		ExtensionHelper::LoadAllExtensions(*db);
	}
	return db;
}

DuckDB::DuckDB(DatabaseInstance &instance_p) : instance(instance_p.shared_from_this()) {
}

DuckDB::~DuckDB() {
}

SecretManager &DatabaseInstance::GetSecretManager() {
	return *config.secret_manager;
}

BufferManager &DatabaseInstance::GetBufferManager() {
	return *buffer_manager;
}

const BufferManager &DatabaseInstance::GetBufferManager() const {
	return *buffer_manager;
}

BufferPool &DatabaseInstance::GetBufferPool() const {
	return *config.buffer_pool;
}

DatabaseManager &DatabaseManager::Get(DatabaseInstance &db) {
	return db.GetDatabaseManager();
}

DatabaseManager &DatabaseManager::Get(ClientContext &db) {
	return DatabaseManager::Get(*db.db);
}

TaskScheduler &DatabaseInstance::GetScheduler() {
	return *scheduler;
}

ObjectCache &DatabaseInstance::GetObjectCache() {
	return *object_cache;
}

FileSystem &DatabaseInstance::GetFileSystem() {
	return *db_file_system;
}

FileSystem &DatabaseInstance::GetLocalFileSystem() {
	return *local_db_file_system;
}

static FileSystem &ResolveLocalFileSystem(DatabaseInstance &db, unique_ptr<FileSystem> &owned) {
	auto &vfs = static_cast<VirtualFileSystem &>(*db.config.file_system);
	auto &default_fs = vfs.GetDefaultFileSystem();
	if (default_fs.IsLocalFileSystem()) {
		return default_fs;
	}
	owned = make_uniq<LocalFileSystem>();
	return *owned;
}

LocalDatabaseFileSystem::LocalDatabaseFileSystem(DatabaseInstance &db_p)
    : db(db_p), local_fs(ResolveLocalFileSystem(db_p, owned_file_system)), database_opener(db_p) {
}

FileSystem &LocalDatabaseFileSystem::GetFileSystem() const {
	if (db.config.file_system->SubSystemIsDisabled(local_fs.GetName())) {
		throw PermissionException("File system %s has been disabled by configuration", local_fs.GetName());
	}
	return local_fs;
}

ExternalFileCache &DatabaseInstance::GetExternalFileCache() {
	return *external_file_cache;
}

ResultSetManager &DatabaseInstance::GetResultSetManager() {
	return *result_set_manager;
}

ConnectionManager &DatabaseInstance::GetConnectionManager() {
	return *connection_manager;
}

ExtensionManager &DatabaseInstance::GetExtensionManager() {
	return *extension_manager;
}

FileSystem &DuckDB::GetFileSystem() {
	return instance->GetFileSystem();
}

Allocator &Allocator::Get(ClientContext &context) {
	return Allocator::Get(*context.db);
}

Allocator &Allocator::Get(DatabaseInstance &db) {
	return *db.config.allocator;
}

Allocator &Allocator::Get(AttachedDatabase &db) {
	return Allocator::Get(db.GetDatabase());
}

void DatabaseInstance::Configure(DBConfig &new_config, const char *database_path) {
	config.options = new_config.options;
	config.SetDisabledOptimizers(config.options.disabled_optimizers);
	config.user_settings = new_config.user_settings;
	os_util = make_uniq<OSUtil>(*this, config.options.configuration_env);
	// carry over a capability set handed to us, so a database created by code with its own copy of
	// DuckDB can be given the extensions the binary that created it links
	config.linked_extensions = new_config.linked_extensions;

	if (Settings::Get<DuckDBAPISetting>(*this).empty()) {
		config.SetOptionByName("duckdb_api", "cpp");
	}

	if (database_path) {
		config.options.database_path = database_path;
	} else {
		config.options.database_path.clear();
	}

	if (new_config.options.temporary_directory.empty()) {
		config.SetDefaultTempDirectory();
	}

	if (new_config.options.http_proxy.empty()) {
		HTTPProxySetting::ResetGlobal(this, config);
	}

	if (config.options.access_mode == AccessMode::UNDEFINED) {
		config.options.access_mode = AccessMode::READ_WRITE;
	}
	if (new_config.file_system) {
		config.file_system = std::move(new_config.file_system);
	} else {
		config.file_system = make_uniq<VirtualFileSystem>(FileSystem::CreateLocal());
	}
	config.http_transport_manager->Initialize(config);
	if (database_path && !Settings::Get<EnableExternalAccessSetting>(*this)) {
		config.AddAllowedDatabasePath(database_path);
		if (!config.options.temporary_directory.empty()) {
			config.AddAllowedDirectory(config.options.temporary_directory);
		}
	}
	if (new_config.secret_manager) {
		config.secret_manager = std::move(new_config.secret_manager);
	}
	if (config.options.maximum_memory == DConstants::INVALID_INDEX) {
		config.SetDefaultMaxMemory();
	}
	if (new_config.options.maximum_threads == DConstants::INVALID_INDEX) {
		config.options.maximum_threads = config.GetSystemMaxThreads(*config.file_system);
	}
	if (new_config.options.async_threads == DConstants::INVALID_INDEX) {
		config.options.async_threads = config.GetSystemMaxAsyncThreads(*config.file_system);
	}
	config.allocator = std::move(new_config.allocator);
	if (!config.allocator) {
		config.allocator = make_uniq<Allocator>();
	}
	config.block_allocator = std::move(new_config.block_allocator);
	if (!config.block_allocator) {
		auto default_block_size = Settings::Get<DefaultBlockSizeSetting>(config);
		config.block_allocator = make_uniq<BlockAllocator>(*config.allocator, default_block_size,
		                                                   DBConfig::GetSystemAvailableMemory(*config.file_system));
	}
	config.block_allocator->SetEnabled(config.options.enable_block_allocator);
	config.replacement_scans = std::move(new_config.replacement_scans);
	if (new_config.callback_manager) {
		config.callback_manager = std::move(new_config.callback_manager);
		new_config.callback_manager = make_uniq<ExtensionCallbackManager>();
	}
	// This is used to open e.g. parquet files. See DBPathAndType::CheckMagicBytes
	config.callback_manager->Register("__open_file__", OpenFileStorageExtension::Create());

	config.error_manager = std::move(new_config.error_manager);
	if (!config.error_manager) {
		config.error_manager = make_uniq<ErrorManager>();
	}
	if (!config.default_allocator) {
		config.default_allocator = Allocator::DefaultAllocatorReference();
	}
	if (new_config.buffer_pool) {
		config.buffer_pool = std::move(new_config.buffer_pool);
	} else {
		config.buffer_pool = make_shared_ptr<BufferPool>(*config.block_allocator, config.options.maximum_memory,
		                                                 config.options.buffer_manager_track_eviction_timestamps,
		                                                 config.options.allocator_bulk_deallocation_flush_threshold);
	}
	config.db_cache_entry = std::move(new_config.db_cache_entry);
	config.path_manager = std::move(new_config.path_manager);
}

DBConfig &DBConfig::GetConfig(ClientContext &context) {
	return context.db->config;
}

const DBConfig &DBConfig::GetConfig(const ClientContext &context) {
	return context.db->config;
}

idx_t DatabaseInstance::NumberOfThreads() {
	return scheduler->NumberOfThreads();
}

idx_t DuckDB::NumberOfThreads() {
	return instance->NumberOfThreads();
}

bool DatabaseInstance::ExtensionIsLoaded(const string &name) {
	return extension_manager->ExtensionIsLoaded(name);
}

bool DuckDB::ExtensionIsLoaded(const std::string &name) {
	return instance->ExtensionIsLoaded(name);
}

SettingLookupResult DatabaseInstance::TryGetCurrentSetting(const Identifier &key, Value &result) const {
	// check the session values
	auto &db_config = DBConfig::GetConfig(*this);
	return db_config.TryGetCurrentSetting(key, result);
}

shared_ptr<EncryptionUtil> DatabaseInstance::GetMbedTLSUtil(bool force_mbedtls) const {
	auto encryption_util = make_shared_ptr<duckdb_mbedtls::MbedTlsWrapper::AESStateMBEDTLSFactory>();

	if (force_mbedtls) {
		encryption_util->ForceMbedTLSUnsafe();
	}

	return encryption_util;
}

shared_ptr<EncryptionUtil> DatabaseInstance::GetEncryptionUtil(bool read_only) {
	auto force_mbedtls = config.options.force_mbedtls;

	if (force_mbedtls) {
		// return mbedtls if setting is enabled
		return GetMbedTLSUtil(force_mbedtls);
	}

	if (!config.encryption_util) {
		// No encryption_util, attempt to get a hold of httpfs
		if (read_only) {
			// load is attempted, but no install is performed
			ExtensionHelper::TryAutoLoadAvailableExtension(*this, "httpfs");
		} else {
			// load is attempted, otherwise install+load
			ExtensionHelper::TryAutoLoadExtension(*this, "httpfs");
		}
	}

	if (config.encryption_util) {
		// already available (potentially via httpfs loading)
		return config.encryption_util;
	}

	if (read_only) {
		// return mbedtls if database is read only
		// and OpenSSL not set
		return GetMbedTLSUtil(force_mbedtls);
	}

	throw InvalidConfigurationException(" DuckDB currently has a read-only crypto module "
	                                    "loaded. Please ensure httpfs is loaded using `LOAD httpfs`, or for DuckDB "
	                                    "database files consider READONLY mode."
	                                    " To write an encrypted database or parquet file that is NOT securely "
	                                    "encrypted, one can use SET force_mbedtls_unsafe = "
	                                    "'true'.");
}

ValidChecker &DatabaseInstance::GetValidChecker() {
	return db_validity;
}

const duckdb_ext_api_v1 DatabaseInstance::GetExtensionAPIV1() {
	D_ASSERT(create_api_v1);
	return create_api_v1();
}

void DatabaseInstance::InvokeExtensionEntrypointV2(const ExtensionInitResult &init_result, const string &extension_name,
                                                   ext_init_c_api_v2_fun_t init_fun,
                                                   optional_ptr<ClientContext> context, bool statically_linked) {
	D_ASSERT(invoke_capi_v2);
	invoke_capi_v2(*this, init_result, extension_name, init_fun, context, statically_linked);
}

LogManager &DatabaseInstance::GetLogManager() const {
	return *log_manager;
}

OSUtil &DatabaseInstance::GetOSUtil() {
	D_ASSERT(os_util);
	return *os_util;
}

MetricsManager &DatabaseInstance::GetMetricsManager() {
	return *metrics_manager;
}

ValidChecker &ValidChecker::Get(DatabaseInstance &db) {
	return db.GetValidChecker();
}

} // namespace duckdb
