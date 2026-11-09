//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/row/tuple_data_segment.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/perfect_map_set.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"

namespace duckdb {

class TupleDataAllocator;
class TupleDataLayout;
struct TupleDataSegment;

struct TupleDataChunkPart {
public:
	explicit TupleDataChunkPart(mutex &lock);

	//! Disable copy constructors
	TupleDataChunkPart(const TupleDataChunkPart &other) = delete;
	TupleDataChunkPart &operator=(const TupleDataChunkPart &) = delete;

	//! Enable move constructors
	TupleDataChunkPart(TupleDataChunkPart &&other) noexcept = default;
	TupleDataChunkPart &operator=(TupleDataChunkPart &&) noexcept = default;

public:
	//! Mark heap as empty
	void SetHeapEmpty();

public:
	//! Index/offset of the row block
	uint32_t row_block_index;
	uint32_t row_block_offset;
	//! Pointer/index/offset of the heap block
	uint32_t heap_block_index;
	uint32_t heap_block_offset;
	data_ptr_t base_heap_ptr;
	//! Total heap size for this chunk part
	idx_t total_heap_size;
	//! Tuple count for this chunk part
	uint32_t count;
	//! Lock for recomputing heap pointers
	reference<mutex> lock;

private:
	//! Marker for empty heaps
	static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(-1);
};

class ContinuousIdSet {
public:
	ContinuousIdSet() : min_id(INVALID_INDEX), max_id(INVALID_INDEX) {
	}

public:
	void Insert(const uint32_t &block_id) {
		if (Empty()) {
			min_id = block_id;
			max_id = block_id;
		} else {
			min_id = MinValue(min_id, block_id);
			max_id = MaxValue(max_id, block_id);
		}
	}

	bool Contains(const uint32_t &block_id) const {
		if (Empty()) {
			return false;
		}
		return block_id >= min_id && block_id <= max_id;
	}

	bool Empty() const {
		return min_id == INVALID_INDEX;
	}

	uint32_t Start() const {
		D_ASSERT(!Empty());
		return min_id;
	}

	uint32_t End() const {
		D_ASSERT(!Empty());
		return max_id + 1;
	}

	uint32_t Size() const {
		D_ASSERT(!Empty());
		return End() - Start();
	}

	void DecrementMax() {
		D_ASSERT(!Empty());
		D_ASSERT(Size() > 1);
		max_id--;
	}

private:
	static constexpr uint32_t INVALID_INDEX = static_cast<uint32_t>(-1);
	uint32_t min_id;
	uint32_t max_id;
};

struct TupleDataChunk {
public:
	explicit TupleDataChunk(mutex &lock_p);

	//! Disable copy constructors
	TupleDataChunk(const TupleDataChunk &other) = delete;
	TupleDataChunk &operator=(const TupleDataChunk &) = delete;

	//! Enable move constructors
	TupleDataChunk(TupleDataChunk &&other) noexcept;
	TupleDataChunk &operator=(TupleDataChunk &&) noexcept;

	//! Add a part to this chunk
	TupleDataChunkPart &AddPart(TupleDataSegment &segment, unsafe_arena_ptr<TupleDataChunkPart> part_ptr);
	//! Tries to merge the last chunk part into the second-to-last one
	void MergeLastChunkPart(TupleDataSegment &segment);
	//! Verify counts of the parts in this chunk
	void Verify(const TupleDataSegment &segment) const;

public:
	//! The parts of this chunk
	ContinuousIdSet part_ids;

	//! The row block ids referenced by the chunk
	ContinuousIdSet row_block_ids;
	//! The heap block ids referenced by the chunk
	ContinuousIdSet heap_block_ids;
	//! Tuple count for this chunk
	idx_t count;
	//! Lock for recomputing heap pointers
	reference<mutex> lock;
};

struct TupleDataSegment {
	friend struct TupleDataChunkPart;
	friend struct TupleDataChunk;

public:
	explicit TupleDataSegment(shared_ptr<TupleDataAllocator> allocator);

	~TupleDataSegment();

public:
	//! The number of chunks in this segment
	idx_t ChunkCount() const;
	//! The size (in bytes) of this segment
	idx_t SizeInBytes() const;
	//! The heap bytes used by the rows of a chunk (fixed-size layouts have no heap)
	idx_t ChunkHeapSize(idx_t chunk_idx) const;
	//! Unpins all held pins
	void Unpin();
	//! Reset this segment for reuse, clearing chunk metadata while keeping the segment object alive
	void Reset();

	//! Verify counts of the chunks in this segment
	void Verify() const;
	//! Verify that all blocks in this segment are pinned
	void VerifyEverythingPinned() const;

public:
	//! The allocator and layout for this segment
	shared_ptr<TupleDataAllocator> allocator;
	const TupleDataLayout &layout;
	//! The chunks of this segment
	unsafe_vector<unsafe_arena_ptr<TupleDataChunk>> chunks;
	//! The chunk parts of this segment
	unsafe_vector<unsafe_arena_ptr<TupleDataChunkPart>> chunk_parts;
	//! The tuple count of this segment
	idx_t count;
	//! The data size of this segment
	idx_t data_size;

	//! Lock for modifying pinned_handles
	mutex pinned_handles_lock;
	//! Where handles to row blocks will be stored with TupleDataPinProperties::KEEP_EVERYTHING_PINNED
	unsafe_arena_vector<BufferHandle> pinned_row_handles;
	//! Where handles to heap blocks will be stored with TupleDataPinProperties::KEEP_EVERYTHING_PINNED
	unsafe_arena_vector<BufferHandle> pinned_heap_handles;
};

} // namespace duckdb
