#include "duckdb/execution/index/fixed_size_buffer.hpp"

#include "duckdb/common/typedefs.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// PartialBlockForIndex
//===--------------------------------------------------------------------===//

PartialBlockForIndex::PartialBlockForIndex(PartialBlockState state, BlockManager &block_manager,
                                           const shared_ptr<BlockHandle> &block_handle)
    : PartialBlock(state, block_manager, block_handle) {
}

void PartialBlockForIndex::Flush(QueryContext context, const idx_t free_space_left) {
	FlushInternal(free_space_left);
	block_handle = block_manager.ConvertToPersistent(context, state.block_id, std::move(block_handle));
	Clear();
}

void PartialBlockForIndex::Merge(PartialBlock &other, idx_t offset, idx_t other_size) {
	throw InternalException("no merge for PartialBlockForIndex");
}

void PartialBlockForIndex::Clear() {
	block_handle.reset();
}

//===--------------------------------------------------------------------===//
// FixedSizeBuffer
//===--------------------------------------------------------------------===//

constexpr idx_t FixedSizeBuffer::BASE[];
constexpr uint8_t FixedSizeBuffer::SHIFT[];

FixedSizeBuffer::FixedSizeBuffer(BlockManager &block_manager, MemoryTag memory_tag)
    : block_manager(block_manager), readers(0), segment_count(0), allocation_size(0), dirty(false), vacuum(false),
      loaded(false), block_pointer(), block_handle(nullptr) {
	auto &buffer_manager = block_manager.buffer_manager;
	buffer_handle = buffer_manager.Allocate(memory_tag, &block_manager, false);
	block_handle = buffer_handle.GetBlockHandle();

	// Zero-initialize the buffer as it might get serialized to storage.
	auto block_size = block_manager.GetBlockSize();
	memset(buffer_handle.GetDataMutable(), 0, block_size);
}

FixedSizeBuffer::FixedSizeBuffer(BlockManager &block_manager, const idx_t segment_count, const idx_t allocation_size,
                                 shared_ptr<BlockHandle> block_handle_p)
    : block_manager(block_manager), readers(0), segment_count(segment_count), allocation_size(allocation_size),
      dirty(false), vacuum(false), loaded(false), block_pointer(), block_handle(std::move(block_handle_p)) {
	D_ASSERT(block_handle);
	buffer_handle = block_manager.buffer_manager.Pin(block_handle);
	D_ASSERT(buffer_handle.IsValid());
}

FixedSizeBuffer::FixedSizeBuffer(BlockManager &block_manager, const idx_t segment_count, const idx_t allocation_size,
                                 const BlockPointer &block_pointer)
    : block_manager(block_manager), readers(0), segment_count(segment_count), allocation_size(allocation_size),
      dirty(false), vacuum(false), loaded(false), block_pointer(block_pointer) {
	D_ASSERT(block_pointer.IsValid());
	block_handle = block_manager.RegisterBlock(block_pointer.block_id);
	D_ASSERT(block_handle->BlockId() < MAXIMUM_BLOCK);
}

FixedSizeBuffer::~FixedSizeBuffer() {
	lock_guard<mutex> l(lock);
	D_ASSERT(readers == 0);

	if (InMemory()) {
		// we can have multiple readers on a pinned block, and unpinning the buffer handle
		// decrements the reader count on the underlying block handle (Destroy() unpins)
		buffer_handle.Destroy();
	}
	if (OnDisk()) {
		// marking a block as modified decreases the reference count of multi-use blocks
		// this must not throw out of a destructor, e.g., if the block ids of a corrupt file overlap
		try {
			block_manager.MarkBlockAsModified(block_pointer.block_id);
		} catch (...) { // NOLINT
		}
	}
}

void FixedSizeBuffer::DisownBlock() {
	lock_guard<mutex> l(lock);
	block_pointer = BlockPointer();
}

