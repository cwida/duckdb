//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/standard_buffer_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/checked_integer.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

class BlockManager;
class BufferPool;
class TemporaryMemoryManager;
class DatabaseInstance;
class TemporaryDirectoryHandle;
struct EvictionQueue;

//! The BufferManager is in charge of handling memory management for a single database. It cooperatively shares a
//! BufferPool with other BufferManagers, belonging to different databases. It hands out memory buffers that can
//! be used by the database internally, and offers configuration options specific to a database, which need not be
//! shared by the BufferPool, including whether to support swapping temp buffers to disk, and where to swap them to.
class StandardBufferManager : public BufferManager {
	friend class BufferHandle;
	friend class BlockHandle;
	friend class BlockManager;
	friend struct BufferAllocatorData;

public:
	StandardBufferManager(DatabaseInstance &db, string temp_directory);
	~StandardBufferManager() override;

public:
	static unique_ptr<StandardBufferManager> CreateBufferManager(DatabaseInstance &db, string temp_directory);

	//! Registers a transient memory buffer.
	shared_ptr<BlockHandle> RegisterTransientMemory(const idx_t size, BlockManager &block_manager) final;
	//! Registers an in-memory buffer that cannot be unloaded until it is destroyed.
	//! This buffer can be small (smaller than the block size of the temporary block manager).
	//! Unpin and Pin are NOPs on this block of memory.
	shared_ptr<BlockHandle> RegisterSmallMemory(MemoryTag tag, const idx_t size) final;

	idx_t GetUsedMemory() const final;
	idx_t GetMaxMemory() const final;
	idx_t GetUsedSwap() const final;
	optional_idx GetMaxSwap() const final;
	//! Returns the block allocation size for buffer-managed blocks.
	idx_t GetBlockAllocSize() const final;
	//! Returns the block size for buffer-managed blocks.
	idx_t GetBlockSize() const final;
	idx_t GetOperatorMemoryLimit() const final;

	//! Allocate an in-memory buffer with a single pin.
	//! The allocated memory is released when the buffer handle is destroyed.
	DUCKDB_API shared_ptr<BlockHandle> AllocateTemporaryMemory(MemoryTag tag, idx_t block_size,
	                                                           bool can_destroy = true) final;
	DUCKDB_API shared_ptr<BlockHandle> AllocateMemory(MemoryTag tag, BlockManager *block_manager,
	                                                  bool can_destroy = true) final;
	DUCKDB_API BufferHandle Allocate(MemoryTag tag, idx_t block_size, bool can_destroy = true) final;
	DUCKDB_API BufferHandle Allocate(MemoryTag tag, BlockManager *block_manager, bool can_destroy = true) final;
	DUCKDB_API BufferHandle Allocate(QueryContext context, MemoryTag tag, idx_t block_size,
	                                 bool can_destroy = true) final;
	DUCKDB_API BufferHandle Allocate(QueryContext context, MemoryTag tag, BlockManager *block_manager,
	                                 bool can_destroy = true) final;

	BufferHandle Pin(shared_ptr<BlockHandle> &handle) final;
	BufferHandle Pin(const QueryContext &context, shared_ptr<BlockHandle> &handle) final;

	void Prefetch(QueryContext context, vector<shared_ptr<BlockHandle>> &handles) final;
	vector<unique_ptr<AsyncTask>> CreatePrefetchTasks(QueryContext context,
	                                                  vector<shared_ptr<BlockHandle>> &handles) final;
	void Unpin(shared_ptr<BlockHandle> &handle) final;

	//! Set a new memory limit to the buffer manager, throws an exception if the new limit is too low and not enough
	//! blocks can be evicted
	void SetMemoryLimit(idx_t limit = (idx_t)-1) final;
	void SetSwapLimit(optional_idx limit = optional_idx()) final;

	//! Returns information about memory usage
	vector<MemoryInformation> GetMemoryUsageInfo() const override;

	BlockManager &GetTemporaryBlockManager() final;

	//! Returns a list of all temporary files
	vector<TemporaryFileInformation> GetTemporaryFiles() final;

	const string &GetTemporaryDirectory() const final {
		return temporary_directory.path;
	}

	void SetTemporaryDirectory(const string &new_dir) final;

	DUCKDB_API BufferAllocator &GetBufferAllocator() final;

	const DatabaseInstance &GetDatabase() const override {
		return db;
	}
	DatabaseInstance &GetDatabase() override {
		return db;
	}

	//! Construct a managed buffer.
	unique_ptr<FileBuffer> ConstructManagedBuffer(idx_t size, idx_t block_header_size, unique_ptr<FileBuffer> &&source,
	                                              FileBufferType type = FileBufferType::MANAGED_BUFFER) override;

