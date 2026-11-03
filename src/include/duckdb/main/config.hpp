//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/config.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/storage_info.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/cgroups.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/enums/thread_pin_mode.hpp"
#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/storage/compression/bitpacking.hpp"
#include "duckdb/function/encoding_function.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/logging/logging.hpp"
#include "duckdb/main/user_settings.hpp"
#include "duckdb/common/types/type_manager.hpp"
#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/common/enums/debug_verification_mode.hpp"
#include "duckdb/common/enums/debug_order_verification.hpp"

namespace duckdb {
class ExternalExtensionProvider;
class ArrowTypeExtension;
struct ArrowExtensionMetadata;
struct ArrowTypeExtensionSet;

class BlockAllocator;
class BufferManager;
class BufferPool;
class CastFunctionSet;
class CollationBinding;
class ClientContext;
class DuckDB;

//! An extension linked into the binary, and how to load it into a database.
class ErrorManager;
class CompressionFunction;
class TableFunctionRef;
class OperatorExtension;
class StorageExtension;
class ExtensionCallback;
class SecretManager;
class CompressionInfo;
class EncryptionUtil;
class HTTPTransportManager;
class HTTPUtil;
class DatabaseFilePathManager;
class ExtensionCallbackManager;
class TypeManager;

struct CompressionFunctionSet;
struct DatabaseCacheEntry;
struct DBConfig;
struct SettingLookupResult;

//! NOTE: DBConfigOptions is mostly deprecated.
//! If you want to add a setting that can be set by the user, add it as a generic setting to `settings.json`.
//! See e.g. "http_proxy" (HTTPProxySetting) as an example
struct DBConfigOptions {
	//! Database file path. May be empty for in-memory mode
	string database_path;
	//! Database type. If empty, automatically extracted from `database_path`, where a `type:path` syntax is expected
	string database_type;
	//! Access mode of the database (AUTOMATIC, READ_ONLY or READ_WRITE)
	AccessMode access_mode = AccessMode::AUTOMATIC;
	//! ATTACH-style options applied to the main database, e.g. IO_MODE. Same keys ATTACH accepts
	unordered_map<string, Value> main_database_options;
	//! Checkpoint when WAL reaches this size (default: 16MiB)
	idx_t checkpoint_wal_size = 1 << 24;
	//! Whether extensions should be loaded on start-up
	bool load_extensions = true;
	//! Where automatic installs go when neither autoinstall_extension_repository nor custom_extension_repository is
	//! set; empty means the core repository. The local_extension_repository capability sets it to its build's
	//! repository
	string default_autoinstall_repository;
	//! The maximum memory used by the database system (in bytes). Default: 80% of System available memory
	idx_t maximum_memory = DConstants::INVALID_INDEX;
	//! Initial permission to allocate from the fixed-size block pool.
#ifdef __MINGW32__
	bool enable_block_allocator = false;
#else
	bool enable_block_allocator = true;
#endif
	//! The maximum size of the 'temp_directory' folder when set (in bytes). Default: 90% of available disk space.
	idx_t maximum_swap_space = DConstants::INVALID_INDEX;
	//! The maximum amount of CPU threads used by the database system. Default: all available.
	idx_t maximum_threads = DConstants::INVALID_INDEX;
	//! The maximum amount of async threads used by the database system. Default: all available.
	idx_t async_threads = DConstants::INVALID_INDEX;
	//! HTTP client limit derived from thread counts unless configured
	idx_t http_client_pool_capacity = DConstants::INVALID_INDEX;
	//! Whether or not to create and use a temporary directory to store intermediates that do not fit in memory
	bool use_temporary_directory = true;
	//! Directory to store temporary structures that do not fit in memory
	string temporary_directory;
	//! Whether or not to invoke filesystem trim on free blocks after checkpoint. This will reclaim
	//! space for sparse files, on platforms that support it.
	bool trim_free_blocks = false;
	//! Record timestamps of buffer manager unpin() events. Usable by custom eviction policies.
	bool buffer_manager_track_eviction_timestamps = false;
	//! Force checkpoint when CHECKPOINT is called or on shutdown, even if no changes have been made
	bool force_checkpoint = false;
	//! Run a checkpoint on successful shutdown and delete the WAL, to leave only a single database file behind
	bool checkpoint_on_shutdown = true;
	//! Serialize the metadata on checkpoint with compatibility for a given DuckDB version.
	StorageCompatibility storage_compatibility = StorageCompatibility::Default();
	//! Initialize the database with the standard set of DuckDB functions
	//! You should probably not touch this unless you know what you are doing
	bool initialize_default_database = true;
	//! Enable mbedtls explicitly (overrides OpenSSL if available)
	bool force_mbedtls = false;
	//! The set of disabled optimizers (default empty)
	set<OptimizerType> disabled_optimizers;
	//! Force a specific schema for VARIANT shredding
	LogicalType force_variant_shredding = LogicalType::INVALID;
	//! Database configuration variable default values;
	case_insensitive_map_t<Value> set_variable_defaults;
	//! Additional directories to store extension binaries in
	vector<string> extension_directories;
	//! Debug setting - how to initialize  blocks in the storage layer when allocating
	DebugInitialize debug_initialize = DebugInitialize::NO_INITIALIZE;
	//! Debug setting - how to verify ORDER BY results (e.g. by rewriting ORDER BY into create_sort_key)
	DebugOrderVerification debug_order_verification = DebugOrderVerification::NONE;
	//! The set of user-provided options
	identifier_map_t<Value> user_options;
	//! The set of unrecognized (other) options
	identifier_map_t<Value> unrecognized_options;
	//! If bulk deallocation larger than this occurs, flush outstanding allocations (1 << 30, ~1GB)
	idx_t allocator_bulk_deallocation_flush_threshold = 536870912ULL;
	//! Delta Only! - Fall back to recognizing Variant columns structurally
	bool variant_legacy_encoding = false;
	//! Metadata from DuckDB callers
	string custom_user_agent;
	//! HTTP proxy host (defaults to the HTTP_PROXY environment variable when unset)
	string http_proxy;
	//! The default block header size for new duckdb database files.
	idx_t default_block_header_size = DEFAULT_BLOCK_HEADER_STORAGE_SIZE;
	//!  Whether or not to abort if a serialization exception is thrown during WAL playback (when reading truncated WAL)
	bool abort_on_wal_failure = false;
	//! Paths that are explicitly allowed, even if enable_external_access is false
	unordered_set<string> allowed_paths;
	//! Environment variables the embedder configures itself from, readable with OSUtil::GetEnvUnrestricted
	unordered_set<string> configuration_env;
	//! Directories that are explicitly allowed, even if enable_external_access is false
	set<string> allowed_directories;
	//! Additional configuration options that are allowed to be changed even when the configuration is locked
	identifier_set_t allowed_configs;
	//! The log configuration
	LogConfig log_config = LogConfig();
	//! Memory limit for the write buffer per row group (optional)
	optional_idx write_buffer_row_group_memory_limit;
	//! Whether to print bindings when printing the plan (debug mode only)
	static bool debug_print_bindings; // NOLINT: debug setting
	//! The global verification mode
	static DebugVerificationMode global_verification_mode; // NOLINT: debug setting

