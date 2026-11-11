//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/setting_info.hpp"

namespace duckdb {

struct ClientConfig;
class BufferManager;
class ClientContext;
struct CatalogTransaction;
class DatabaseInstance;
class FileOpener;
class FileSystem;
class Logger;
class MetricsManager;
class QueryProfiler;
class RegisteredStateManager;
class TaskScheduler;
class Value;

//! Context owns the services that operations need to do their work: reading settings, logging, recording
//! metrics and accessing the file system.
//! A ClientContext owns one (see ClientContext::GetContext) that holds all of its connection-specific services.
//! A DatabaseInstance can create one (see DatabaseInstance::Services) that only provides database-wide services.
class Context {
public:
	//! Creates database-wide services
	//! The database must outlive the services
	DUCKDB_API explicit Context(DatabaseInstance &db);
	DUCKDB_API ~Context();

	//! Turns these into the services of the given client context: creates the client config, the query profiler,
	//! the file opener and the client file system. The connection logger is set by the context (see SetLogger).
	//! The context must own these services - it is kept as a back reference.
	void InitializeClient(ClientContext &context);

public:
	//! The database instance the services belong to
	DUCKDB_API DatabaseInstance &GetDatabase() const;
	//! The client context owning the services (if any) - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<ClientContext> TryGetClientContext() const;
	//! Null-safe variants for optional services
	DUCKDB_API static optional_ptr<ClientContext> TryGetClientContext(optional_ptr<Context> services);
	DUCKDB_API static optional_ptr<QueryProfiler> TryGetQueryProfiler(optional_ptr<Context> services);
	//! Checks whether the query was interrupted (or timed out) and throws if so - no-op without a client
	DUCKDB_API void InterruptCheck() const;
	//! Whether or not these are the services of a client
	DUCKDB_API bool IsClient() const;
	//! Whether the query of the client was interrupted (always false for database-wide services)
	DUCKDB_API bool IsInterrupted() const;
	//! A catalog transaction on the system catalog: the client's own for client services, otherwise a system one
	DUCKDB_API CatalogTransaction GetSystemCatalogTransaction();

	//! The client config - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<ClientConfig> TryGetClientConfig() const;
	DUCKDB_API ClientConfig &GetClientConfig() const;

	//! Equivalent to CURRENT_SETTING(key) SQL function.
	DUCKDB_API SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) const;
	//! Typed variant, leaves result untouched if not found
	template <class TYPE>
	SettingLookupResult TryGetCurrentSetting(const Identifier &key, TYPE &result) const {
		Value output;
		auto lookup_result = TryGetCurrentSetting(key, output);
		if (lookup_result) {
			result = output.GetValue<TYPE>();
		}
		return lookup_result;
	}
	//! Returns the value of the current setting set by the user - if the user has set it.
	DUCKDB_API SettingLookupResult TryGetCurrentUserSetting(idx_t setting_index, Value &result) const;

	//! The logger: connection-scoped for the services of a ClientContext, otherwise the global logger
	DUCKDB_API Logger &GetLogger() const;
	//! Re-creates the connection logger (e.g. after the logging configuration changed)
	DUCKDB_API void SetLogger(shared_ptr<Logger> new_logger);
	DUCKDB_API const shared_ptr<Logger> &GetLoggerPtr() const;
	//! The file system: client-specific for the services of a ClientContext, otherwise the database file system
	DUCKDB_API FileSystem &GetFileSystem() const;
	//! The file opener of the client - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<FileOpener> TryGetFileOpener() const;
	//! The buffer manager wrapper: forwards to the database buffer manager, tracking the allocations of the services
	DUCKDB_API BufferManager &GetBufferManager() const;
	//! The database-wide task scheduler
	DUCKDB_API TaskScheduler &GetTaskScheduler() const;
	//! The database-wide metrics manager
	DUCKDB_API MetricsManager &GetMetricsManager() const;
	//! The states (e.g. caches) registered by extensions - only present for the services of a ClientContext
	DUCKDB_API RegisteredStateManager &GetRegisteredState() const;
	//! The query profiler of the client - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<QueryProfiler> TryGetQueryProfiler() const;
	DUCKDB_API QueryProfiler &GetQueryProfiler() const;
	DUCKDB_API const shared_ptr<QueryProfiler> &GetQueryProfilerPtr() const;

private:
	DatabaseInstance &db;
	//! Back reference to the owning client context
	optional_ptr<ClientContext> context;
	unique_ptr<ClientConfig> config;
	shared_ptr<Logger> logger;
	shared_ptr<QueryProfiler> profiler;
	unique_ptr<RegisteredStateManager> registered_state;
	unique_ptr<FileOpener> file_opener;
	unique_ptr<FileSystem> client_file_system;
	//! The buffer manager wrapper of the services
	unique_ptr<BufferManager> buffer_manager;
};

} // namespace duckdb
