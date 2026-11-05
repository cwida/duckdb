#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_storage.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/local_file_system.hpp"

namespace duckdb {

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

bool LogManager::RegisterLogStorage(const string &name, shared_ptr<LogStorage> &storage) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	if (registered_log_storages.find(name) != registered_log_storages.end()) {
		return false;
	}
	registered_log_storages.insert({name, std::move(storage)});
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
	log_storage->FlushAll();
}

shared_ptr<LogStorage> LogManager::GetLogStorage() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return log_storage;
}

bool LogManager::CanScan(LoggingTargetTable table) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return log_storage->CanScan(table);
}

LogManager::LogManager(DatabaseInstance &db, LogConfig config_p)
    : config(make_shared_ptr<const LogConfig>(std::move(config_p))), db_instance(db) {
	log_storage = make_uniq<InMemoryLogStorage>(db);
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
		log_storage->WriteLogEntry(timestamp, log_level, log_type, log_message, context);
	}
}

void LogManager::FlushCachedLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) {
	throw NotImplementedException("FlushCachedLogEntries");
}

void LogManager::SetConfig(DatabaseInstance &db, const LogConfig &config_p) {
	annotated_unique_lock<annotated_mutex> lck(lock);

	// We need extra handling for switching storage
	auto new_config = config_p;
	SetLogStorageInternal(db, new_config, config_p.storage);
	SetConfigInternal(std::move(new_config));
}

pair<LogConfig, shared_ptr<LogStorage>> LogManager::GetConfigAndStorage() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	return make_pair(*config, log_storage);
}

void LogManager::RestoreConfig(const LogConfig &config_p, shared_ptr<LogStorage> storage) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	log_storage = std::move(storage);
	SetConfigInternal(config_p);
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

void LogManager::SetLogStorage(DatabaseInstance &db, const string &storage_name) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	// 'SET logging_storage' cannot supply the path that file storage requires, so reject the switch
	// here (active storage preserved) and point users at enable_logging instead of installing a
	// path-less storage that throws on every later flush.
	auto storage_name_to_lower = StringUtil::Lower(storage_name);
	if (storage_name_to_lower == LogConfig::FILE_STORAGE_NAME && config->storage != storage_name_to_lower) {
		throw InvalidConfigurationException(
		    "Cannot select 'file' log storage via 'SET logging_storage' because it requires a path. "
		    "Use CALL enable_logging(storage='file', storage_path='...') instead.");
	}
	auto new_config = *config;
	SetLogStorageInternal(db, new_config, storage_name);
	SetConfigInternal(std::move(new_config));
}

void LogManager::SetLogStorageInternal(DatabaseInstance &db, LogConfig &new_config, const string &storage_name) {
	auto storage_name_to_lower = StringUtil::Lower(storage_name);

	// keep the lowercased name, so that it is recognized when set again
	new_config.storage = storage_name_to_lower;
	if (config->storage == storage_name_to_lower) {
		return;
	}

	if (storage_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		auto &fs = FileSystem::GetFileSystem(db);
		if (fs.SubSystemIsDisabled(LocalFileSystem().GetName())) {
			throw InvalidConfigurationException("Can not enable file logging with the LocalFileSystem disabled");
		}
	}

	// Flush the old storage, we are going to replace it.
	log_storage->FlushAll();

	if (storage_name_to_lower == LogConfig::IN_MEMORY_STORAGE_NAME) {
		log_storage = make_shared_ptr<InMemoryLogStorage>(db);
	} else if (storage_name_to_lower == LogConfig::STDOUT_STORAGE_NAME) {
		log_storage = make_shared_ptr<StdOutLogStorage>(db);
	} else if (storage_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		log_storage = make_shared_ptr<FileLogStorage>(db);
	} else if (registered_log_storages.find(storage_name_to_lower) != registered_log_storages.end()) {
		log_storage = registered_log_storages[storage_name_to_lower];
	} else {
		throw InvalidInputException("Log storage '%s' is not yet registered", storage_name);
	}
}

void LogManager::UpdateLogStorageConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config_value) {
	annotated_unique_lock<annotated_mutex> lck(lock);
	log_storage->UpdateConfig(db, config_value);
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

void LogManager::TruncateLogStorage() {
	annotated_unique_lock<annotated_mutex> lck(lock);
	log_storage->Truncate();
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