	DUCKDB_API void ReserveMemory(idx_t size) final;
	DUCKDB_API void FreeReservedMemory(idx_t size) final;
	bool HasTemporaryDirectory() const final;
	bool HasFilesInTemporaryDirectory() const final;

protected:
	//! Helper
	template <typename... ARGS>
	TempBufferPoolReservation EvictBlocksOrThrow(QueryContext context, MemoryTag tag, idx_t memory_delta,
	                                             unique_ptr<FileBuffer> *buffer, const ARGS &...);

	//! Register an in-memory buffer of arbitrary size, as long as it is >= BLOCK_SIZE. can_destroy signifies whether or
	//! not the buffer can be destroyed instead of evicted,
	//! if true, it will be destroyed,
	//! if false, it will be written to a temporary file so it can be reloaded
	//! If we want to change this, e.g., to immediately destroy the buffer upon unpinning,
	//! we can call BlockHandle::SetDestroyBufferUpon
	//! The resulting buffer will already be allocated, but needs to be pinned in order to be used.
	//! This needs to be private to prevent creating blocks without ever pinning them:
	//! blocks that are never pinned are never added to the eviction queue
	shared_ptr<BlockHandle> RegisterMemory(MemoryTag tag, idx_t block_size, idx_t block_header_size, bool can_destroy,
	                                       QueryContext context = QueryContext());

	//! Get allocated size for a block
	idx_t GetBlockAllocSize(idx_t block_size) const;

	//! Garbage collect eviction queue
	void PurgeQueue(const BlockHandle &handle) final;

	BufferPool &GetBufferPool() const final;
	TemporaryMemoryManager &GetTemporaryMemoryManager() final;

	//! Write a temporary buffer to disk
	void WriteTemporaryBuffer(QueryContext context, MemoryTag tag, block_id_t block_id, FileBuffer &buffer) final;
	//! Read a temporary buffer from disk
	unique_ptr<FileBuffer> ReadTemporaryBuffer(QueryContext context, MemoryTag tag, BlockHandle &block,
	                                           unique_ptr<FileBuffer> buffer = nullptr) final;
	//! Get the path of the temporary buffer
	string GetTemporaryPath(block_id_t id);

	void DeleteTemporaryFile(BlockMemory &memory) final;

	void RequireTemporaryDirectory();

	void AddToEvictionQueue(shared_ptr<BlockHandle> &handle) final;

	const char *InMemoryWarning();

	//! When the BlockHandle reaches 0 readers, this creates a new FileBuffer for this BlockHandle and
	//! overwrites the data within with garbage. Any readers that do not hold the pin will notice
	void VerifyZeroReaders(BlockLock &l, shared_ptr<BlockHandle> &handle);

	//! A run of handles with adjacent block ids starting at first_block
	struct PrefetchRun {
		block_id_t first_block;
		vector<shared_ptr<BlockHandle>> handles;
	};
	//! Computes the contiguous runs of blocks that still need to be loaded, without performing any I/O
	static vector<PrefetchRun> RegisterPrefetch(vector<shared_ptr<BlockHandle>> &handles);
	//! Synchronously executes every run in the plan through BatchRead
	void ExecutePrefetch(QueryContext context, vector<PrefetchRun> &plan);
	void BatchRead(QueryContext context, PrefetchRun &run);

	bool EncryptTemporaryFiles();

protected:
	// These are stored here because temp_directory creation is lazy
	// so we need to store information related to the temporary directory before it's created
	struct TemporaryFileData {
		//! The directory name where temporary files are stored
		string path;
		//! Publishes successful temporary-directory initialization
		atomic<bool> initialized {false};
		//! Protects configuration and handle creation (mutable so 'GetMaxSwap' can be const)
		mutable mutex lock;
		//! Size of temp file on disk
		atomic<idx_t> size_on_disk = {0};
		//! Handle for the temporary directory
		unique_ptr<TemporaryDirectoryHandle> handle;
		//! The maximum swap space that can be used
		optional_idx maximum_swap_space = optional_idx();
	};

protected:
	//! The database instance
	DatabaseInstance &db;
	//! The buffer pool
	BufferPool &buffer_pool;
	//! The variables related to temporary file management
	TemporaryFileData temporary_directory;
	//! The temporary id used for managed buffers
	atomic<block_id_t> temporary_id;
	//! Allocator associated with the buffer manager, that passes all allocations through this buffer manager
	BufferAllocator buffer_allocator;
	//! Block manager for temp data
	unique_ptr<BlockManager> temp_block_manager;
	//! Temporary evicted memory data per tag
	atomic<CheckedInteger<idx_t, InternalException>> evicted_data_per_tag[MEMORY_TAG_COUNT];
};

} // namespace duckdb
