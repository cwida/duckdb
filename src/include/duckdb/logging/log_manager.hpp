//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/log_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/logging/logger.hpp"
#include "duckdb/logging/log_storage.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {
class LogType;

// Holds global logging state
// - Handles configuration changes
// - Creates Loggers with cached configuration
// - Main sink for logs (either by logging directly into this, or by syncing a pre-cached set of log entries)
// - Holds the log storage
class LogManager {
	friend class ThreadSafeLogger;
	friend class ThreadLocalLogger;
	friend class MutableLogger;

public:
	// Note: two step initialization because Logger needs shared pointer to log manager TODO: can we clean up?
	explicit LogManager(DatabaseInstance &db, LogConfig config = LogConfig());
	~LogManager();
	void Initialize();

	DUCKDB_API static LogManager &Get(ClientContext &context);
	unique_ptr<Logger> CreateLogger(const LoggingContext &context, bool thread_safe = true,
	                                bool mutable_settings = false);
	//! Creates a logger from the snapshot without locking, unless the config changed since the snapshot was taken
	unique_ptr<Logger> CreateLogger(const LoggingContext &context, const LogConfigSnapshot &snapshot);
	//! Refreshes the snapshot if the config changed since it was taken, and creates a logger from it
	unique_ptr<Logger> CreateLogger(const LoggingContext &context, LogConfigSnapshot &snapshot);

	RegisteredLoggingContext RegisterLoggingContext(const LoggingContext &context);

	DUCKDB_API bool RegisterLogStorage(const string &name, shared_ptr<LogStorage> &storage);

	//! The global logger can be used when
	DUCKDB_API Logger &GlobalLogger();
	DUCKDB_API shared_ptr<Logger> GlobalLoggerReference();

	//! Flush everything
	DUCKDB_API void Flush();

	//! Get a shared_ptr to the log storage (For example, to scan it)
	DUCKDB_API shared_ptr<LogStorage> GetLogStorage();
	DUCKDB_API bool CanScan(LoggingTargetTable table);

	DUCKDB_API void SetConfig(DatabaseInstance &db, const LogConfig &config);
	//! The config together with its storage, to put back with RestoreConfig when a change fails
	DUCKDB_API pair<LogConfig, shared_ptr<LogStorage>> GetConfigAndStorage();
	//! Puts back a config and its storage as GetConfigAndStorage returned them
	DUCKDB_API void RestoreConfig(const LogConfig &config, shared_ptr<LogStorage> storage);
	DUCKDB_API void SetEnableLogging(bool enable);
	DUCKDB_API void SetLogMode(LogMode mode);
	DUCKDB_API void SetLogLevel(LogLevel level);
	DUCKDB_API void SetEnabledLogTypes(optional_ptr<unordered_set<string>> enabled_log_types);
	DUCKDB_API void SetDisabledLogTypes(optional_ptr<unordered_set<string>> disabled_log_types);
	DUCKDB_API void SetLogStorage(DatabaseInstance &db, const string &storage_name);

	DUCKDB_API void UpdateLogStorageConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config_value);

	DUCKDB_API void SetEnableStructuredLoggers(vector<string> &enabled_logger_types);

	DUCKDB_API void TruncateLogStorage();

	DUCKDB_API LogConfig GetConfig();

	DUCKDB_API void RegisterLogType(unique_ptr<LogType> type);
	DUCKDB_API optional_ptr<const LogType> LookupLogType(const string &type);
	DUCKDB_API void RegisterDefaultLogTypes();

protected:
	RegisteredLoggingContext RegisterLoggingContextInternal(const LoggingContext &context);

	// This is to be called by the Loggers only, it does not verify log_level and log_type
	void WriteLogEntry(timestamp_t, const char *log_type, LogLevel log_level, const char *log_message,
	                   const RegisteredLoggingContext &context);
	// This allows efficiently pushing a cached set of log entries into the log manager
	void FlushCachedLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context);

	//! Switches the log storage and sets the storage of new_config accordingly
	void SetLogStorageInternal(DatabaseInstance &db, LogConfig &new_config, const string &storage_name)
	    DUCKDB_REQUIRES(lock);

	optional_ptr<const LogType> LookupLogTypeInternal(const string &type) DUCKDB_REQUIRES(lock);

	//! The only way to change the config: publishes it to the global logger and to newly created loggers
	void SetConfigInternal(LogConfig new_config) DUCKDB_REQUIRES(lock);
	unique_ptr<Logger> CreateLoggerInternal(const LoggingContext &context,
	                                        const shared_ptr<const LogConfig> &logger_config);

protected:
	annotated_mutex lock;
	//! Shared by the created loggers; replaced only by SetConfigInternal
	shared_ptr<const LogConfig> config DUCKDB_GUARDED_BY(lock);
	//! Incremented whenever config is replaced, invalidating the snapshots of it
	PaddedAtomic<idx_t> config_version {0};

	shared_ptr<Logger> global_logger;
	shared_ptr<LogStorage> log_storage DUCKDB_GUARDED_BY(lock);
	DatabaseInstance &db_instance;

	PaddedAtomic<idx_t> next_registered_logging_context_index {0};

	// Any additional LogStorages registered (by extensions for example)
	case_insensitive_map_t<shared_ptr<LogStorage>> registered_log_storages DUCKDB_GUARDED_BY(lock);
	case_insensitive_map_t<unique_ptr<LogType>> registered_log_types DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
