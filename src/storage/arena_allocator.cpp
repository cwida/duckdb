#include "duckdb/storage/arena_allocator.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Arena Chunk
//===--------------------------------------------------------------------===//
ArenaChunk::ArenaChunk(Allocator &allocator, idx_t size) : current_position(0), maximum_size(size), prev(nullptr) {
	D_ASSERT(size > 0);
	data = allocator.Allocate(size);
}
ArenaChunk::~ArenaChunk() {
	ResetPosition(0);
	if (next) {
		auto current_next = std::move(next);
		while (current_next) {
			current_next = std::move(current_next->next);
		}
	}
}

//===--------------------------------------------------------------------===//
// Allocator Wrapper
//===--------------------------------------------------------------------===//
struct ArenaAllocatorData : public PrivateAllocatorData {
	explicit ArenaAllocatorData(ArenaAllocator &allocator) : allocator(allocator) {
		free_type = AllocatorFreeType::DOES_NOT_REQUIRE_FREE;
	}

	ArenaAllocator &allocator;
};

static data_ptr_t ArenaAllocatorAllocate(PrivateAllocatorData *private_data, idx_t size) {
	auto &allocator_data = private_data->Cast<ArenaAllocatorData>();
	return allocator_data.allocator.Allocate(size);
}

static void ArenaAllocatorFree(PrivateAllocatorData *, data_ptr_t, idx_t) {
	// nop
}

static data_ptr_t ArenaAllocateReallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size,
                                          idx_t size) {
	auto &allocator_data = private_data->Cast<ArenaAllocatorData>();
	return allocator_data.allocator.Reallocate(pointer, old_size, size);
}
//===--------------------------------------------------------------------===//
// Arena Allocator
//===--------------------------------------------------------------------===//
ArenaAllocator::ArenaAllocator(Allocator &allocator, idx_t initial_capacity)
    : allocator(allocator), initial_capacity(initial_capacity),
      arena_allocator(ArenaAllocatorAllocate, ArenaAllocatorFree, ArenaAllocateReallocate,
                      make_uniq<ArenaAllocatorData>(*this)) {
	head = nullptr;
	tail = nullptr;
}

ArenaAllocator::~ArenaAllocator() {
}

void ArenaAllocator::AllocateNewBlock(idx_t min_size) {
	idx_t capacity;
	// start off with either (1) initial capacity (if we have no block) or (2) capacity of the previous block
	if (!head) {
		capacity = initial_capacity;
	} else {
		capacity = head->maximum_size;
	}
	// capacity of the previous block can be bigger than the max capacity if we allocate len > max capacity
	// for new blocks - try to set it back to the max capacity
	if (capacity > ARENA_ALLOCATOR_MAX_CAPACITY) {
		capacity = ARENA_ALLOCATOR_MAX_CAPACITY;
	}
	// if we are below the max capacity - double the size of the block
	if (capacity < ARENA_ALLOCATOR_MAX_CAPACITY) {
		capacity *= 2;
	}
	// we double the size until we can fit `len`
	// this is generally only relevant if len is very large
	while (capacity < min_size) {
		capacity *= 2;
	}
	auto new_chunk = make_unsafe_uniq<ArenaChunk>(allocator, capacity);
	if (head) {
		head->prev = new_chunk.get();
		new_chunk->next = std::move(head);
	} else {
		tail = new_chunk.get();
	}
	head = std::move(new_chunk);
	allocated_size += capacity;
}

data_ptr_t ArenaAllocator::Reallocate(data_ptr_t pointer, idx_t old_size, idx_t size) {
	if (old_size == size) {
		// nothing to do
		return pointer;
	}

	if (head && old_size <= head->current_position) {
		const auto head_ptr = head->data.get() + head->current_position - old_size;
		int64_t current_position = NumericCast<int64_t>(head->current_position);
		int64_t diff = NumericCast<int64_t>(size) - NumericCast<int64_t>(old_size);
		if (pointer == head_ptr &&
		    (size < old_size || current_position + diff <= NumericCast<int64_t>(head->maximum_size))) {
			// passed pointer is the head pointer, and the diff fits on the current chunk
			if (size < old_size) {
				head->ResetPosition(NumericCast<idx_t>(current_position + diff));
			} else {
				head->current_position = NumericCast<idx_t>(current_position + diff);
			}
			return pointer;
		}
	}
	// The allocation may belong to an earlier chunk or another arena.
	auto result = Allocate(size);
	memcpy(result, pointer, MinValue(old_size, size));
	return result;
}

void ArenaAllocator::AlignNext() {
	if (head && !ValueIsAligned<idx_t>(head->current_position)) {
		// move the current position forward so that the next allocation is aligned
		head->current_position = AlignValue<idx_t>(head->current_position);
	}
}

data_ptr_t ArenaAllocator::AllocateAligned(idx_t size) {
	AlignNext();
	return Allocate(AlignValue<idx_t>(size));
}

data_ptr_t ArenaAllocator::ReallocateAligned(data_ptr_t pointer, idx_t old_size, idx_t size) {
	AlignNext();
	return Reallocate(pointer, old_size, AlignValue<idx_t>(size));
}

ArenaAllocator::Position ArenaAllocator::GetPosition() const {
	Position result;
	if (head) {
		result.chunk = head.get();
		result.current_position = head->current_position;
	}
	return result;
}

void ArenaAllocator::Rewind(const Position &position) {
	if (!position.chunk) {
		// nothing was allocated when the position was taken
		FreeAll();
		return;
	}
	// destroy the chunks that were allocated after the position was taken, newest first
	while (head.get() != position.chunk) {
		D_ASSERT(head && head->next);
		allocated_size -= head->maximum_size;
		auto older = std::move(head->next);
		head = std::move(older);
		head->prev = nullptr;
	}
	D_ASSERT(position.current_position <= head->current_position);
	head->ResetPosition(position.current_position);
}

void ArenaAllocator::FreeAll() {
	Reset();
	if (head) {
		head.reset();
		tail = nullptr;
	}
	allocated_size = 0;
}

void ArenaAllocator::Reset() {
	if (head) {
		// destroy all chunks except the current one
		if (head->next) {
			auto current_next = std::move(head->next);
			while (current_next) {
				current_next = std::move(current_next->next);
			}
		}
		tail = head.get();

		// reset the head
		head->ResetPosition(0);
		head->prev = nullptr;
	}
	allocated_size = 0;
}

void ArenaAllocator::Destroy() {
	head = nullptr;
	tail = nullptr;
	allocated_size = 0;
}

void ArenaAllocator::Move(ArenaAllocator &other) {
	D_ASSERT(!other.head);
	other.tail = tail;
	other.head = std::move(head);
	other.initial_capacity = initial_capacity;
	other.allocated_size = allocated_size;
	Destroy();
}

ArenaChunk *ArenaAllocator::GetHead() {
	return head.get();
}

ArenaChunk *ArenaAllocator::GetTail() {
	return tail;
}

bool ArenaAllocator::IsEmpty() const {
	return head == nullptr;
}

idx_t ArenaAllocator::SizeInBytes() const {
	idx_t total_size = 0;
	if (!IsEmpty()) {
		auto current = head.get();
		while (current != nullptr) {
			total_size += current->current_position;
			current = current->next.get();
		}
	}
	return total_size;
}

idx_t ArenaAllocator::AllocationSize() const {
	D_ASSERT(head || allocated_size == 0);
	return allocated_size;
}

} // namespace duckdb
