//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/row/tuple_data_allocator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"
#include "duckdb/common/types/row/tuple_data_states.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"

namespace duckdb {

struct TupleDataSegment;
struct TupleDataChunk;
struct TupleDataChunkPart;
class ContinuousIdSet;

struct TupleDataBlock {
public:
	TupleDataBlock(BufferManager &buffer_manager, MemoryTag tag, idx_t capacity_p);
	TupleDataBlock(shared_ptr<BlockHandle> handle, idx_t capacity_p);

	//! Disable copy constructors
	TupleDataBlock(const TupleDataBlock &other) = delete;
	TupleDataBlock &operator=(const TupleDataBlock &) = delete;

	//! Enable move constructors
	TupleDataBlock(TupleDataBlock &&other) noexcept;
	TupleDataBlock &operator=(TupleDataBlock &&) noexcept;

public:
	//! Remaining capacity (in bytes)
	idx_t RemainingCapacity() const {
		D_ASSERT(size <= capacity);
		return capacity - size;
	}

	//! Remaining capacity (in rows)
	idx_t RemainingCapacity(idx_t row_width) const {
		return RemainingCapacity() / row_width;
	}

public:
	//! The underlying row block
	shared_ptr<BlockHandle> handle;
	//! Capacity (in bytes)
	idx_t capacity;
	//! Occupied size (in bytes)
	idx_t size;
};

class TupleDataAllocator {
public:
	TupleDataAllocator(BufferManager &buffer_manager, shared_ptr<TupleDataLayout> layout_ptr, MemoryTag tag,
	                   shared_ptr<ArenaAllocator> stl_allocator, QueryContext context = QueryContext());
	TupleDataAllocator(TupleDataAllocator &allocator);

	~TupleDataAllocator();

