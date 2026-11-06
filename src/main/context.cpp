#include "duckdb/main/context.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
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
#include "duckdb/storage/buffer/block_handle.hpp"

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
		return context.GetContext().TryGetFileOpener();
	}

private:
	ClientContext &context;
};

//! ContextBufferManager wraps the buffer manager to forward the services of a client.
class ContextBufferManager : public BufferManager {
public:
	explicit ContextBufferManager(Context &context_p) : context(context_p) {
	}

public:
	shared_ptr<BlockHandle> AllocateTemporaryMemory(MemoryTag tag, idx_t block_size, bool can_destroy = true) override {
		auto result = Inner().AllocateTemporaryMemory(tag, block_size, can_destroy);
		// Track allocation based on actual allocated size from the handle
		if (result) {
			TrackMemoryAllocation(result->GetMemory().GetMemoryUsage());
		}
		return result;
	}
	shared_ptr<BlockHandle> AllocateMemory(MemoryTag tag, BlockManager *block_manager,
	                                       bool can_destroy = true) override {
		auto result = Inner().AllocateMemory(tag, block_manager, can_destroy);
		// Track allocation based on actual allocated size from the handle
		if (result) {
			TrackMemoryAllocation(result->GetMemory().GetMemoryUsage());
		}
		return result;
	}
	BufferHandle Allocate(MemoryTag tag, idx_t block_size, bool can_destroy = true) override {
		auto result = Inner().Allocate(QueryContext(context), tag, block_size, can_destroy);
		// Track allocation based on actual allocated size from the handle
		if (result.GetBlockHandle()) {
			TrackMemoryAllocation(result.GetBlockHandle()->GetMemory().GetMemoryUsage());
		}
		return result;
	}
	BufferHandle Allocate(MemoryTag tag, BlockManager *block_manager, bool can_destroy = true) override {
		auto result = Inner().Allocate(QueryContext(context), tag, block_manager, can_destroy);
		// Track allocation based on actual allocated size from the handle
		if (result.GetBlockHandle()) {
			TrackMemoryAllocation(result.GetBlockHandle()->GetMemory().GetMemoryUsage());
		}
		return result;
	}
	BufferHandle Allocate(QueryContext context, MemoryTag tag, idx_t block_size, bool can_destroy = true) override {
		auto result = Inner().Allocate(context, tag, block_size, can_destroy);
		if (result.GetBlockHandle()) {
			TrackMemoryAllocation(result.GetBlockHandle()->GetMemory().GetMemoryUsage());
		}
		return result;
	}
	BufferHandle Allocate(QueryContext context, MemoryTag tag, BlockManager *block_manager,
	                      bool can_destroy = true) override {
		auto result = Inner().Allocate(context, tag, block_manager, can_destroy);
		if (result.GetBlockHandle()) {
			TrackMemoryAllocation(result.GetBlockHandle()->GetMemory().GetMemoryUsage());
		}
		return result;
	}
	BufferHandle Pin(shared_ptr<BlockHandle> &handle) override {
		return Pin(QueryContext(context), handle);
	}
	BufferHandle Pin(const QueryContext &context, shared_ptr<BlockHandle> &handle) override {
		return Inner().Pin(context, handle);
	}
	void Prefetch(QueryContext context, vector<shared_ptr<BlockHandle>> &handles) override {
		return Inner().Prefetch(context, handles);
	}
	void Unpin(shared_ptr<BlockHandle> &handle) override {
		return Inner().Unpin(handle);
	}

	idx_t GetUsedMemory() const override {
		return Inner().GetUsedMemory();
	}
	idx_t GetMaxMemory() const override {
		return Inner().GetMaxMemory();
	}
	idx_t GetUsedSwap() const override {
		return Inner().GetUsedSwap();
	}
	optional_idx GetMaxSwap() const override {
		return Inner().GetMaxSwap();
	}
	idx_t GetBlockAllocSize() const override {
		return Inner().GetBlockAllocSize();
	}
	idx_t GetBlockSize() const override {
		return Inner().GetBlockSize();
	}
	idx_t GetOperatorMemoryLimit() const override {
		idx_t global_budget = Inner().GetOperatorMemoryLimit();
		auto config = context.TryGetClientConfig();
		if (!config || !config->operator_memory_limit.IsValid()) {
			return global_budget;
		}
		return MinValue(global_budget, config->operator_memory_limit.GetIndex());
	}

