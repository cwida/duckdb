#include "duckdb/main/db_instance_cache.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/database_file_path_manager.hpp"

namespace duckdb {

DatabaseCacheEntry::DatabaseCacheEntry() {
}

DatabaseCacheEntry::DatabaseCacheEntry(const shared_ptr<DatabaseInstance> &instance_p) : instance(instance_p) {
}

DatabaseCacheEntry::~DatabaseCacheEntry() {
}

static string GetDBAbsolutePath(const string &database_p, FileSystem &fs) {
	auto database = FileSystem::ExpandPath(database_p, nullptr);
	if (database.empty()) {
		return IN_MEMORY_PATH;
	}
	if (database.rfind(IN_MEMORY_PATH, 0) == 0) {
		// this is a memory db, just return it.
		return database;
	}
	if (!ExtensionHelper::ExtractExtensionPrefixFromPath(database).empty()) {
		// this database path is handled by a replacement open and is not a file path
		return database;
	}
	// canonicalize the path
	return fs.CanonicalizePath(database);
}

static string GetCacheKey(const string &database_p, const DBConfig &config) {
	// Currently the cache key is derived directly from the abs path,
	// but it can apply more transformations to the path if necessary
	string abs_database_path;
	if (config.file_system) {
		abs_database_path = GetDBAbsolutePath(database_p, *config.file_system);
	} else {
		auto tmp_fs = FileSystem::CreateLocal();
		abs_database_path = GetDBAbsolutePath(database_p, *tmp_fs);
	}
#if defined(_WIN32) || defined(__APPLE__)
	return StringUtil::Lower(abs_database_path);
#else  // !(_WIN32 or __APPLE__)
	return abs_database_path;
#endif // _WIN32 or __APPLE__
}

DBInstanceCache::DBInstanceCache() {
	path_manager = make_shared_ptr<DatabaseFilePathManager>();
}

DBInstanceCache::~DBInstanceCache() {
}

shared_ptr<DuckDB> DBInstanceCache::GetInstanceInternal(const string &database, const DBConfig &config,
                                                        std::unique_lock<std::mutex> &db_instances_lock) {
	D_ASSERT(db_instances_lock.owns_lock());
	auto cache_key = GetCacheKey(database, config);
	auto entry = db_instances.find(cache_key);
	if (entry == db_instances.end()) {
		// path does not exist in the list yet - no cache entry
		return nullptr;
	}
	auto weak_cache_entry = entry->second;
	auto cache_entry = weak_cache_entry.lock();
	if (!cache_entry) {
		// cache entry does not exist anymore - clean it up
		db_instances.erase(entry);
		return nullptr;
	}
	shared_ptr<DatabaseInstance> db_instance;
	{
		db_instances_lock.unlock();
		std::lock_guard<mutex> create_db_lock(cache_entry->update_database_mutex);
		db_instance = cache_entry->instance.lock();
	}
	// cache entry exists - check if the actual database still exists
	if (!db_instance) {
		// if the instance does not exist, but the cache entry still exists, the instance is being shut down
		// we need to wait until the database is fully shut down to safely proceed
		// we do this here using a busy spin
		cache_entry.reset();
		while (!weak_cache_entry.expired()) {
		}
		D_ASSERT(!cache_entry);
		// the cache entry has now been deleted - clear it from the set of database instances and return
		db_instances_lock.lock();
		// another thread can have cached a new entry under the same key by now
		auto current_entry = db_instances.find(cache_key);
		if (current_entry != db_instances.end() && current_entry->second.expired()) {
			db_instances.erase(current_entry);
		}
		db_instances_lock.unlock();
		return nullptr;
	}
	// the database instance exists - check that the config matches
	if (db_instance->config != config) {
		throw ConnectionException("Can't open a connection to same database file with a different configuration "
		                          "than existing connections");
	}
	return make_shared_ptr<DuckDB>(*db_instance);
}

shared_ptr<DuckDB> DBInstanceCache::GetInstance(const string &database, const DBConfig &config) {
	unique_lock<mutex> lock {cache_lock};
	return GetInstanceInternal(database, config, lock);
}

shared_ptr<DuckDB> DBInstanceCache::CreateInstanceInternal(const string &database, DBConfig &config,
                                                           const bool cache_instance,
                                                           std::unique_lock<std::mutex> db_instances_lock,
                                                           const std::function<void(DuckDB &)> &on_create) {
	D_ASSERT(db_instances_lock.owns_lock());
	string abs_database_path;
	if (config.file_system) {
		abs_database_path = GetDBAbsolutePath(database, *config.file_system);
	} else {
		auto tmp_fs = FileSystem::CreateLocal();
		abs_database_path = GetDBAbsolutePath(database, *tmp_fs);
	}
	// Creates new instance
	string instance_path = abs_database_path;
	if (abs_database_path.rfind(IN_MEMORY_PATH, 0) == 0) {
		instance_path = IN_MEMORY_PATH;
	}
	shared_ptr<DuckDB> db_instance;
	config.path_manager = path_manager;
	if (cache_instance) {
		string cache_key = GetCacheKey(database, config);
		D_ASSERT(db_instances.find(cache_key) == db_instances.end());
		shared_ptr<DatabaseCacheEntry> cache_entry = make_shared_ptr<DatabaseCacheEntry>();
		config.db_cache_entry = cache_entry;
		// Create the new instance after unlocking to avoid new ddb creation requests to be blocked
		lock_guard<mutex> create_db_lock(cache_entry->update_database_mutex);
		db_instances[cache_key] = cache_entry;
		db_instances_lock.unlock();
		db_instance = make_shared_ptr<DuckDB>(instance_path, &config);
		cache_entry->instance = db_instance->instance;
	} else {
		db_instances_lock.unlock();
		db_instance = make_shared_ptr<DuckDB>(instance_path, &config);
	}
	if (on_create) {
		on_create(*db_instance);
	}
	return db_instance;
}

shared_ptr<DuckDB> DBInstanceCache::CreateInstance(const string &database, DBConfig &config, bool cache_instance,
                                                   const std::function<void(DuckDB &)> &on_create) {
	return CreateInstanceInternal(database, config, cache_instance, unique_lock<mutex>(cache_lock), on_create);
}

shared_ptr<DuckDB> DBInstanceCache::CreateEmptyInstance(DBConfig &config) {
	config.path_manager = path_manager;
	return DuckDB::CreateEmpty(&config);
}

shared_ptr<DuckDB> DBInstanceCache::GetOrCreateInstance(const string &database, DBConfig &config_dict,
                                                        bool cache_instance,
                                                        const std::function<void(DuckDB &)> &on_create) {
	auto cache_behavior = cache_instance ? CacheBehavior::ALWAYS_CACHE : CacheBehavior::NEVER_CACHE;
	return GetOrCreateInstance(database, config_dict, cache_behavior, on_create);
}

shared_ptr<DuckDB> DBInstanceCache::GetOrCreateInstance(const string &database, DBConfig &config_dict,
                                                        CacheBehavior cache_behavior,
                                                        const std::function<void(DuckDB &)> &on_create) {
	unique_lock<mutex> lock(cache_lock, std::defer_lock);
	bool cache_instance = cache_behavior == CacheBehavior::ALWAYS_CACHE;
	if (cache_behavior == CacheBehavior::AUTOMATIC) {
		// cache all unnamed in-memory connections
		cache_instance = true;
		if (database == IN_MEMORY_PATH || database.empty()) {
			cache_instance = false;
		}
	}
	if (cache_instance) {
		// While we do not own the lock, we cannot definitively say that the database instance does not exist.
		while (!lock.owns_lock()) {
			// The problem is, that we have to unlock the mutex in GetInstanceInternal, so we can non-blockingly wait
			// for the database creation within the cache entry.
			// Now even after unlocking and waiting for the DB creation we cannot guarantee that the database exists (it
			// could have gone out of scope in the meantime). If that happened we have unlocked the global lock to wait,
			// however we still return a nullptr. In this case we have to re-lock and try again until we either do not
			// find a cache entry (can be done without unlocking) or we find a cache entry and the database is valid as
			// well (in this case we can return that database)
			lock.lock();
			auto instance = GetInstanceInternal(database, config_dict, lock);
			if (instance) {
				return instance;
			}
		}
	} else {
		lock.lock();
	}
	return CreateInstanceInternal(database, config_dict, cache_instance, std::move(lock), on_create);
}

} // namespace duckdb
