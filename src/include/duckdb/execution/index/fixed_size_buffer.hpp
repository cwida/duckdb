//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/fixed_size_buffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/block_packable.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/block_manager.hpp"

namespace duckdb {

class FixedSizeAllocator;
class PartialBlockManager;

//! A fixed-size buffer holds fixed-size segments of data. It lazily deserializes a buffer, if on-disk and not
//! in memory, and it only serializes dirty and non-written buffers to disk during serialization.
class FixedSizeBuffer : public BlockPackable {
	friend class FixedSizeAllocator;
	friend class SegmentHandle;

public:
	//! Constants for fast offset calculations in the bitmask
	static constexpr idx_t BASE[] = {0x00000000FFFFFFFF, 0x0000FFFF, 0x00FF, 0x0F, 0x3, 0x1};
	static constexpr uint8_t SHIFT[] = {32, 16, 8, 4, 2, 1};

public:
	//! Constructor for checkpoint metadata awaiting its packed backing buffer
	FixedSizeBuffer(BlockManager &block_manager, idx_t segment_count, idx_t allocation_size);
	//! Constructor for a new in-memory buffer
	explicit FixedSizeBuffer(BlockManager &block_manager, MemoryTag memory_tag);
	FixedSizeBuffer(BlockManager &block_manager, const idx_t segment_count, const idx_t allocation_size,
	                shared_ptr<BlockHandle> block_handle);
	//! Constructor for deserializing buffer metadata from disk
	FixedSizeBuffer(BlockManager &block_manager, const idx_t segment_count, const idx_t allocation_size,
	                const BlockPointer &block_pointer);

	~FixedSizeBuffer() override;

public:
	void Finalize(shared_ptr<BlockHandle> block_handle_p, uint32_t offset) override;

public:
	bool IsEmpty() const {
		return segment_count == 0;
	}

private:
	//! Returns a pointer to the buffer in memory, and calls Deserialize, if the buffer is not in memory.
	//! DEPRECATED. Use segment handles.
	data_ptr_t GetDeprecated(const bool dirty_p = true) {
		lock_guard<mutex> l(lock);
		if (!InMemory()) {
			LoadFromDisk();
		}
		if (dirty_p) {
			dirty = dirty_p;
		}
		return buffer_handle.GetDataMutable();
	}

	//! Returns true, if the buffer is in-memory
	bool InMemory() const {
		return buffer_handle.IsValid();
	}

	//! Returns true, if the block is on-disk
	bool OnDisk() const {
		return block_pointer.IsValid();
	}

	//! Returns true, if the block is modified or does not exist on-disk
	bool RequiresPersist() const {
		return dirty || !OnDisk();
	}

	//! Constructs a new FixedSizeBuffer based on the live FixedSizeBuffer, which is prepared for
	//! serialization without affecting the in-memory buffer.
	shared_ptr<FixedSizeBuffer> Persist(PartialBlockManager &partial_block_manager, const idx_t available_segments,
	                                    const idx_t segment_size, const idx_t bitmask_offset);

	//! Load a buffer from disk, if not in memory.
	void LoadFromDisk();
	//! Reserves a free slot in the buffer and returns the corresponding offset, marks the buffer as modified.
	uint32_t AllocateSegment(const idx_t bitmask_count, const idx_t available_segments);
	//! Frees the slot at the given offset in the current buffer.
	void FreeSegment(const idx_t offset, const idx_t available_segments);
	//! Sets the allocation size, if dirty
	void SetAllocationSize(const idx_t available_segments, const idx_t segment_size, const idx_t bitmask_offset);
	idx_t GetNewAllocationSize(const idx_t available_segments, const idx_t segment_size, const idx_t bitmask_offset);

private:
	//! Block manager of the database instance
	BlockManager &block_manager;

	//! The number of active segments.
	atomic<idx_t> readers;

	//! The number of allocated segments
	idx_t segment_count;
	//! The size of allocated memory in this buffer (necessary for copying while pinning)
	idx_t allocation_size;

	//! True: the in-memory buffer is no longer consistent with its optional copy on disk.
	bool dirty;
	//! True: can be vacuumed after the vacuum operation.
	bool vacuum;
	//! True: has been loaded from disk.
	bool loaded;

	//! Partial block id and offset
	BlockPointer block_pointer;
	//! The buffer handle of the in-memory buffer
	BufferHandle buffer_handle;
	//! The block handle of the on-disk buffer
	shared_ptr<BlockHandle> block_handle;
	//! The lock for this fixed size buffer handle
	mutex lock;
};

class SegmentHandle {
public:
	SegmentHandle() = delete;
	SegmentHandle(FixedSizeBuffer &buffer_p, const idx_t offset);

	~SegmentHandle() {
		if (!buffer_ptr) {
			return;
		}
		buffer_ptr->readers--;
		buffer_ptr = nullptr;
		ptr = nullptr;

		// FIXME: Enable unpinning buffers with zero readers while preventing oscillation.
		// FIXME: loaded must be set to true.
	}

	SegmentHandle(const SegmentHandle &) = delete;
	SegmentHandle &operator=(const SegmentHandle &) = delete;

	SegmentHandle(SegmentHandle &&other) noexcept : buffer_ptr(other.buffer_ptr), ptr(other.ptr) {
		other.buffer_ptr = nullptr;
		other.ptr = nullptr;
	}
	SegmentHandle &operator=(SegmentHandle &&other) noexcept {
		if (this != &other) {
			// Decrement reader count for existing handle if it exists
			if (buffer_ptr) {
				buffer_ptr->readers--;
			}
			buffer_ptr = other.buffer_ptr;
			ptr = other.ptr;
			other.buffer_ptr = nullptr;
			other.ptr = nullptr;
		}
		return *this;
	}

public:
	template <class T>
	const T &GetRef() const {
		return *reinterpret_cast<const T *>(ptr);
	}

	template <class T>
	T &GetRef() {
		return *reinterpret_cast<T *>(ptr);
	}

	template <class T = data_t>
	T *GetPtr() {
		return reinterpret_cast<T *>(ptr);
	}

	template <class T = data_t>
	const T *GetPtr() const {
		return reinterpret_cast<const T *>(ptr);
	}

	void MarkModified() {
		lock_guard<mutex> l(buffer_ptr->lock);
		buffer_ptr->dirty = true;
	}

private:
	optional_ptr<FixedSizeBuffer> buffer_ptr;
	data_ptr_t ptr;
};

} // namespace duckdb