	shared_ptr<BlockHandle> RegisterTransientMemory(const idx_t size, BlockManager &block_manager) override {
		auto result = Inner().RegisterTransientMemory(size, block_manager);
		TrackMemoryAllocation(size);
		return result;
	}
	shared_ptr<BlockHandle> RegisterSmallMemory(const idx_t size) override {
		auto result = Inner().RegisterSmallMemory(size);
		TrackMemoryAllocation(size);
		return result;
	}
	shared_ptr<BlockHandle> RegisterSmallMemory(MemoryTag tag, const idx_t size) override {
		auto result = Inner().RegisterSmallMemory(tag, size);
		TrackMemoryAllocation(size);
		return result;
	}

	BufferAllocator &GetBufferAllocator() override {
		return Inner().GetBufferAllocator();
	}
	void ReserveMemory(idx_t size) override {
		return Inner().ReserveMemory(size);
	}
	void FreeReservedMemory(idx_t size) override {
		return Inner().FreeReservedMemory(size);
	}
	vector<MemoryInformation> GetMemoryUsageInfo() const override {
		return Inner().GetMemoryUsageInfo();
	}
	void SetMemoryLimit(idx_t limit = (idx_t)-1) override {
		return Inner().SetMemoryLimit(limit);
	}
	void SetSwapLimit(optional_idx limit = optional_idx()) override {
		return Inner().SetSwapLimit(limit);
	}

	BlockManager &GetTemporaryBlockManager() override {
		return Inner().GetTemporaryBlockManager();
	}
	vector<TemporaryFileInformation> GetTemporaryFiles() override {
		return Inner().GetTemporaryFiles();
	}
	const string &GetTemporaryDirectory() const override {
		return Inner().GetTemporaryDirectory();
	}
	void SetTemporaryDirectory(const string &new_dir) override {
		return Inner().SetTemporaryDirectory(new_dir);
	}
	bool HasTemporaryDirectory() const override {
		return Inner().HasTemporaryDirectory();
	}
	bool HasFilesInTemporaryDirectory() const override {
		return Inner().HasFilesInTemporaryDirectory();
	}

	unique_ptr<FileBuffer> ConstructManagedBuffer(idx_t size, idx_t block_header_size, unique_ptr<FileBuffer> &&source,
	                                              FileBufferType type = FileBufferType::MANAGED_BUFFER) override {
		return Inner().ConstructManagedBuffer(size, block_header_size, std::move(source), type);
	}
	BufferPool &GetBufferPool() const override {
		return Inner().GetBufferPool();
	}
	const DatabaseInstance &GetDatabase() const override {
		return Inner().GetDatabase();
	}
	DatabaseInstance &GetDatabase() override {
		return Inner().GetDatabase();
	}
	TemporaryMemoryManager &GetTemporaryMemoryManager() override {
		return Inner().GetTemporaryMemoryManager();
	}

	void PurgeQueue(const BlockHandle &handle) override {
		return Inner().PurgeQueue(handle);
	}
	void AddToEvictionQueue(shared_ptr<BlockHandle> &handle) override {
		return Inner().AddToEvictionQueue(handle);
	}
	void WriteTemporaryBuffer(QueryContext context, MemoryTag tag, block_id_t block_id, FileBuffer &buffer) override {
		return Inner().WriteTemporaryBuffer(context, tag, block_id, buffer);
	}
	unique_ptr<FileBuffer> ReadTemporaryBuffer(QueryContext context, MemoryTag tag, BlockHandle &block,
	                                           unique_ptr<FileBuffer> buffer) override {
		return Inner().ReadTemporaryBuffer(context, tag, block, std::move(buffer));
	}
	void DeleteTemporaryFile(BlockMemory &memory) override {
		return Inner().DeleteTemporaryFile(memory);
	}

private:
	void TrackMemoryAllocation(idx_t size) const {
		auto profiler = context.TryGetQueryProfiler();
		if (size > 0 && profiler) {
			profiler->TrackTotalMemoryAllocated(size);
		}
	}

