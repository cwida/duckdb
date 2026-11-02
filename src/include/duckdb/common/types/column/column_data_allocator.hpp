//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/column/column_data_allocator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/result_set_manager.hpp"

namespace duckdb {

struct ChunkMetaData;
struct VectorMetaData;
struct SwizzleMetaData;

struct BlockMetaData {
public:
	//! How much space is currently used within the block
	uint32_t size;
	//! How much space is available in the block
	uint32_t capacity;

private:
	//! The underlying block handle
	shared_ptr<BlockHandle> handle;
	//! Weak pointer to underlying block handle (if ColumnDataCollectionLifetime::DATABASE_INSTANCE)
	weak_ptr<BlockHandle> weak_handle;

public:
	shared_ptr<BlockHandle> GetHandle() const;
	BufferHandle Pin(BufferManager &buffer_manager);
	void SetHandle(ManagedResultSet &managed_result_set, shared_ptr<BlockHandle> handle);
	uint32_t Capacity();
};

class ColumnDataAllocator {
public:
	explicit ColumnDataAllocator(Allocator &allocator);
	explicit ColumnDataAllocator(BufferManager &buffer_manager,
	                             ColumnDataCollectionLifetime lifetime = ColumnDataCollectionLifetime::REGULAR);
	ColumnDataAllocator(ClientContext &context, ColumnDataAllocatorType allocator_type,
	                    ColumnDataCollectionLifetime lifetime = ColumnDataCollectionLifetime::REGULAR);
	ColumnDataAllocator(ColumnDataAllocator &allocator);
	~ColumnDataAllocator();

	//! Returns an allocator object to allocate with. This returns the allocator in IN_MEMORY_ALLOCATOR, and a buffer
	//! allocator in case of BUFFER_MANAGER_ALLOCATOR.
	Allocator &GetAllocator();
	//! Returns the buffer manager, if this is not an in-memory allocation.
	BufferManager &GetBufferManager();
	//! Returns the allocator type
	ColumnDataAllocatorType GetType() {
		return type;
	}
	void MakeShared() {
		shared = true;
	}
	bool IsShared() const {
		return shared;
	}
	//! Whether this allocator belongs to a managed result set
	bool IsManagedResultSet() const {
		return managed_result_set.IsValid();
	}
	idx_t BlockCount() const {
		return blocks.size();
	}
	idx_t SizeInBytes() const {
		idx_t total_size = 0;
		for (const auto &block : blocks) {
			total_size += block.size;
		}
		return total_size;
	}
	idx_t AllocationSize() const {
		return allocated_size;
	}
	//! Sets the partition index of this tuple data collection
	void SetPartitionIndex(idx_t index) {
		D_ASSERT(!partition_index.IsValid());
		D_ASSERT(blocks.empty() && allocated_data.empty());
		partition_index = index;
	}

public:
	void AllocateData(idx_t size, uint32_t &block_id, uint32_t &offset, ChunkManagementState *chunk_state);
	void Reset();
	void ResetPreserveLastBlock();

	void Initialize(ColumnDataAllocator &other);
	void InitializeChunkState(ChunkManagementState &state, ChunkMetaData &meta_data);
	data_ptr_t GetDataPointer(ChunkManagementState &state, uint32_t block_id, uint32_t offset);
	void UnswizzlePointers(ChunkManagementState &state, Vector &result, SwizzleMetaData &swizzle_segment,
	                       const VectorMetaData &string_heap_segment, const idx_t &v_offset, const bool &copied);

	//! Prevents the block with the given id from being added to the eviction queue
	void SetDestroyBufferUponUnpin(uint32_t block_id);
	//! Gets a shared pointer to the database instance if ColumnDataCollectionLifetime::DATABASE_INSTANCE
	shared_ptr<DatabaseInstance> GetDatabase() const;

private:
	void AllocateEmptyBlock(idx_t size);
	BufferHandle AllocateBlock(idx_t size);
	BufferHandle Pin(uint32_t block_id);

	bool HasBlocks() const {
		return !blocks.empty();
	}

private:
	void AllocateBuffer(idx_t size, uint32_t &block_id, uint32_t &offset, ChunkManagementState *chunk_state);
	void AllocateMemory(idx_t size, uint32_t &block_id, uint32_t &offset, ChunkManagementState *chunk_state);
	void AssignPointer(uint32_t &block_id, uint32_t &offset, data_ptr_t pointer);

private:
	ColumnDataAllocatorType type;
	union {
		//! The allocator object (if this is a IN_MEMORY_ALLOCATOR)
		Allocator *allocator;
		//! The buffer manager (if this is a BUFFER_MANAGER_ALLOCATOR)
		BufferManager *buffer_manager;
	} alloc;
	//! The set of blocks used by the column data collection
	vector<BlockMetaData> blocks;
	//! The set of allocated data
	vector<AllocatedData> allocated_data;
	//! Whether this ColumnDataAllocator is shared across ColumnDataCollections that allocate in parallel
	bool shared = false;
	//! Lock used in case this ColumnDataAllocator is shared across threads
	mutex lock;
	//! Total allocated size
	idx_t allocated_size = 0;
	//! Partition index (optional, if partitioned)
	optional_idx partition_index;
	//! Lifetime management for this allocator
	ManagedResultSet managed_result_set;
};

} // namespace duckdb
