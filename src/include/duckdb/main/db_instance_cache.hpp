//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/db_instance_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/unordered_map.hpp"
#include <functional>

namespace duckdb {
class DBInstanceCache;
class DatabaseFilePathManager;

struct DatabaseCacheEntry {
	DatabaseCacheEntry();
	explicit DatabaseCacheEntry(const shared_ptr<DatabaseInstance> &instance);
	~DatabaseCacheEntry();

	//! Connections and unfinished results keep the instance alive after its last handle is closed
	weak_ptr<DatabaseInstance> instance;
	mutex update_database_mutex;
};

enum class CacheBehavior { AUTOMATIC, ALWAYS_CACHE, NEVER_CACHE };

class DBInstanceCache {
public:
	DBInstanceCache();
	~DBInstanceCache();

	//! Gets a DB Instance from the cache if already exists (Fails if the configurations do not match)
	shared_ptr<DuckDB> GetInstance(const string &database, const DBConfig &config_dict);

	//! Creates and caches a new DB Instance (Fails if a cached instance already exists)
	shared_ptr<DuckDB> CreateInstance(const string &database, DBConfig &config_dict, bool cache_instance = true,
	                                  const std::function<void(DuckDB &)> &on_create = nullptr);

	//! Either returns an existing entry, or creates and caches a new DB Instance
	shared_ptr<DuckDB> GetOrCreateInstance(const string &database, DBConfig &config_dict, bool cache_instance,
	                                       const std::function<void(DuckDB &)> &on_create = nullptr);
	shared_ptr<DuckDB> GetOrCreateInstance(const string &database, DBConfig &config_dict,
	                                       CacheBehavior cache_behavior = CacheBehavior::AUTOMATIC,
	                                       const std::function<void(DuckDB &)> &on_create = nullptr);

	//! Creates an uncached instance with no database attached (see DuckDB::CreateEmpty). Databases attached to it
	//! later go through this cache's path manager, so opening one file twice is detected across every instance
	//! created through the cache.
	shared_ptr<DuckDB> CreateEmptyInstance(DBConfig &config);

private:
	shared_ptr<DatabaseFilePathManager> path_manager;
	//! A map with the cached instances <absolute_path/instance>
	unordered_map<string, weak_ptr<DatabaseCacheEntry>> db_instances;

	//! Lock to alter cache
	mutex cache_lock;

private:
	shared_ptr<DuckDB> GetInstanceInternal(const string &database, const DBConfig &config,
	                                       std::unique_lock<std::mutex> &db_instances_lock);
	shared_ptr<DuckDB> CreateInstanceInternal(const string &database, DBConfig &config_dict, bool cache_instance,
	                                          std::unique_lock<std::mutex> db_instances_lock,
	                                          const std::function<void(DuckDB &)> &on_create);
};
} // namespace duckdb
