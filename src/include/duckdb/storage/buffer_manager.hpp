//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer/temporary_file_information.hpp"

namespace duckdb {
class BlockMemory;
class AsyncTask;
class BufferPool;
class TemporaryMemoryManager;
class AttachedDatabase;
class BlockManager;
class DatabaseInstance;

class BufferManager {
	friend class BufferHandle;
	friend class BlockHandle;
	friend class BlockManager;

public:
	BufferManager() {
	}
	virtual ~BufferManager() {
	}

public:
	DUCKDB_API static BufferManager &GetBufferManager(DatabaseInstance &db);
	DUCKDB_API static const BufferManager &GetBufferManager(const DatabaseInstance &db);
	DUCKDB_API static BufferManager &GetBufferManager(ClientContext &context);
	DUCKDB_API static const BufferManager &GetBufferManager(const ClientContext &context);
	DUCKDB_API static BufferManager &GetBufferManager(AttachedDatabase &db);
	DUCKDB_API static idx_t GetAllocSize(const idx_t alloc_size);

public:
	//! Allocate temporary memory of size block_size.
	virtual shared_ptr<BlockHandle> AllocateTemporaryMemory(MemoryTag tag, idx_t block_size,
	                                                        bool can_destroy = true) = 0;
	//! Allocate block-based memory.
	//! The block manager provides the block size, and the block header size.
	//! Returns the BlockHandle managing the registered memory.
	virtual shared_ptr<BlockHandle> AllocateMemory(MemoryTag tag, BlockManager *block_manager,
	                                               bool can_destroy = true) = 0;
	//! Allocate (temporary) memory of size block_size, and pin it.
	virtual BufferHandle Allocate(MemoryTag tag, idx_t block_size, bool can_destroy = true) = 0;
	//! Allocate block-based memory and pin it.
	virtual BufferHandle Allocate(MemoryTag tag, BlockManager *block_manager, bool can_destroy = true) = 0;
	//! Allocate variants that attribute the (possible) eviction/spill I/O to a query. The base implementations
	//! ignore the context and delegate to the variants above; StandardBufferManager threads it through.
	virtual BufferHandle Allocate(QueryContext context, MemoryTag tag, idx_t block_size, bool can_destroy = true);
	virtual BufferHandle Allocate(QueryContext context, MemoryTag tag, BlockManager *block_manager,
	                              bool can_destroy = true);
	//! Pin a block handle.
	virtual BufferHandle Pin(shared_ptr<BlockHandle> &handle) = 0;
	virtual BufferHandle Pin(const QueryContext &context, shared_ptr<BlockHandle> &handle) = 0;
	//! Pre-fetch a series of blocks.
	//! Using this function is a performance suggestion.
	virtual void Prefetch(QueryContext context, vector<shared_ptr<BlockHandle>> &handles) = 0;
	//! Creates one async task per contiguous run of blocks needing load
	virtual vector<unique_ptr<AsyncTask>> CreatePrefetchTasks(QueryContext context,
	                                                          vector<shared_ptr<BlockHandle>> &handles);
	//! Unpin a block handle.
	virtual void Unpin(shared_ptr<BlockHandle> &handle) = 0;

	//! Returns the currently allocated memory.
	virtual idx_t GetUsedMemory() const = 0;
	//! Returns the maximum available memory.
	virtual idx_t GetMaxMemory() const = 0;
	//! Returns the currently used swap space.
	virtual idx_t GetUsedSwap() const = 0;
	//! Returns the maximum swap space that can be used.
	virtual optional_idx GetMaxSwap() const = 0;
	//! Returns the block allocation size for buffer-managed blocks.
	virtual idx_t GetBlockAllocSize() const = 0;
	//! Returns the block size for buffer-managed blocks.
	virtual idx_t GetBlockSize() const = 0;
	//! Returns the maximum available memory for a given query.
	virtual idx_t GetOperatorMemoryLimit() const = 0;

	//! Returns a newly registered block of transient memory.
	virtual shared_ptr<BlockHandle> RegisterTransientMemory(const idx_t size, BlockManager &block_manager);
	//! Returns a newly registered block of memory that is smaller than the block size setting.
	virtual shared_ptr<BlockHandle> RegisterSmallMemory(const idx_t size);
	//! Returns a newly registered block of memory that is smaller than the block size setting and has a memory tag.
	virtual shared_ptr<BlockHandle> RegisterSmallMemory(MemoryTag tag, const idx_t size);

	//! Get the buffer allocator.
	virtual DUCKDB_API BufferAllocator &GetBufferAllocator();
	//! Reserve memory.
	virtual DUCKDB_API void ReserveMemory(idx_t size);
	//! Free reserved memory.
	virtual DUCKDB_API void FreeReservedMemory(idx_t size);
	//! GetMemoryUsageInfo returns MemoryInformation for each memory tag.
	virtual vector<MemoryInformation> GetMemoryUsageInfo() const = 0;
	//! Set a new memory limit.
	//! Throws an exception, if the new limit is too low, meaning not enough blocks can be evicted.
	virtual void SetMemoryLimit(idx_t limit = (idx_t)-1);
	//! Set a new swap limit.
	virtual void SetSwapLimit(optional_idx limit = optional_idx());

	//! Get the block manager used for in-memory data
	virtual BlockManager &GetTemporaryBlockManager() = 0;
	//! Get the temporary file information of each temporary file.
	virtual vector<TemporaryFileInformation> GetTemporaryFiles();
	//! Get the path to the temporary file directory.
	virtual const string &GetTemporaryDirectory() const;
	//! Set the path to the temporary file directory.
	virtual void SetTemporaryDirectory(const string &new_dir);
	//! Returns true, if the path to the temporary file directory is not empty.
	virtual bool HasTemporaryDirectory() const;
	//! Returns true if there are files found in the temporary directory
	virtual bool HasFilesInTemporaryDirectory() const;

	//! Construct a managed buffer.
	virtual unique_ptr<FileBuffer> ConstructManagedBuffer(idx_t size, idx_t block_header_size,
	                                                      unique_ptr<FileBuffer> &&source,
	                                                      FileBufferType type = FileBufferType::MANAGED_BUFFER);
	//! Get the buffer pool.
	virtual BufferPool &GetBufferPool() const;
	//! Get the const database.
	virtual const DatabaseInstance &GetDatabase() const = 0;
	//! Get the database.
	virtual DatabaseInstance &GetDatabase() = 0;
	//! Get the manager assigning reservations for temporary memory, e.g., for query intermediates.
	virtual TemporaryMemoryManager &GetTemporaryMemoryManager();

	//! Purge the eviction queue of the block handle.
	virtual void PurgeQueue(const BlockHandle &handle) = 0;
	//! Add the block handle to the eviction queue.
	virtual void AddToEvictionQueue(shared_ptr<BlockHandle> &handle);
	//! Write a temporary file buffer.
	virtual void WriteTemporaryBuffer(QueryContext context, MemoryTag tag, block_id_t block_id, FileBuffer &buffer);
	//! Read a temporary buffer.
	virtual unique_ptr<FileBuffer> ReadTemporaryBuffer(QueryContext context, MemoryTag tag, BlockHandle &block,
	                                                   unique_ptr<FileBuffer> buffer);
	//! Delete the temporary file containing the block memory.
	virtual void DeleteTemporaryFile(BlockMemory &memory);
};

} // namespace duckdb
