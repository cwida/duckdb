#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/local_file_system.hpp"

namespace duckdb {

struct DeprecatedLogSinkName {
	const char *old_name;
	const char *new_name;
};

static const DeprecatedLogSinkName DEPRECATED_LOG_SINK_NAMES[] = {{"shell_log_storage", "shell_log_sink"}};

//! Maps a deprecated sink name to its new name and warns. Call it without holding the LogManager lock
static string ResolveDeprecatedLogSinkName(DatabaseInstance &db, const string &sink_name) {
	for (auto &entry : DEPRECATED_LOG_SINK_NAMES) {
		if (StringUtil::CIEquals(sink_name, entry.old_name)) {
			DUCKDB_LOG_WARNING(db, "The log sink '%s' has been renamed to '%s', the old name is deprecated",
			                   entry.old_name, entry.new_name);
			return entry.new_name;
		}
	}
	return sink_name;
}

unique_ptr<Logger> LogManager::CreateLogger(LoggingContext context, bool thread_safe, bool mutable_settings) {
	unique_lock<mutex> lck(lock);

	auto registered_logging_context = RegisterLoggingContextInternal(context);

	if (mutable_settings) {
		return make_uniq<MutableLogger>(config, registered_logging_context, *this);
	}
	if (!config.enabled) {
		return make_uniq<NopLogger>(*this);
	}
	if (!thread_safe) {
		// TODO: implement ThreadLocalLogger and return it here
	}
	return make_uniq<ThreadSafeLogger>(config, registered_logging_context, *this);
}

RegisteredLoggingContext LogManager::RegisterLoggingContext(LoggingContext &context) {
	unique_lock<mutex> lck(lock);

	return RegisterLoggingContextInternal(context);
}

bool LogManager::RegisterLogSink(const string &name, shared_ptr<LogSink> &sink) {
	unique_lock<mutex> lck(lock);
	if (registered_log_sinks.find(name) != registered_log_sinks.end()) {
		return false;
	}
	registered_log_sinks.insert({name, std::move(sink)});
	return true;
}

Logger &LogManager::GlobalLogger() {
	return *global_logger;
}

shared_ptr<Logger> LogManager::GlobalLoggerReference() {
	return global_logger;
}

void LogManager::Flush() {
	unique_lock<mutex> lck(lock);
	log_sink->FlushAll();
}

shared_ptr<LogSink> LogManager::GetLogSink() {
	unique_lock<mutex> lck(lock);
	return log_sink;
}

bool LogManager::CanScan(LoggingTargetTable table) {
	unique_lock<mutex> lck(lock);
	return log_sink->CanScan(table);
}

LogManager::LogManager(DatabaseInstance &db, LogConfig config_p) : config(std::move(config_p)), db_instance(db) {
	log_sink = make_uniq<InMemoryLogSink>(db);
}

LogManager::~LogManager() {
}

void LogManager::Initialize() {
	LoggingContext context(LogContextScope::DATABASE);
	global_logger = CreateLogger(context, true, true);

	RegisterDefaultLogTypes();
}

LogManager &LogManager::Get(ClientContext &context) {
	return context.db->GetLogManager();
}

RegisteredLoggingContext LogManager::RegisterLoggingContextInternal(LoggingContext &context) {
	RegisteredLoggingContext result = {next_registered_logging_context_index, context};

	next_registered_logging_context_index += 1;

	if (next_registered_logging_context_index == NumericLimits<idx_t>::Maximum()) {
		throw InternalException("Ran out of available log context ids.");
	}

	return result;
}

void LogManager::WriteLogEntry(timestamp_t timestamp, const char *log_type, LogLevel log_level, const char *log_message,
                               const RegisteredLoggingContext &context) {
	if (log_level == LogLevel::LOG_WARNING && Settings::Get<WarningsAsErrorsSetting>(db_instance)) {
		throw InvalidInputException(log_message);
	} else {
		unique_lock<mutex> lck(lock);
		log_sink->WriteLogEntry(timestamp, log_level, log_type, log_message, context);
	}
}

void LogManager::FlushCachedLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) {
	throw NotImplementedException("FlushCachedLogEntries");
}

