#include "duckdb/main/database.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/main/connection_manager.hpp"

#ifndef DUCKDB_NO_THREADS
#include "duckdb/common/thread.hpp"
#endif

namespace duckdb {

DBConfig::~DBConfig() {
}

DatabaseInstance::DatabaseInstance() {
}

DatabaseInstance::~DatabaseInstance() {
	// shutting down: attempt to checkpoint the database
	try {
		auto &storage = StorageManager::GetStorageManager(*this);
		if (!storage.InMemory()) {
			auto &config = storage.db.config;
			if (!config.checkpoint_on_shutdown) {
				return;
			}
			storage.CreateCheckpoint(true);
		}
	} catch (...) {
	}
}

BufferManager &BufferManager::GetBufferManager(DatabaseInstance &db) {
	return *db.GetStorageManager().buffer_manager;
}

BlockManager &BlockManager::GetBlockManager(DatabaseInstance &db) {
	return *db.GetStorageManager().block_manager;
}

BlockManager &BlockManager::GetBlockManager(ClientContext &context) {
	return BlockManager::GetBlockManager(DatabaseInstance::GetDatabase(context));
}

DatabaseInstance &DatabaseInstance::GetDatabase(ClientContext &context) {
	return *context.db;
}

StorageManager &StorageManager::GetStorageManager(DatabaseInstance &db) {
	return db.GetStorageManager();
}

Catalog &Catalog::GetCatalog(DatabaseInstance &db) {
	return db.GetCatalog();
}

FileSystem &FileSystem::GetFileSystem(DatabaseInstance &db) {
	return db.GetFileSystem();
}

DBConfig &DBConfig::GetConfig(DatabaseInstance &db) {
	return db.config;
}

TransactionManager &TransactionManager::Get(ClientContext &context) {
	return TransactionManager::Get(DatabaseInstance::GetDatabase(context));
}

TransactionManager &TransactionManager::Get(DatabaseInstance &db) {
	return db.GetTransactionManager();
}

ConnectionManager &ConnectionManager::Get(DatabaseInstance &db) {
	return db.GetConnectionManager();
}

ConnectionManager &ConnectionManager::Get(ClientContext &context) {
	return ConnectionManager::Get(DatabaseInstance::GetDatabase(context));
}

void DatabaseInstance::Initialize(const char *path, DBConfig *new_config) {
	if (new_config) {
		// user-supplied configuration
		Configure(*new_config);
	} else {
		// default configuration
		DBConfig config;
		Configure(config);
	}
	if (config.temporary_directory.empty() && path) {
		// no directory specified: use default temp path
		config.temporary_directory = string(path) + ".tmp";

		// special treatment for in-memory mode
		if (strcmp(path, ":memory:") == 0) {
			config.temporary_directory = ".tmp";
		}
	}
	if (new_config && !new_config->use_temporary_directory) {
		// temporary directories explicitly disabled
		config.temporary_directory = string();
	}

	storage =
	    make_unique<StorageManager>(*this, path ? string(path) : string(), config.access_mode == AccessMode::READ_ONLY);
	catalog = make_unique<Catalog>(*this);
	transaction_manager = make_unique<TransactionManager>(*this);
	scheduler = make_unique<TaskScheduler>();
	object_cache = make_unique<ObjectCache>();
	connection_manager = make_unique<ConnectionManager>();

	// initialize the database
	storage->Initialize();

	// only increase thread count after storage init because we get races on catalog otherwise
	scheduler->SetThreads(config.maximum_threads);
}

DuckDB::DuckDB(const char *path, DBConfig *new_config) : instance(make_shared<DatabaseInstance>()) {
	instance->Initialize(path, new_config);
}

DuckDB::DuckDB(const string &path, DBConfig *config) : DuckDB(path.c_str(), config) {
}

DuckDB::~DuckDB() {
}

StorageManager &DatabaseInstance::GetStorageManager() {
	return *storage;
}

Catalog &DatabaseInstance::GetCatalog() {
	return *catalog;
}

TransactionManager &DatabaseInstance::GetTransactionManager() {
	return *transaction_manager;
}

TaskScheduler &DatabaseInstance::GetScheduler() {
	return *scheduler;
}

ObjectCache &DatabaseInstance::GetObjectCache() {
	return *object_cache;
}

FileSystem &DatabaseInstance::GetFileSystem() {
	return *config.file_system;
}

ConnectionManager &DatabaseInstance::GetConnectionManager() {
	return *connection_manager;
}

FileSystem &DuckDB::GetFileSystem() {
	return instance->GetFileSystem();
}

Allocator &Allocator::Get(ClientContext &context) {
	return Allocator::Get(*context.db);
}

Allocator &Allocator::Get(DatabaseInstance &db) {
	return db.config.allocator;
}

void DatabaseInstance::Configure(DBConfig &new_config) {
	config.access_mode = AccessMode::READ_WRITE;
	if (new_config.access_mode != AccessMode::UNDEFINED) {
		config.access_mode = new_config.access_mode;
	}
	if (new_config.file_system) {
		config.file_system = move(new_config.file_system);
	} else {
		config.file_system = make_unique<VirtualFileSystem>();
	}
	config.maximum_memory = new_config.maximum_memory;
	if (config.maximum_memory == (idx_t)-1) {
		config.maximum_memory = config.file_system->GetAvailableMemory() * 8 / 10;
	}
	if (new_config.maximum_threads == (idx_t)-1) {
#ifndef DUCKDB_NO_THREADS
		config.maximum_threads = 1;
		// FIXME: next release
		// config.maximum_threads = std::thread::hardware_concurrency();
#else
		config.maximum_threads = 1;
#endif
	} else {
		config.maximum_threads = new_config.maximum_threads;
	}
	config.allocator = move(new_config.allocator);
	config.checkpoint_wal_size = new_config.checkpoint_wal_size;
	config.use_direct_io = new_config.use_direct_io;
	config.temporary_directory = new_config.temporary_directory;
	config.collation = new_config.collation;
	config.default_order_type = new_config.default_order_type;
	config.default_null_order = new_config.default_null_order;
	config.enable_external_access = new_config.enable_external_access;
	config.replacement_scans = move(new_config.replacement_scans);
	config.initialize_default_database = new_config.initialize_default_database;
	config.disabled_optimizers = move(new_config.disabled_optimizers);
}

DBConfig &DBConfig::GetConfig(ClientContext &context) {
	return context.db->config;
}

idx_t DatabaseInstance::NumberOfThreads() {
	return scheduler->NumberOfThreads();
}

idx_t DuckDB::NumberOfThreads() {
	return instance->NumberOfThreads();
}

} // namespace duckdb
