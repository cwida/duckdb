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

unique_ptr<Logger> LogManager::CreateLogger(const LoggingContext &context, bool thread_safe, bool mutable_settings) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	if (mutable_settings) {
		return make_uniq<MutableLogger>(*config, RegisterLoggingContextInternal(context), *this);
	}
	if (!thread_safe) {
		// TODO: implement ThreadLocalLogger and return it here
	}
	return CreateLoggerInternal(context, config);
}

unique_ptr<Logger> LogManager::CreateLogger(const LoggingContext &context, const LogConfigSnapshot &snapshot) {
	if (snapshot.config && snapshot.version == config_version.load()) {
		return CreateLoggerInternal(context, snapshot.config);
	}
	annotated_unique_lock<annotated_mutex> lck(lock);
	return CreateLoggerInternal(context, config);
}

unique_ptr<Logger> LogManager::CreateLogger(const LoggingContext &context, LogConfigSnapshot &snapshot) {
	if (!snapshot.config || snapshot.version != config_version.load()) {
		annotated_unique_lock<annotated_mutex> lck(lock);
		snapshot.config = config;
		snapshot.version = config_version.load();
	}
	return CreateLoggerInternal(context, snapshot.config);
}

unique_ptr<Logger> LogManager::CreateLoggerInternal(const LoggingContext &context,
                                                    const shared_ptr<const LogConfig> &logger_config) {
	if (!logger_config->enabled) {
		return make_uniq<NopLogger>(*this);
	}
	return make_uniq<ThreadSafeLogger>(logger_config, RegisterLoggingContextInternal(context), *this);
}

RegisteredLoggingContext LogManager::RegisterLoggingContext(const LoggingContext &context) {
	return RegisterLoggingContextInternal(context);
}

bool LogManager::RegisterLogSink(const string &name, shared_ptr<LogSink> &sink) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	// the built-in sinks are created by the LogManager, they cannot be replaced
	if (LogConfig::IsBuiltInLogSink(StringUtil::Lower(name)) ||
	    registered_log_sinks.find(name) != registered_log_sinks.end()) {
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
	annotated_unique_lock<annotated_mutex> lck(lock);
	main_log_sink->FlushAll();
	for (auto &entry : added_log_sinks) {
		entry.second->FlushAll();
	}
}

shared_ptr<LogSink> LogManager::GetMainLogSink() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return main_log_sink;
}

shared_ptr<LogSink> LogManager::GetLogSink(const string &name) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return GetLogSinkInternal(name);
}

shared_ptr<LogSink> LogManager::GetEnabledLogSinkInternal(const string &sink_name) {
	if (StringUtil::CIEquals(sink_name, config->storage)) {
		return main_log_sink;
	}
	auto entry = added_log_sinks.find(sink_name);
	if (entry == added_log_sinks.end()) {
		return nullptr;
	}
	return entry->second;
}

shared_ptr<LogSink> LogManager::GetLogSinkInternal(const string &name) {
	if (name.empty()) {
		return main_log_sink;
	}
	auto sink = GetEnabledLogSinkInternal(name);
	if (sink) {
		return sink;
	}
	if (LogConfig::IsBuiltInLogSink(StringUtil::Lower(name)) ||
	    registered_log_sinks.find(name) != registered_log_sinks.end()) {
		throw InvalidInputException("Log sink '%s' is not enabled", name);
	}
	throw InvalidInputException("Log sink '%s' is not yet registered", name);
}

shared_ptr<LogSink> LogManager::GetScannableLogSink(const string &name, LoggingTargetTable table) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto sink = GetLogSinkInternal(name);
	if (!sink->CanScan(table)) {
		if (name.empty()) {
			throw InvalidConfigurationException(
			    "Log sink '%s' does not support this query. Select a queryable sink, such as 'memory' or 'file', "
			    "before generating logs.",
			    config->storage);
		}
		throw InvalidConfigurationException("Log sink '%s' does not support this query", name);
	}
	return sink;
}

shared_ptr<LogSink> LogManager::GetRegisteredLogSink(const string &name) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto entry = registered_log_sinks.find(name);
	if (entry == registered_log_sinks.end()) {
		return nullptr;
	}
	return entry->second;
}

bool LogManager::IsLogSinkEnabled(const string &name) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return GetEnabledLogSinkInternal(name) != nullptr;
}

bool LogManager::CanScan(LoggingTargetTable table) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return main_log_sink->CanScan(table);
}