	bool operator==(const DBConfigOptions &other) const;
};

//! The replacement scans of a database. Scans can be registered while queries are being bound, so the list is
//! guarded by a lock and the binder iterates over a snapshot.
class ReplacementScanList {
public:
	ReplacementScanList() = default;
	ReplacementScanList(ReplacementScanList &&other) noexcept {
		lock_guard<mutex> guard(other.lock);
		scans = std::move(other.scans);
	}
	ReplacementScanList &operator=(ReplacementScanList &&other) noexcept {
		if (this != &other) {
			lock_guard<mutex> other_guard(other.lock);
			lock_guard<mutex> guard(lock);
			scans = std::move(other.scans);
		}
		return *this;
	}

	template <class... ARGS>
	void emplace_back(ARGS &&... args) { // NOLINT
		auto scan = make_shared_ptr<ReplacementScan>(std::forward<ARGS>(args)...);
		lock_guard<mutex> guard(lock);
		scans.push_back(std::move(scan));
	}
	void push_back(ReplacementScan scan) { // NOLINT
		auto shared_scan = make_shared_ptr<ReplacementScan>(std::move(scan));
		lock_guard<mutex> guard(lock);
		scans.push_back(std::move(shared_scan));
	}
	vector<shared_ptr<ReplacementScan>> Snapshot() const {
		lock_guard<mutex> guard(lock);
		return scans;
	}

private:
	mutable mutex lock;
	vector<shared_ptr<ReplacementScan>> scans;
};

struct DBConfig {
	friend class DatabaseInstance;
	friend class StorageManager;

public:
	DUCKDB_API DBConfig();
	explicit DUCKDB_API DBConfig(bool read_only);
	DUCKDB_API DBConfig(const identifier_map_t<Value> &config_dict, bool read_only);
	DUCKDB_API ~DBConfig();