void FixedSizeBuffer::Serialize(PartialBlockManager &partial_block_manager, const idx_t available_segments,
                                const idx_t segment_size, const idx_t bitmask_offset) {
	D_ASSERT(readers == 0);

	// Early-out, if the block is already on disk and not in memory.
	if (!InMemory()) {
		if (!OnDisk() || dirty) {
			throw InternalException("invalid or missing buffer in FixedSizeAllocator");
		}
		return;
	}

	// Early-out, if the buffer is already on disk and not dirty.
	if (!dirty && OnDisk()) {
		return;
	}

	// Adjust the allocation size.
	D_ASSERT(segment_count != 0);
	SetAllocationSize(available_segments, segment_size, bitmask_offset);

	// The buffer is in memory.
	D_ASSERT(InMemory());
	if (OnDisk()) {
		// We copied it onto a new buffer when loading from disk.
		block_manager.MarkBlockAsModified(block_pointer.block_id);
	}

	// Write the changes.
	// First, we get a partial block allocation.
	PartialBlockAllocation allocation =
	    partial_block_manager.GetBlockAllocation(NumericCast<uint32_t>(allocation_size));
	block_pointer.block_id = allocation.state.block_id;
	block_pointer.offset = allocation.state.offset;

	auto &buffer_manager = block_manager.buffer_manager;

	if (allocation.partial_block) {
		// There is space, so we copy to an existing partial block.
		D_ASSERT(block_pointer.offset > 0);
		auto &p_block_for_index = allocation.partial_block->Cast<PartialBlockForIndex>();
		auto dst_handle = buffer_manager.Pin(p_block_for_index.block_handle);
		memcpy(dst_handle.GetDataMutable() + block_pointer.offset, buffer_handle.Ptr(), allocation_size);

	} else {
		// No partial block available, so we create a new partial block.
		D_ASSERT(block_handle);
		D_ASSERT(!block_pointer.offset);
		auto p_block_for_index = make_uniq<PartialBlockForIndex>(allocation.state, block_manager, block_handle);
		allocation.partial_block = std::move(p_block_for_index);
	}

	// We are done with this buffer.
	// To use the fixed-size buffer again, we need to re-load it from disk.
	buffer_handle.Destroy();

	// Register the partial block and the block handle.
	partial_block_manager.RegisterPartialBlock(std::move(allocation));

	block_handle = block_manager.RegisterBlock(block_pointer.block_id);
	D_ASSERT(block_handle->BlockId() < MAXIMUM_BLOCK);

	// We persisted any changes, so the fixed-size buffer is no longer dirty.
	dirty = false;
}

void FixedSizeBuffer::LoadFromDisk() {
	D_ASSERT(block_pointer.IsValid());
	D_ASSERT(block_handle && block_handle->BlockId() < MAXIMUM_BLOCK);
	D_ASSERT(!dirty);

	// Pin the partial block.
	auto &buffer_manager = block_manager.buffer_manager;
	auto pinned_buffer_handle = buffer_manager.Pin(block_handle);

	// block_pointer.offset and allocation_size come straight from the index's on-disk metadata - verify the read
	// stays within the (fixed-size) source and destination blocks before copying out of either of them.
	auto block_size = block_manager.GetBlockSize();
	if (allocation_size > block_size || block_pointer.offset > block_size - allocation_size) {
		throw DataCorruptionException("Corrupted index buffer: offset %llu with allocation size %llu exceeds the "
		                              "block size %llu",
		                              block_pointer.offset, allocation_size, block_size);
	}

	// Copy the (partial) data into a new (not yet disk-backed) buffer handle.
	shared_ptr<BlockHandle> new_block_handle;
	auto new_buffer_handle = buffer_manager.Allocate(MemoryTag::ART_INDEX, &block_manager, false);
	new_block_handle = new_buffer_handle.GetBlockHandle();
	memcpy(new_buffer_handle.GetDataMutable(), pinned_buffer_handle.Ptr() + block_pointer.offset, allocation_size);
	memset(new_buffer_handle.GetDataMutable() + allocation_size, 0, block_size - allocation_size);

	buffer_handle = std::move(new_buffer_handle);
	block_handle = std::move(new_block_handle);
}