void LogManager::SetConfig(DatabaseInstance &db, const LogConfig &config_p) {
	auto sink_name = ResolveDeprecatedLogSinkName(db, config_p.storage);

	unique_lock<mutex> lck(lock);

	// We need extra handling for switching sink
	SetLogSinkInternal(db, sink_name);

	// keep the storage name as SetLogSinkInternal lowercased it, so that it is recognized when set again
	auto new_config = config_p;
	new_config.storage = config.storage;
	SetConfigInternal(std::move(new_config));
}

pair<LogConfig, shared_ptr<LogSink>> LogManager::GetConfigAndSink() {
	unique_lock<mutex> lck(lock);
	return make_pair(config, log_sink);
}

void LogManager::RestoreConfig(const LogConfig &config_p, shared_ptr<LogSink> sink) {
	unique_lock<mutex> lck(lock);
	log_sink = std::move(sink);
	SetConfigInternal(config_p);
}

void LogManager::SetEnableLogging(bool enable) {
	unique_lock<mutex> lck(lock);
	config.enabled = enable;
	global_logger->UpdateConfig(config);
}

void LogManager::SetLogMode(LogMode mode) {
	unique_lock<mutex> lck(lock);
	config.mode = mode;
	global_logger->UpdateConfig(config);
}

void LogManager::SetLogLevel(LogLevel level) {
	unique_lock<mutex> lck(lock);
	config.level = level;
	global_logger->UpdateConfig(config);
}

void LogManager::SetEnabledLogTypes(optional_ptr<unordered_set<string>> enabled_log_types) {
	unique_lock<mutex> lck(lock);
	if (enabled_log_types) {
		config.enabled_log_types = *enabled_log_types;
	} else {
		config.enabled_log_types = {};
	}
	global_logger->UpdateConfig(config);
}

void LogManager::SetDisabledLogTypes(optional_ptr<unordered_set<string>> disabled_log_types) {
	unique_lock<mutex> lck(lock);
	if (disabled_log_types) {
		config.disabled_log_types = *disabled_log_types;
	} else {
		config.disabled_log_types = {};
	}
	global_logger->UpdateConfig(config);
}

void LogManager::SetLogSink(DatabaseInstance &db, const string &requested_sink_name) {
	auto sink_name = ResolveDeprecatedLogSinkName(db, requested_sink_name);

	unique_lock<mutex> lck(lock);
	// 'SET logging_sink' cannot supply the path that file sink requires, so reject the switch
	// here (active sink preserved) and point users at enable_logging instead of installing a
	// path-less sink that throws on every later flush.
	auto sink_name_to_lower = StringUtil::Lower(sink_name);
	if (sink_name_to_lower == LogConfig::FILE_STORAGE_NAME && config.storage != sink_name_to_lower) {
		throw InvalidConfigurationException(
		    "Cannot select 'file' log sink via 'SET logging_sink' because it requires a path. "
		    "Use CALL enable_logging(storage='file', storage_path='...') instead.");
	}
	SetLogSinkInternal(db, sink_name);
}