	//! Get the buffer manager
	BufferManager &GetBufferManager();
	//! Get the query context used to attribute eviction/spill I/O
	QueryContext GetContext() const {
		return context;
	}
	//! Get the buffer allocator
	Allocator &GetAllocator();
	//! Get the STL allocator
	ArenaAllocator &GetStlAllocator();
	//! Get the layout
	shared_ptr<TupleDataLayout> GetLayoutPtr() const;
	const TupleDataLayout &GetLayout() const;
	//! Number of row blocks
	idx_t RowBlockCount() const;
	//! Number of heap blocks
	idx_t HeapBlockCount() const;
	//! Cached allocation size of retained row and heap blocks
	idx_t GetBlockAllocationSize() const;
	//! Sets the partition index of this tuple data allocator
	void SetPartitionIndex(idx_t index);
	//! Gets the partition index of this tuple data allocator
	idx_t GetPartitionIndex() const;

public:
	//! Builds out the chunks for next append, given the metadata in the append state
	void Build(TupleDataSegment &segment, TupleDataPinState &pin_state, TupleDataChunkState &chunk_state,
	           const idx_t append_offset, const idx_t append_count);
	bool BuildFastPath(TupleDataSegment &segment, TupleDataPinState &pin_state, TupleDataChunkState &chunk_state,
	                   const idx_t append_offset, const idx_t append_count);
	//! Initializes a chunk, making its pointers valid
	void InitializeChunkState(TupleDataSegment &segment, TupleDataPinState &pin_state, TupleDataChunkState &chunk_state,
	                          idx_t chunk_idx, bool init_heap,
	                          optional_ptr<SortKeyPayloadState> sort_key_payload_state = nullptr);
	static void RecomputeHeapPointers(Vector &old_heap_ptrs, const SelectionVector &old_heap_sel,
	                                  const data_ptr_t row_locations[], Vector &new_heap_ptrs, const idx_t offset,
	                                  const idx_t count, const TupleDataLayout &layout, const idx_t base_col_offset);
	static void FindHeapPointers(TupleDataChunkState &chunk_state, SelectionVector &not_found, idx_t &not_found_count,
	                             const TupleDataLayout &layout, const idx_t base_col_offset);
	//! Releases or stores any handles in the management state that are no longer required
	void ReleaseOrStoreHandles(TupleDataPinState &state, TupleDataSegment &segment, TupleDataChunk &chunk,
	                           bool release_heap);
	//! Releases or stores ALL handles in the management state
	void ReleaseOrStoreHandles(TupleDataPinState &state, TupleDataSegment &segment);
	//! Sets 'can_destroy' to true for all blocks so they aren't added to the eviction queue
	void SetDestroyBufferUponUnpin();
	//! Release all row/heap blocks and reset the allocator for reuse without a heap allocation
	void Reset();
	//! Destroy the blocks between the given indices
	void DestroyRowBlocks(idx_t row_block_begin, idx_t row_block_end);
	void DestroyHeapBlocks(idx_t heap_block_begin, idx_t heap_block_end);

private:
	//! Builds out a single part (grabs the lock)
	unsafe_arena_ptr<TupleDataChunkPart> BuildChunkPart(TupleDataSegment &segment, TupleDataPinState &pin_state,
	                                                    TupleDataChunkState &chunk_state, const idx_t append_offset,
	                                                    const idx_t append_count, TupleDataChunk &chunk);
	//! Internal function for InitializeChunkState
	void InitializeChunkStateInternal(TupleDataPinState &pin_state, TupleDataChunkState &chunk_state, idx_t offset,
	                                  bool recompute, bool init_heap_pointers, bool init_heap_sizes,
	                                  unsafe_vector<reference<TupleDataChunkPart>> &parts,
	                                  optional_ptr<SortKeyPayloadState> sort_key_payload_state = nullptr);
	//! Internal function for ReleaseOrStoreHandles
	void ReleaseOrStoreHandlesInternal(TupleDataSegment &segment, unsafe_arena_vector<BufferHandle> &pinned_row_handles,
	                                   buffer_handle_map_t &handles, const ContinuousIdSet &block_ids,
	                                   unsafe_arena_vector<TupleDataBlock> &blocks, TupleDataPinProperties properties);
	//! Create a row/heap block, store the handle in pin_state so the block stays pinned
	void CreateRowBlock(TupleDataSegment &segment, TupleDataPinState &pin_state);
	void CreateHeapBlock(TupleDataSegment &segment, TupleDataPinState &pin_state, idx_t size);
	//! Pins the given row block
	BufferHandle &PinRowBlock(TupleDataPinState &state, const TupleDataChunkPart &part);
	//! Pins the given heap block
	BufferHandle &PinHeapBlock(TupleDataPinState &state, const TupleDataChunkPart &part);
	//! Gets the pointer to the rows for the given chunk part
	data_ptr_t GetRowPointer(TupleDataPinState &state, const TupleDataChunkPart &part);
	//! Gets the base pointer to the heap for the given chunk part
	data_ptr_t GetBaseHeapPointer(TupleDataPinState &state, const TupleDataChunkPart &part);

private:
	//! Shared allocator for STL allocations
	shared_ptr<ArenaAllocator> stl_allocator;
	//! The buffer manager
	BufferManager &buffer_manager;
	//! The query context, used to attribute eviction/spill I/O to the query
	QueryContext context;
	//! The layout of the data
	shared_ptr<TupleDataLayout> layout_ptr;
	const TupleDataLayout &layout;
	//! Memory tag (for keeping track what the allocated memory belongs to)
	const MemoryTag tag;
	//! Partition index (optional, if partitioned)
	optional_idx partition_index;
	//! Blocks storing the fixed-size rows
	unsafe_arena_vector<TupleDataBlock> row_blocks;
	//! Blocks storing the variable-size data of the fixed-size rows (e.g., string, list)
	unsafe_arena_vector<TupleDataBlock> heap_blocks;
	//! Blocks can be released by parallel scans of separate segments
	atomic<idx_t> block_allocation_size {0};
};

} // namespace duckdb
