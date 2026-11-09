//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/log_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/logging/logger.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {
class LogType;

// Holds global logging state
// - Handles configuration changes
// - Creates Loggers with cached configuration
// - Main sink for logs (either by logging directly into this, or by syncing a pre-cached set of log entries)
// - Holds the main log sink and the log sinks added next to it, a sink is never both
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
	unique_ptr<Logger> CreateLogger(LoggingContext context, bool thread_safe = true, bool mutable_settings = false);

	RegisteredLoggingContext RegisterLoggingContext(LoggingContext &context);

	DUCKDB_API bool RegisterLogSink(const string &name, shared_ptr<LogSink> &sink);

	//! The global logger can be used when
	DUCKDB_API Logger &GlobalLogger();
	DUCKDB_API shared_ptr<Logger> GlobalLoggerReference();

	//! Flush everything
	DUCKDB_API void Flush();

	//! Get a shared_ptr to the main log sink (For example, to scan it)
	DUCKDB_API shared_ptr<LogSink> GetMainLogSink();
	//! Get the main log sink or an added log sink by name, or the main log sink if the name is empty
	DUCKDB_API shared_ptr<LogSink> GetLogSink(const string &name);
	//! Like GetLogSink, but throws if the sink cannot be scanned for this table
	DUCKDB_API shared_ptr<LogSink> GetScannableLogSink(const string &name, LoggingTargetTable table);
	//! Get the registered log sink with this name, or nullptr if there is none
	DUCKDB_API shared_ptr<LogSink> GetRegisteredLogSink(const string &name);
	//! Whether this sink is the main log sink or an added log sink
	DUCKDB_API bool IsLogSinkEnabled(const string &name);
	DUCKDB_API bool CanScan(LoggingTargetTable table);

	DUCKDB_API void SetConfig(DatabaseInstance &db, const LogConfig &config);
	DUCKDB_API void SetEnableLogging(bool enable);
	DUCKDB_API void SetLogMode(LogMode mode);
	DUCKDB_API void SetLogLevel(LogLevel level);
	DUCKDB_API void SetEnabledLogTypes(optional_ptr<unordered_set<string>> enabled_log_types);
	DUCKDB_API void SetDisabledLogTypes(optional_ptr<unordered_set<string>> disabled_log_types);
	DUCKDB_API void SetMainLogSink(DatabaseInstance &db, const string &sink_name);
	//! Also send the log entries to this sink, and enable logging if it is off
	//! If the sink is already the main or an added sink, only its config is updated
	DUCKDB_API void AddLogSink(DatabaseInstance &db, const string &sink_name,
	                           const case_insensitive_map_t<Value> &sink_config = case_insensitive_map_t<Value>());
	//! Stop sending the log entries to a sink added with AddLogSink
	DUCKDB_API void RemoveLogSink(DatabaseInstance &db, const string &sink_name);

	DUCKDB_API void UpdateMainLogSinkConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config_value);

	DUCKDB_API void SetEnableStructuredLoggers(vector<string> &enabled_logger_types);

	DUCKDB_API void TruncateMainLogSink();

	DUCKDB_API LogConfig GetConfig();

	DUCKDB_API void RegisterLogType(unique_ptr<LogType> type);
	DUCKDB_API optional_ptr<const LogType> LookupLogType(const string &type);
	DUCKDB_API void RegisterDefaultLogTypes();

protected:
	RegisteredLoggingContext RegisterLoggingContextInternal(LoggingContext &context);

	// This is to be called by the Loggers only, it does not verify log_level and log_type
	void WriteLogEntry(timestamp_t, const char *log_type, LogLevel log_level, const char *log_message,
	                   const RegisteredLoggingContext &context);
	// This allows efficiently pushing a cached set of log entries into the log manager
	void FlushCachedLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context);

	void SetMainLogSinkInternal(DatabaseInstance &db, const string &sink_name);
	//! A new built-in sink, or the sink registered under this name. Throws if there is none
	shared_ptr<LogSink> CreateLogSinkInternal(DatabaseInstance &db, const string &sink_name);
	//! The main log sink or the added sink with this name, or nullptr if there is none
	shared_ptr<LogSink> GetEnabledLogSinkInternal(const string &sink_name);
	shared_ptr<LogSink> GetLogSinkInternal(const string &name);

	optional_ptr<const LogType> LookupLogTypeInternal(const string &type);

	void SetConfigInternal(LogConfig config);

protected:
	mutex lock;
	LogConfig config;

	shared_ptr<Logger> global_logger;
	//! The main log sink, it always receives the log entries. Its name is config.storage
	shared_ptr<LogSink> main_log_sink;
	//! The sinks that receive the log entries next to the main log sink, never including the main log sink
	case_insensitive_map_t<shared_ptr<LogSink>> added_log_sinks;
	DatabaseInstance &db_instance;

	idx_t next_registered_logging_context_index = 0;

	// The LogSinks registered with RegisterLogSink, by extensions for example
	case_insensitive_map_t<shared_ptr<LogSink>> registered_log_sinks;
	case_insensitive_map_t<unique_ptr<LogType>> registered_log_types;
};

} // namespace duckdb