	//! Replacement table scans are automatically attempted when a table name cannot be found in the schema
	ReplacementScanList replacement_scans;

	//! The FileSystem to use, can be overwritten to allow for injecting custom file systems for testing purposes (e.g.
	//! RamFS or something similar)
	unique_ptr<FileSystem> file_system;
	//! Secret manager
	unique_ptr<SecretManager> secret_manager;
	//! The allocator used by the system
	unique_ptr<Allocator> allocator;
	//! The block allocator used by the system
	unique_ptr<BlockAllocator> block_allocator;
	//! Database configuration options
	DBConfigOptions options;
	//! Error manager
	unique_ptr<ErrorManager> error_manager;
	//! A reference to the (shared) default allocator (Allocator::DefaultAllocator)
	shared_ptr<Allocator> default_allocator;
	//! A buffer pool can be shared across multiple databases (if desired).
	shared_ptr<BufferPool> buffer_pool;
	//! Provide a custom buffer manager implementation (if desired).
	shared_ptr<BufferManager> buffer_manager;
	//! Encryption Util for OpenSSL and MbedTLS
	shared_ptr<EncryptionUtil> encryption_util;
	//! Reference to the database cache entry (if any)
	shared_ptr<DatabaseCacheEntry> db_cache_entry;
	//! Reference to the database file path manager
	shared_ptr<DatabaseFilePathManager> path_manager;
	//! Database configuration variables as controlled by SET
	GlobalUserSettings user_settings;

public:
	DUCKDB_API static DBConfig &GetConfig(ClientContext &context);
	DUCKDB_API static DBConfig &GetConfig(DatabaseInstance &db);
	DUCKDB_API static DBConfig &Get(AttachedDatabase &db);
	DUCKDB_API static const DBConfig &GetConfig(const ClientContext &context);
	DUCKDB_API static const DBConfig &GetConfig(const DatabaseInstance &db);
	DUCKDB_API static vector<ConfigurationOption> GetOptions();
	//! The amount of unflushed row group data after which the optimistic writer flushes (reads under the config lock)
	DUCKDB_API idx_t GetWriteBufferMemoryLimit() const;
	//! A copy of the configured extension directories (read under the config lock)
	DUCKDB_API vector<string> GetExtensionDirectories() const;
	DUCKDB_API static vector<ConfigurationAlias> GetAliases();
	DUCKDB_API static idx_t GetOptionCount();
	DUCKDB_API static idx_t GetAliasCount();
	DUCKDB_API static vector<Identifier> GetOptionNames();
	DUCKDB_API static bool IsInMemoryDatabase(const char *database_path);

	DUCKDB_API void AddExtensionOption(const Identifier &name, string description, LogicalType parameter,
	                                   const Value &default_value = Value(), set_option_callback_t function = nullptr,
	                                   SetScope default_scope = SetScope::SESSION, bool is_debug = false,
	                                   bool is_deprecated = false);
	DUCKDB_API void AddExtensionOption(const Identifier &name, ExtensionOption extension_option);
	DUCKDB_API bool HasExtensionOption(const Identifier &name) const;
	DUCKDB_API identifier_map_t<ExtensionOption> GetExtensionSettings() const;
	DUCKDB_API bool TryGetExtensionOption(const Identifier &name, ExtensionOption &result) const;
	//! Fetch an option by index. Returns a pointer to the option, or nullptr if out of range
	DUCKDB_API static optional_ptr<const ConfigurationOption> GetOptionByIndex(idx_t index);
	//! Fetcha n alias by index, or nullptr if out of range
	DUCKDB_API static optional_ptr<const ConfigurationAlias> GetAliasByIndex(idx_t index);
	//! Fetch an option by name. Returns a pointer to the option, or nullptr if none exists.
	DUCKDB_API static optional_ptr<const ConfigurationOption> GetOptionByName(const Identifier &name);
	DUCKDB_API void SetOption(const ConfigurationOption &option, const Value &value);
	DUCKDB_API void SetOption(optional_ptr<DatabaseInstance> db, const ConfigurationOption &option, const Value &value);
	DUCKDB_API void SetOption(const Identifier &name, Value value);
	DUCKDB_API void SetOption(idx_t setting_index, Value value);
	DUCKDB_API void SetOptionByName(const Identifier &name, const Value &value);
	DUCKDB_API void SetOptionsByName(const identifier_map_t<Value> &values);
	DUCKDB_API void ResetOption(optional_ptr<DatabaseInstance> db, const ConfigurationOption &option);
	DUCKDB_API void ResetOption(const ExtensionOption &extension_option);
	DUCKDB_API void ResetGenericOption(idx_t setting_index);
	DUCKDB_API optional_idx TryGetSettingIndex(const Identifier &name,
	                                           optional_ptr<const ConfigurationOption> &option) const;
	static LogicalType ParseLogicalType(const string &type);

