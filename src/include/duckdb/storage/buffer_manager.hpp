//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer/managed_buffer.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {
class DatabaseInstance;
class TemporaryDirectoryHandle;
struct EvictionQueue;

//! The buffer manager is in charge of handling memory management for the database. It hands out memory buffers that can
//! be used by the database internally.
class BufferManager {
	friend class BufferHandle;
	friend class BlockHandle;

public:
	BufferManager(DatabaseInstance &db, string temp_directory, idx_t maximum_memory);
	~BufferManager();

	//! Register a block with the given block id in the base file
	shared_ptr<BlockHandle> RegisterBlock(block_id_t block_id);

	//! Register an in-memory buffer of arbitrary size, as long as it is >= BLOCK_SIZE. can_destroy signifies whether or
	//! not the buffer can be destroyed when unpinned, or whether or not it needs to be written to a temporary file so
	//! it can be reloaded. The resulting buffer will already be allocated, but needs to be pinned in order to be used.
	shared_ptr<BlockHandle> RegisterMemory(idx_t alloc_size, bool can_destroy);

	//! Allocate an in-memory buffer with a single pin.
	//! The allocated memory is released when the buffer handle is destroyed.
	unique_ptr<BufferHandle> Allocate(idx_t alloc_size);

	//! Reallocate an in-memory buffer that is pinned.
	void ReAllocate(shared_ptr<BlockHandle> &handle, idx_t alloc_size);

	unique_ptr<BufferHandle> Pin(shared_ptr<BlockHandle> &handle);
	void Unpin(shared_ptr<BlockHandle> &handle);

	void UnregisterBlock(block_id_t block_id, bool can_destroy);

	//! Set a new memory limit to the buffer manager, throws an exception if the new limit is too low and not enough
	//! blocks can be evicted
	void SetLimit(idx_t limit = (idx_t)-1);

	static BufferManager &GetBufferManager(ClientContext &context);
	static BufferManager &GetBufferManager(DatabaseInstance &db);

	idx_t GetUsedMemory() {
		return current_memory;
	}
	idx_t GetMaxMemory() {
		return maximum_memory;
	}

	const string &GetTemporaryDirectory() {
		return temp_directory;
	}

	void SetTemporaryDirectory(string new_dir);

private:
	//! Evict blocks until the currently used memory + extra_memory fit, returns false if this was not possible
	//! (i.e. not enough blocks could be evicted)
	bool EvictBlocks(idx_t extra_memory, idx_t memory_limit);

	//! Write a temporary buffer to disk
	void WriteTemporaryBuffer(ManagedBuffer &buffer);
	//! Read a temporary buffer from disk
	unique_ptr<FileBuffer> ReadTemporaryBuffer(block_id_t id);
	//! Get the path of the temporary buffer
	string GetTemporaryPath(block_id_t id);

	void DeleteTemporaryFile(block_id_t id);

	void RequireTemporaryDirectory();

private:
	//! The database instance
	DatabaseInstance &db;
	//! The current amount of memory that is occupied by the buffer manager (in bytes)
	atomic<idx_t> current_memory;
	//! The maximum amount of memory that the buffer manager can keep (in bytes)
	atomic<idx_t> maximum_memory;
	//! The directory name where temporary files are stored
	string temp_directory;
	//! Lock for creating the temp handle
	mutex temp_handle_lock;
	//! Handle for the temporary directory
	unique_ptr<TemporaryDirectoryHandle> temp_directory_handle;
	//! The lock for the set of blocks
	mutex manager_lock;
	//! A mapping of block id -> BlockHandle
	unordered_map<block_id_t, weak_ptr<BlockHandle>> blocks;
	//! Eviction queue
	unique_ptr<EvictionQueue> queue;
	//! The temporary id used for managed buffers
	atomic<block_id_t> temporary_id;
};
} // namespace duckdb