	//! The database buffer manager is looked up lazily: it might not exist yet when the services are created
	BufferManager &Inner() const {
		return context.GetDatabase().GetBufferManager();
	}

	Context &context;
};

Context::Context(DatabaseInstance &db_p) : db(db_p), buffer_manager(make_uniq<ContextBufferManager>(*this)) {
}

Context::~Context() {
}

void Context::InitializeClient(ClientContext &context_p) {
	D_ASSERT(!context);
	context = &context_p;
	config = make_uniq<ClientConfig>();
	file_opener = make_uniq<ClientContextFileOpener>(*this);
	client_file_system = make_uniq<ClientFileSystem>(context_p);
	profiler = make_shared_ptr<QueryProfiler>(context_p);
	registered_state = make_uniq<RegisteredStateManager>();
}

DatabaseInstance &Context::GetDatabase() const {
	return db;
}

optional_ptr<ClientContext> Context::TryGetClientContext() const {
	return context;
}

optional_ptr<ClientContext> Context::TryGetClientContext(optional_ptr<Context> context) {
	if (!context) {
		return nullptr;
	}
	return context->TryGetClientContext();
}

optional_ptr<QueryProfiler> Context::TryGetQueryProfiler(optional_ptr<Context> context) {
	if (!context) {
		return nullptr;
	}
	return context->TryGetQueryProfiler();
}

void Context::InterruptCheck() const {
	if (context) {
		context->InterruptCheck();
	}
}

bool Context::IsClient() const {
	return context != nullptr;
}

bool Context::IsInterrupted() const {
	return context && context->IsInterrupted();
}

CatalogTransaction Context::GetSystemCatalogTransaction() {
	if (context) {
		return CatalogTransaction::GetSystemCatalogTransaction(*context);
	}
	return CatalogTransaction::GetSystemTransaction(db);
}

optional_ptr<ClientConfig> Context::TryGetClientConfig() const {
	return config.get();
}

ClientConfig &Context::GetClientConfig() const {
	D_ASSERT(config);
	return *config;
}

SettingLookupResult Context::TryGetCurrentSetting(const Identifier &key, Value &result) const {
	if (context) {
		return context->TryGetCurrentSetting(key, result);
	}
	return db.TryGetCurrentSetting(key, result);
}

SettingLookupResult Context::TryGetCurrentUserSetting(idx_t setting_index, Value &result) const {
	auto &db_config = DBConfig::GetConfig(db);
	if (config) {
		return config->user_settings.TryGetSetting(db_config.user_settings, setting_index, result);
	}
	return db_config.TryGetCurrentUserSetting(setting_index, result);
}

Logger &Context::GetLogger() const {
	if (logger) {
		return *logger;
	}
	return Logger::Get(db);
}

void Context::SetLogger(shared_ptr<Logger> new_logger) {
	logger = std::move(new_logger);
}

const shared_ptr<Logger> &Context::GetLoggerPtr() const {
	return logger;
}

FileSystem &Context::GetFileSystem() const {
	if (client_file_system) {
		return *client_file_system;
	}
	return FileSystem::GetFileSystem(db);
}

optional_ptr<FileOpener> Context::TryGetFileOpener() const {
	return file_opener.get();
}

BufferManager &Context::GetBufferManager() const {
	return *buffer_manager;
}

TaskScheduler &Context::GetTaskScheduler() const {
	return db.GetScheduler();
}

MetricsManager &Context::GetMetricsManager() const {
	return db.GetMetricsManager();
}

RegisteredStateManager &Context::GetRegisteredState() const {
	D_ASSERT(registered_state);
	return *registered_state;
}

optional_ptr<QueryProfiler> Context::TryGetQueryProfiler() const {
	return profiler.get();
}

QueryProfiler &Context::GetQueryProfiler() const {
	D_ASSERT(profiler);
	return *profiler;
}

const shared_ptr<QueryProfiler> &Context::GetQueryProfilerPtr() const {
	return profiler;
}

Context &DatabaseInstance::GetContext() {
	return *services;
}

} // namespace duckdb