LogManager::LogManager(DatabaseInstance &db, LogConfig config_p)
    : config(make_shared_ptr<const LogConfig>(std::move(config_p))), db_instance(db) {
	main_log_sink = make_shared_ptr<InMemoryLogSink>(db);
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

RegisteredLoggingContext LogManager::RegisterLoggingContextInternal(const LoggingContext &context) {
	auto context_id = next_registered_logging_context_index++;
	if (context_id >= NumericLimits<idx_t>::Maximum() - 1) {
		throw InternalException("Ran out of available log context ids.");
	}
	return {context_id, context};
}

void LogManager::WriteLogEntry(timestamp_t timestamp, const char *log_type, LogLevel log_level, const char *log_message,
                               const RegisteredLoggingContext &context) {
	if (log_level == LogLevel::LOG_WARNING && Settings::Get<WarningsAsErrorsSetting>(db_instance)) {
		throw InvalidInputException(log_message);
	} else {
		annotated_unique_lock<annotated_mutex> lck(lock);
		main_log_sink->WriteLogEntry(timestamp, log_level, log_type, log_message, context);
		for (auto &entry : added_log_sinks) {
			entry.second->WriteLogEntry(timestamp, log_level, log_type, log_message, context);
		}
	}
}

void LogManager::FlushCachedLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) {
	throw NotImplementedException("FlushCachedLogEntries");
}

void LogManager::SetConfig(DatabaseInstance &db, const LogConfig &config_p) {
	auto sink_name = ResolveDeprecatedLogSinkName(db, config_p.storage);

	annotated_unique_lock<annotated_mutex> lck(lock);

	// We need extra handling for switching sink
	auto new_config = config_p;
	SetMainLogSinkInternal(db, new_config, sink_name);
	SetConfigInternal(std::move(new_config));
}

LogConfigAndSinks LogManager::GetConfigAndSinks() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return LogConfigAndSinks {*config, main_log_sink, added_log_sinks};
}

void LogManager::RestoreConfig(LogConfigAndSinks previous) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	main_log_sink = std::move(previous.main_log_sink);
	added_log_sinks = std::move(previous.added_log_sinks);
	SetConfigInternal(std::move(previous.config));
}