	DUCKDB_API void CheckLock(const Identifier &name);

	//! Parse a memory size - an unlimited size (e.g. "-1" or "none") is returned as an invalid optional_idx
	DUCKDB_API static optional_idx ParseMemoryLimit(const string &arg);
	//! Parse a memory size, returning NumericLimits<idx_t>::Maximum() for an unlimited size
	DUCKDB_API static idx_t ParseMemoryLimitOrMaximum(const string &arg);

	//! Returns the list of possible compression functions for the physical type.
	DUCKDB_API vector<reference<const CompressionFunction>>
	GetCompressionFunctions(const PhysicalType physical_type) const;
	//! Returns the compression function matching the compression and physical type.
	//! Throws an error if the function does not exist.
	DUCKDB_API reference<const CompressionFunction> GetCompressionFunction(CompressionType type,
	                                                                       const PhysicalType physical_type) const;
	DUCKDB_API optional_ptr<const CompressionFunction>
	TryGetCompressionFunction(CompressionType type, const PhysicalType physical_type) const;
	//! Sets the disabled compression methods
	DUCKDB_API void SetDisabledCompressionMethods(const vector<CompressionType> &disabled_compression_methods);
	//! Returns a list of disabled compression methods
	DUCKDB_API vector<CompressionType> GetDisabledCompressionMethods() const;

	//! Returns the encode function matching the encoding name.
	DUCKDB_API optional_ptr<EncodingFunction> GetEncodeFunction(const string &name) const;
	DUCKDB_API void RegisterEncodeFunction(const EncodingFunction &function) const;
	//! Returns the encode function names.
	DUCKDB_API vector<reference<EncodingFunction>> GetLoadedEncodedFunctions() const;
	//! Returns the encode function matching the encoding name.
	DUCKDB_API ArrowTypeExtension GetArrowExtension(ArrowExtensionMetadata info) const;
	DUCKDB_API ArrowTypeExtension GetArrowExtension(const LogicalType &type) const;
	DUCKDB_API bool HasArrowExtension(const LogicalType &type) const;
	DUCKDB_API bool HasArrowExtension(ArrowExtensionMetadata info) const;
	DUCKDB_API void RegisterArrowExtension(const ArrowTypeExtension &extension) const;

	//! Extensions compiled into the binary that produced this config, published as callables rather
	//! than as generated code. Code carrying its own copy of DuckDB - a statically built extension -
	//! cannot see the generated loader, but it can read this; and copying it into a child config
	//! hands a database the same capability set as the one that created it.
	//! A vector, not a map: extensions load in registration order, and that order has to be stable
	//! across runs and platforms.
	vector<LinkedExtension> linked_extensions;

	bool operator==(const DBConfig &other);
	bool operator!=(const DBConfig &other);

	DUCKDB_API CastFunctionSet &GetCastFunctions();
	DUCKDB_API const CastFunctionSet &GetCastFunctions() const;
	DUCKDB_API TypeManager &GetTypeManager();
	DUCKDB_API CollationBinding &GetCollationBinding();
	DUCKDB_API IndexTypeSet &GetIndexTypes();
	static idx_t GetSystemMaxThreads(FileSystem &fs);
	static idx_t GetSystemMaxAsyncThreads(FileSystem &fs);
	static idx_t GetSystemAvailableMemory(FileSystem &fs);
	static optional_idx ParseMemoryLimitSlurm(const string &arg);
	void SetDefaultMaxMemory();
	void SetDefaultTempDirectory();