uint32_t FixedSizeBuffer::AllocateSegment(const idx_t bitmask_count, const idx_t available_segments) {
	D_ASSERT(segment_count < available_segments);

	// Get a handle to the buffer's validity mask (offset 0).
	SegmentHandle handle(*this, 0);
	handle.MarkModified();
	const auto bitmask_ptr = handle.GetPtr<validity_t>();

	ValidityMask mask(bitmask_ptr, available_segments);
	const auto data = mask.GetData();

	// fills up a buffer sequentially before searching for free bits
	if (mask.RowIsValid(segment_count)) {
		const auto offset = segment_count;
		mask.SetInvalid(offset);
		segment_count++;
		return UnsafeNumericCast<uint32_t>(offset);
	}

	for (idx_t entry_idx = 0; entry_idx < bitmask_count; entry_idx++) {
		// get an entry with free bits
		if (data[entry_idx] == 0) {
			continue;
		}

		// find the position of the free bit
		auto entry = data[entry_idx];
		idx_t first_valid_bit = 0;

		// this loop finds the position of the rightmost set bit in entry and stores it
		// in first_valid_bit
		for (idx_t i = 0; i < 6; i++) {
			// set the left half of the bits of this level to zero and test if the entry is still not zero
			if (entry & BASE[i]) {
				// first valid bit is in the rightmost s[i] bits
				// permanently set the left half of the bits to zero
				entry &= BASE[i];
			} else {
				// first valid bit is in the leftmost s[i] bits
				// shift by s[i] for the next iteration and add s[i] to the position of the rightmost set bit
				entry >>= SHIFT[i];
				first_valid_bit += SHIFT[i];
			}
		}
		D_ASSERT(entry);

		const auto offset = entry_idx * sizeof(validity_t) * 8 + first_valid_bit;
		D_ASSERT(mask.RowIsValid(offset));
		mask.SetInvalid(offset);
		segment_count++;
		return UnsafeNumericCast<uint32_t>(offset);
	}

	throw InternalException("Invalid bitmask for FixedSizeAllocator");
}

void FixedSizeBuffer::FreeSegment(const idx_t offset, const idx_t available_segments) {
	D_ASSERT(segment_count > 0);
	D_ASSERT(offset < available_segments);

	SegmentHandle handle(*this, 0);
	handle.MarkModified();
	const auto bitmask_ptr = handle.GetPtr<validity_t>();

	ValidityMask mask(bitmask_ptr, available_segments);
	D_ASSERT(!mask.RowIsValid(offset));
	mask.SetValid(offset);

	segment_count--;
}

void FixedSizeBuffer::SetAllocationSize(const idx_t available_segments, const idx_t segment_size,
                                        const idx_t bitmask_offset) {
	if (!dirty) {
		return;
	}

	// We traverse from the back. A binary search would be faster.
	// However, buffers are often (almost) full, so the overhead is acceptable.

	// Get a handle to the buffer's validity mask (offset 0).
	SegmentHandle handle(*this, 0);
	const auto bitmask_ptr = handle.GetPtr<validity_t>();
	const ValidityMask mask(bitmask_ptr, available_segments);

	auto max_offset = available_segments;
	for (idx_t i = available_segments; i > 0; i--) {
		if (!mask.RowIsValid(i - 1)) {
			max_offset = i;
			break;
		}
	}
	allocation_size = max_offset * segment_size + bitmask_offset;
}

SegmentHandle::SegmentHandle(FixedSizeBuffer &buffer_p, const idx_t offset) : buffer_ptr(buffer_p) {
	lock_guard<mutex> l(buffer_ptr->lock);

	if (!buffer_ptr->InMemory() && !buffer_ptr->loaded) {
		buffer_ptr->LoadFromDisk();
	}
	if (!buffer_ptr->InMemory() && buffer_ptr->loaded) {
		buffer_ptr->block_manager.buffer_manager.Pin(buffer_ptr->block_handle);
	}

	ptr = buffer_ptr->buffer_handle.GetDataMutable() + offset;
	buffer_ptr->readers++;
}

} // namespace duckdb
