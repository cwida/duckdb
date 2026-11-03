#include "duckdb/main/runtime_services.hpp"

#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

//! The file system of a client: the database file system with the client file opener
class ClientFileSystem : public OpenerFileSystem {
public:
	explicit ClientFileSystem(ClientContext &context_p) : context(context_p) {
	}

	FileSystem &GetFileSystem() const override {
		auto &config = DBConfig::GetConfig(context);
		return *config.file_system;
	}

	optional_ptr<FileOpener> GetOpener() const override {
		return context.Services().TryGetFileOpener();
	}

private:
	ClientContext &context;
};

RuntimeServices::RuntimeServices(DatabaseInstance &db_p) : db(db_p) {
}

RuntimeServices::~RuntimeServices() {
}

void RuntimeServices::InitializeClient(ClientContext &context_p) {
	D_ASSERT(!context);
	context = &context_p;
	config = make_uniq<ClientConfig>();
	LoggingContext logging_context(LogContextScope::CONNECTION);
	logger = db.GetLogManager().CreateLogger(logging_context, true);
	file_opener = make_uniq<ClientContextFileOpener>(context_p);
	client_file_system = make_uniq<ClientFileSystem>(context_p);
	profiler = make_shared_ptr<QueryProfiler>(context_p);
	registered_state = make_uniq<RegisteredStateManager>();
}

DatabaseInstance &RuntimeServices::GetDatabase() const {
	return db;
}

optional_ptr<ClientContext> RuntimeServices::TryGetClientContext() const {
	return context;
}

optional_ptr<ClientContext> RuntimeServices::TryGetClientContext(optional_ptr<RuntimeServices> services) {
	if (!services) {
		return nullptr;
	}
	return services->TryGetClientContext();
}

optional_ptr<QueryProfiler> RuntimeServices::TryGetQueryProfiler(optional_ptr<RuntimeServices> services) {
	if (!services) {
		return nullptr;
	}
	return services->TryGetQueryProfiler();
}

bool RuntimeServices::IsClient() const {
	return context != nullptr;
}

optional_ptr<ClientConfig> RuntimeServices::TryGetClientConfig() const {
	return config.get();
}

ClientConfig &RuntimeServices::GetClientConfig() const {
	D_ASSERT(config);
	return *config;
}

SettingLookupResult RuntimeServices::TryGetCurrentSetting(const Identifier &key, Value &result) const {
	if (context) {
		return context->TryGetCurrentSetting(key, result);
	}
	return db.TryGetCurrentSetting(key, result);
}

SettingLookupResult RuntimeServices::TryGetCurrentUserSetting(idx_t setting_index, Value &result) const {
	auto &db_config = DBConfig::GetConfig(db);
	if (config) {
		return config->user_settings.TryGetSetting(db_config.user_settings, setting_index, result);
	}
	return db_config.TryGetCurrentUserSetting(setting_index, result);
}

Logger &RuntimeServices::GetLogger() const {
	if (logger) {
		return *logger;
	}
	return Logger::Get(db);
}

void RuntimeServices::SetLogger(shared_ptr<Logger> new_logger) {
	logger = std::move(new_logger);
}

const shared_ptr<Logger> &RuntimeServices::GetLoggerPtr() const {
	return logger;
}

FileSystem &RuntimeServices::GetFileSystem() const {
	if (client_file_system) {
		return *client_file_system;
	}
	return FileSystem::GetFileSystem(db);
}

optional_ptr<FileOpener> RuntimeServices::TryGetFileOpener() const {
	return file_opener.get();
}

BufferManager &RuntimeServices::GetBufferManager() const {
	auto client_context = context;
	if (client_context) {
		return BufferManager::GetBufferManager(*client_context);
	}
	return BufferManager::GetBufferManager(db);
}

TaskScheduler &RuntimeServices::GetTaskScheduler() const {
	return db.GetScheduler();
}

MetricsManager &RuntimeServices::GetMetricsManager() const {
	return db.GetMetricsManager();
}

RegisteredStateManager &RuntimeServices::GetRegisteredState() const {
	D_ASSERT(registered_state);
	return *registered_state;
}

optional_ptr<QueryProfiler> RuntimeServices::TryGetQueryProfiler() const {
	return profiler.get();
}

QueryProfiler &RuntimeServices::GetQueryProfiler() const {
	D_ASSERT(profiler);
	return *profiler;
}

const shared_ptr<QueryProfiler> &RuntimeServices::GetQueryProfilerPtr() const {
	return profiler;
}

shared_ptr<RuntimeServices> DatabaseInstance::Services() {
	return make_shared_ptr<RuntimeServices>(*this);
}

} // namespace duckdb