	OrderType ResolveOrder(ClientContext &context, OrderType order_type) const;
	OrderByNullType ResolveNullOrder(ClientContext &context, OrderType order_type, OrderByNullType null_type) const;
	const string UserAgent() const;

	//! Returns the value of a setting currently. If the setting is not set by the user, returns the default value.
	SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) const;
	//! Returns the value of a setting set by the user currently
	SettingLookupResult TryGetCurrentUserSetting(idx_t setting_index, Value &result) const;
	//! Returns the default value of an option
	static SettingLookupResult TryGetDefaultValue(optional_ptr<const ConfigurationOption> option, Value &result);

	//! The opener (if any) is used to expand "~" using the connection-specific home_directory
	bool CanAccessFile(const string &path, FileType type, optional_ptr<FileOpener> opener = nullptr);
	void AddAllowedConfig(const Identifier &config_name);
	void AddAllowedDirectory(const string &path);
	void AddAllowedPath(const string &path);
	//! Replace / reset the allowed directories and paths as a whole (under the lock that guards them)
	void SetAllowedDirectories(const vector<string> &paths);
	void SetAllowedPaths(const vector<string> &paths);
	void ResetAllowedDirectories();
	void ResetAllowedPaths();
	//! Whether the given optimizer is disabled (disabled_optimizers)
	DUCKDB_API bool IsOptimizerDisabled(OptimizerType type) const;
	DUCKDB_API set<OptimizerType> GetDisabledOptimizers() const;
	DUCKDB_API void SetDisabledOptimizers(set<OptimizerType> disabled_optimizers);
	//! Allows a database file and its WAL files, so a database can be opened while external access is disabled.
	//! Only possible through API calls, not SQL calls.
	void AddAllowedDatabasePath(const string &database_path);
	vector<string> GetAllowedDirectories() const;
	vector<string> GetAllowedPaths() const;
	string SanitizeAllowedPath(const string &path, optional_ptr<FileOpener> opener = nullptr) const;
	ExtensionCallbackManager &GetCallbackManager();
	const ExtensionCallbackManager &GetCallbackManager() const;

	void SetHTTPUtil(const shared_ptr<HTTPUtil> &new_http_util);
	HTTPUtil &GetHTTPUtil() const;
	//! Replace how external extensions are installed and loaded, before the database runs queries
	DUCKDB_API void SetExternalExtensionProvider(const shared_ptr<ExternalExtensionProvider> &new_provider);
	//! Returns a copy of the forced variant shredding type (INVALID if unset), safe to call while it is being modified
	DUCKDB_API LogicalType GetForceVariantShredding() const;
	//! Returns a copy of the allowed_configs set, safe to call while it is being modified
	DUCKDB_API identifier_set_t GetAllowedConfigs() const;
	DUCKDB_API ExternalExtensionProvider &GetExternalExtensionProvider() const;
	DUCKDB_API HTTPTransportManager &GetHTTPTransportManager();
	DUCKDB_API const HTTPTransportManager &GetHTTPTransportManager() const;

private:
	mutable std::recursive_mutex config_lock;
	//! Guards allowed_paths and allowed_directories, which a running instance can extend while files are being opened
	mutable mutex allowed_paths_lock;
	//! Guards disabled_optimizers, which can be changed while other connections plan queries
	mutable mutex disabled_optimizers_lock;
	//! Whether disabled_optimizers is non-empty
	atomic<bool> has_disabled_optimizers {false};
	unique_ptr<CompressionFunctionSet> compression_functions;
	unique_ptr<EncodingFunctionSet> encoding_functions;
	unique_ptr<ArrowTypeExtensionSet> arrow_extensions;
	unique_ptr<TypeManager> type_manager;
	unique_ptr<CollationBinding> collation_bindings;
	unique_ptr<IndexTypeSet> index_types;
	unique_ptr<ExtensionCallbackManager> callback_manager;
	bool is_user_config = true;
	//! HTTP provider publication and bounded client ownership
	unique_ptr<HTTPTransportManager> http_transport_manager;
	//! Installs and loads external extensions; "none" unless a loader library is linked
	shared_ptr<ExternalExtensionProvider> external_extension_provider;
};

} // namespace duckdb