void LogManager::SetLogSinkInternal(DatabaseInstance &db, const string &sink_name) {
	auto sink_name_to_lower = StringUtil::Lower(sink_name);

	if (config.storage == sink_name_to_lower) {
		return;
	}

	if (sink_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		auto &fs = FileSystem::GetFileSystem(db);
		if (fs.SubSystemIsDisabled(LocalFileSystem().GetName())) {
			throw InvalidConfigurationException("Can not enable file logging with the LocalFileSystem disabled");
		}
	}

	// Flush the old sink, we are going to replace it.
	log_sink->FlushAll();

	if (sink_name_to_lower == LogConfig::IN_MEMORY_STORAGE_NAME) {
		log_sink = make_shared_ptr<InMemoryLogSink>(db);
	} else if (sink_name_to_lower == LogConfig::STDOUT_STORAGE_NAME) {
		log_sink = make_shared_ptr<StdOutLogSink>(db);
	} else if (sink_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		log_sink = make_shared_ptr<FileLogSink>(db);
	} else if (registered_log_sinks.find(sink_name_to_lower) != registered_log_sinks.end()) {
		log_sink = registered_log_sinks[sink_name_to_lower];
	} else {
		throw InvalidInputException("Log sink '%s' is not yet registered", sink_name);
	}
	config.storage = sink_name_to_lower;
}

void LogManager::UpdateLogSinkConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config_value) {
	unique_lock<mutex> lck(lock);
	log_sink->UpdateConfig(db, config_value);
}

void LogManager::SetEnableStructuredLoggers(vector<string> &enabled_logger_types) {
	unique_lock<mutex> lck(lock);

	LogConfig new_config = config;
	new_config.enabled_log_types.clear();

	LogLevel min_log_level = LogLevel::LOG_FATAL;

	for (const auto &enabled_logger_type : enabled_logger_types) {
		auto lookup = LookupLogTypeInternal(enabled_logger_type);
		if (!lookup) {
			throw InvalidInputException("Unknown log type: '%s'", enabled_logger_type);
		}

		new_config.enabled_log_types.insert(lookup->name);

		min_log_level = MinValue(min_log_level, lookup->level);
	}

	new_config.level = min_log_level;
	new_config.mode = LogMode::ENABLE_SELECTED;
	new_config.enabled = true;

	SetConfigInternal(new_config);
}

void LogManager::TruncateLogSink() {
	unique_lock<mutex> lck(lock);
	log_sink->Truncate();
}

LogConfig LogManager::GetConfig() {
	unique_lock<mutex> lck(lock);
	return config;
}

optional_ptr<const LogType> LogManager::LookupLogType(const string &type) {
	unique_lock<mutex> lck(lock);
	return LookupLogTypeInternal(type);
}

DUCKDB_API void RegisterDefaultLogTypes() {
}

optional_ptr<const LogType> LogManager::LookupLogTypeInternal(const string &type) {
	auto lookup = registered_log_types.find(type);
	if (lookup != registered_log_types.end()) {
		return *lookup->second;
	}
	return nullptr;
}

void LogManager::SetConfigInternal(LogConfig config_p) {
	// Apply the remainder of the config
	config = std::move(config_p);
	global_logger->UpdateConfig(config);
}

void LogManager::RegisterLogType(unique_ptr<LogType> type) {
	unique_lock<mutex> lck(lock);

	auto lookup = registered_log_types.find(type->name);
	if (lookup != registered_log_types.end()) {
		throw InvalidInputException("Registered log writer '%s' already exists", type->name);
	}

	registered_log_types[type->name] = std::move(type);
}

void LogManager::RegisterDefaultLogTypes() {
	RegisterLogType(make_uniq<DefaultLogType>());
	RegisterLogType(make_uniq<FileSystemLogType>());
	RegisterLogType(make_uniq<HTTPLogType>());
	RegisterLogType(make_uniq<QueryLogType>());
	RegisterLogType(make_uniq<PhysicalOperatorLogType>());
	RegisterLogType(make_uniq<MetricsLogType>());
	RegisterLogType(make_uniq<AdaptiveFilterLogType>());
	RegisterLogType(make_uniq<ParquetPrefetchLogType>());
	RegisterLogType(make_uniq<AsyncTaskScheduleLogType>());
	RegisterLogType(make_uniq<ReadAheadBudgetLogType>());
	RegisterLogType(make_uniq<ExternalResourceLogType>());
	RegisterLogType(make_uniq<ProgressVerificationLogType>());
}

} // namespace duckdb
