#include "duckdb/common/types/row/tuple_data_segment.hpp"

#include "duckdb/common/types/row/tuple_data_allocator.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"

namespace duckdb {

TupleDataChunkPart::TupleDataChunkPart(mutex &lock_p) : lock(lock_p) {
}

void TupleDataChunkPart::SetHeapEmpty() {
	heap_block_index = INVALID_INDEX;
	heap_block_offset = INVALID_INDEX;
	total_heap_size = 0;
	base_heap_ptr = nullptr;
}

TupleDataChunk::TupleDataChunk(mutex &lock_p) : count(0), lock(lock_p) {
}

static inline void SwapTupleDataChunk(TupleDataChunk &a, TupleDataChunk &b) noexcept {
	std::swap(a.part_ids, b.part_ids);
	std::swap(a.row_block_ids, b.row_block_ids);
	std::swap(a.heap_block_ids, b.heap_block_ids);
	std::swap(a.count, b.count);
	std::swap(a.lock, b.lock);
}

TupleDataChunk::TupleDataChunk(TupleDataChunk &&other) noexcept : count(0), lock(other.lock) {
	SwapTupleDataChunk(*this, other);
}

TupleDataChunk &TupleDataChunk::operator=(TupleDataChunk &&other) noexcept {
	SwapTupleDataChunk(*this, other);
	return *this;
}

TupleDataChunkPart &TupleDataChunk::AddPart(TupleDataSegment &segment, unsafe_arena_ptr<TupleDataChunkPart> part_ptr) {
	auto &part = *part_ptr;
	count += part.count;
	row_block_ids.Insert(part.row_block_index);
	if (!segment.layout.AllConstant() && part.total_heap_size > 0) {
		heap_block_ids.Insert(part.heap_block_index);
	}
	part.lock = lock;
	part_ids.Insert(UnsafeNumericCast<uint32_t>(segment.chunk_parts.size()));
	segment.chunk_parts.emplace_back(std::move(part_ptr));
	return part;
}

void TupleDataChunk::Verify(const TupleDataSegment &segment) const {
#ifdef D_ASSERT_IS_ENABLED
	idx_t total_count = 0;
	for (auto part_id = part_ids.Start(); part_id < part_ids.End(); part_id++) {
		total_count += segment.chunk_parts[part_id]->count;
	}
	D_ASSERT(this->count == total_count);
	D_ASSERT(this->count <= STANDARD_VECTOR_SIZE);
#endif
}

void TupleDataChunk::MergeLastChunkPart(TupleDataSegment &segment) {
	if (part_ids.Size() < 2) {
		return;
	}

	auto &second_to_last = *segment.chunk_parts[part_ids.End() - 2];
	auto &last = *segment.chunk_parts[part_ids.End() - 1];

	auto rows_align =
	    last.row_block_index == second_to_last.row_block_index &&
	    last.row_block_offset == second_to_last.row_block_offset + second_to_last.count * segment.layout.GetRowWidth();

	if (!rows_align) { // If rows don't align we can never merge
		return;
	}

	if (segment.layout.AllConstant()) { // No heap and rows align - merge
		second_to_last.count += last.count;
		if (segment.chunk_parts.size() == part_ids.End()) {
			// Can only remove if the part we're merging was the last added chunk part
			// If not, we just leave it there (no chunk will reference it anyway)
			segment.chunk_parts.pop_back();
		}
		part_ids.DecrementMax();
		return;
	}

	if (last.heap_block_index == second_to_last.heap_block_index &&
	    last.heap_block_offset == second_to_last.heap_block_index + second_to_last.total_heap_size &&
	    last.base_heap_ptr == second_to_last.base_heap_ptr) { // There is a heap and it aligns - merge
		second_to_last.total_heap_size += last.total_heap_size;
		second_to_last.count += last.count;
		if (segment.chunk_parts.size() == part_ids.End()) {
			segment.chunk_parts.pop_back(); // Same as above
		}
		part_ids.DecrementMax();
	}
}

TupleDataSegment::TupleDataSegment(shared_ptr<TupleDataAllocator> allocator_p)
    : allocator(std::move(allocator_p)), layout(allocator->GetLayout()), count(0), data_size(0),
      pinned_row_handles(allocator->GetStlAllocator()), pinned_heap_handles(allocator->GetStlAllocator()) {
}

TupleDataSegment::~TupleDataSegment() {
	if (allocator) {
		allocator->SetDestroyBufferUponUnpin(); // Prevent blocks from being added to eviction queue
	}
	// Avoid triggering the mutex init slow-path (on macOS, first lock on a fresh mutex is expensive).
	// In the destructor we are the sole owner, so checking without the lock is safe.
	if (!pinned_row_handles.empty() || !pinned_heap_handles.empty()) {
		lock_guard<mutex> guard(pinned_handles_lock);
		pinned_row_handles.clear();
		pinned_heap_handles.clear();
	}
}

void TupleDataSegment::Reset() {
	count = 0;
	data_size = 0;
	// Release any remaining pin handles before clearing. For UNPIN_AFTER_DONE, pinned_row_handles /
	// pinned_heap_handles may be non-empty due to resize-only calls in CreateRowBlock / CreateHeapBlock
	// (actual BufferHandles are in the TupleDataPinState, not here).  Unpin() safely clears them.
	Unpin();
	// chunk/chunk_part destructors are trivial (only POD fields), so clear is cheap.
	chunks.clear();
	chunk_parts.clear();
}

idx_t TupleDataSegment::ChunkCount() const {
	return chunks.size();
}

idx_t TupleDataSegment::SizeInBytes() const {
	return data_size;
}

idx_t TupleDataSegment::ChunkHeapSize(idx_t chunk_idx) const {
	D_ASSERT(chunk_idx < chunks.size());
	if (layout.AllConstant()) {
		return 0;
	}
	const auto &chunk = *chunks[chunk_idx];
	idx_t result = 0;
	for (auto part_id = chunk.part_ids.Start(); part_id < chunk.part_ids.End(); part_id++) {
		result += chunk_parts[part_id]->total_heap_size;
	}
	return result;
}

void TupleDataSegment::Unpin() {
	lock_guard<mutex> guard(pinned_handles_lock);
	pinned_row_handles.clear();
	pinned_heap_handles.clear();
}

void TupleDataSegment::Verify() const {
#ifdef D_ASSERT_IS_ENABLED
	const auto &allocator_layout = allocator->GetLayout();

	idx_t total_count = 0;
	idx_t total_size = 0;
	for (const auto &chunk_ptr : chunks) {
		const auto &chunk = *chunk_ptr;
		chunk.Verify(*this);
		total_count += chunk.count;

		total_size += chunk.count * allocator_layout.GetRowWidth();
		if (!allocator_layout.AllConstant()) {
			for (auto part_id = chunk.part_ids.Start(); part_id < chunk.part_ids.End(); part_id++) {
				total_size += chunk_parts[part_id]->total_heap_size;
			}
		}
	}
	D_ASSERT(total_count == this->count);
	D_ASSERT(total_size == this->data_size);
#endif
}

void TupleDataSegment::VerifyEverythingPinned() const {
#ifdef D_ASSERT_IS_ENABLED
	D_ASSERT(pinned_row_handles.size() == allocator->RowBlockCount());
	D_ASSERT(pinned_heap_handles.size() == allocator->HeapBlockCount());
#endif
}

} // namespace duckdb