void LogManager::SetEnableLogging(bool enable) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto new_config = *config;
	new_config.enabled = enable;
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetLogMode(LogMode mode) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto new_config = *config;
	new_config.mode = mode;
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetLogLevel(LogLevel level) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto new_config = *config;
	new_config.level = level;
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetEnabledLogTypes(optional_ptr<unordered_set<string>> enabled_log_types) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto new_config = *config;
	if (enabled_log_types) {
		new_config.enabled_log_types = *enabled_log_types;
	} else {
		new_config.enabled_log_types = {};
	}
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetDisabledLogTypes(optional_ptr<unordered_set<string>> disabled_log_types) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	auto new_config = *config;
	if (disabled_log_types) {
		new_config.disabled_log_types = *disabled_log_types;
	} else {
		new_config.disabled_log_types = {};
	}
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetMainLogSink(DatabaseInstance &db, const string &requested_sink_name) {
	auto sink_name = ResolveDeprecatedLogSinkName(db, requested_sink_name);

	annotated_unique_lock<annotated_mutex> lck(lock);
	// 'SET logging_sink' cannot supply the path that file sink requires, so reject the switch
	// here (active sink preserved) and point users at enable_logging instead of installing a
	// path-less sink that throws on every later flush.
	auto sink_name_to_lower = StringUtil::Lower(sink_name);
	if (sink_name_to_lower == LogConfig::FILE_STORAGE_NAME && !GetEnabledLogSinkInternal(sink_name_to_lower)) {
		throw InvalidConfigurationException(
		    "Cannot select 'file' log sink via 'SET logging_sink' because it requires a path. "
		    "Use CALL enable_logging(sink='file', sink_path='...') instead.");
	}
	auto new_config = *config;
	SetMainLogSinkInternal(db, new_config, sink_name);
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetMainLogSinkInternal(DatabaseInstance &db, LogConfig &new_config, const string &sink_name) {
	auto sink_name_to_lower = StringUtil::Lower(sink_name);

	// keep the lowercased name, so that it is recognized when set again
	new_config.storage = sink_name_to_lower;
	if (config->storage == sink_name_to_lower) {
		return;
	}

	// an added sink becomes the main log sink, it keeps its entries
	auto new_sink = GetEnabledLogSinkInternal(sink_name_to_lower);
	if (!new_sink) {
		new_sink = CreateLogSinkInternal(db, sink_name_to_lower);
	}

	// Flush the old sink, we are going to replace it.
	main_log_sink->FlushAll();
	added_log_sinks.erase(sink_name_to_lower);
	main_log_sink = std::move(new_sink);
}

shared_ptr<LogSink> LogManager::CreateLogSinkInternal(DatabaseInstance &db, const string &sink_name) {
	auto sink_name_to_lower = StringUtil::Lower(sink_name);
	if (sink_name_to_lower == LogConfig::IN_MEMORY_STORAGE_NAME) {
		return make_shared_ptr<InMemoryLogSink>(db);
	}
	if (sink_name_to_lower == LogConfig::STDOUT_STORAGE_NAME) {
		return make_shared_ptr<StdOutLogSink>(db);
	}
	if (sink_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		auto &fs = FileSystem::GetFileSystem(db);
		if (fs.SubSystemIsDisabled(LocalFileSystem().GetName())) {
			throw InvalidConfigurationException("Can not enable file logging with the LocalFileSystem disabled");
		}
		return make_shared_ptr<FileLogSink>(db);
	}
	auto entry = registered_log_sinks.find(sink_name_to_lower);
	if (entry == registered_log_sinks.end()) {
		throw InvalidInputException("Log sink '%s' is not yet registered", sink_name);
	}
	return entry->second;
}

void LogManager::AddLogSink(DatabaseInstance &db, const string &requested_sink_name,
                            const case_insensitive_map_t<Value> &sink_config) {
	auto sink_name = StringUtil::Lower(ResolveDeprecatedLogSinkName(db, requested_sink_name));
	auto config_copy = sink_config;

	annotated_unique_lock<annotated_mutex> lck(lock);
	auto sink = GetEnabledLogSinkInternal(sink_name);
	if (sink) {
		// the sink already receives the log entries, only update its config
		if (!config_copy.empty()) {
			sink->UpdateConfig(db, config_copy);
		}
	} else {
		if (sink_name == LogConfig::FILE_STORAGE_NAME) {
			auto path_entry = config_copy.find("path");
			if (path_entry == config_copy.end() || path_entry->second.IsNull() ||
			    path_entry->second.ToString().empty()) {
				throw InvalidInputException(
				    "The 'file' log sink requires a path, e.g. CALL add_log_sink('file', path='log.csv')");
			}
		}
		sink = CreateLogSinkInternal(db, sink_name);
		if (!config_copy.empty()) {
			sink->UpdateConfig(db, config_copy);
		}
		added_log_sinks[sink_name] = std::move(sink);
	}

	if (!config->enabled) {
		auto new_config = *config;
		new_config.enabled = true;
		SetConfigInternal(std::move(new_config));
	}
}

void LogManager::RemoveLogSink(DatabaseInstance &db, const string &requested_sink_name) {
	auto sink_name = StringUtil::Lower(ResolveDeprecatedLogSinkName(db, requested_sink_name));

	annotated_unique_lock<annotated_mutex> lck(lock);
	if (sink_name == config->storage) {
		throw InvalidInputException("Log sink '%s' is the main log sink and cannot be removed, switch it with "
		                            "CALL enable_logging(sink='...') or SET logging_sink",
		                            requested_sink_name);
	}
	auto entry = added_log_sinks.find(sink_name);
	if (entry == added_log_sinks.end()) {
		throw InvalidInputException("Log sink '%s' is not enabled", requested_sink_name);
	}
	entry->second->FlushAll();
	added_log_sinks.erase(entry);
}

void LogManager::UpdateMainLogSinkConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config_value) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	main_log_sink->UpdateConfig(db, config_value);
}

void LogManager::SetEnableStructuredLoggers(vector<string> &enabled_logger_types) {
	annotated_unique_lock<annotated_mutex> lck(lock);

	LogConfig new_config = *config;
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

	SetConfigInternal(std::move(new_config));
}

void LogManager::TruncateMainLogSink() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	main_log_sink->Truncate();
}

LogConfig LogManager::GetConfig() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return *config;
}

optional_ptr<const LogType> LogManager::LookupLogType(const string &type) {
	annotated_unique_lock<annotated_mutex> lck(lock);
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

void LogManager::SetConfigInternal(LogConfig new_config) {
	config = make_shared_ptr<const LogConfig>(std::move(new_config));
	config_version++;
	global_logger->UpdateConfig(*config);
}

void LogManager::RegisterLogType(unique_ptr<LogType> type) {
	annotated_unique_lock<annotated_mutex> lck(lock);

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
