#include "duckdb/transaction/undo_buffer_allocator.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

UndoBufferEntry::~UndoBufferEntry() {
	if (next) {
		auto current_next = std::move(next);
		while (current_next) {
			current_next = std::move(current_next->next);
		}
	}
}
UndoBufferPointer UndoBufferReference::GetBufferPointer() {
	return UndoBufferPointer(*entry, position);
}

UndoBufferReference UndoBufferPointer::Pin() const {
	if (!entry) {
		throw InternalException("UndoBufferPointer::Pin called but no entry was found");
	}
	D_ASSERT(entry->capacity >= position);
	auto handle = entry->buffer_manager.Pin(entry->block);
	return UndoBufferReference(*entry, std::move(handle), position);
}

UndoBufferAllocator::UndoBufferAllocator(BufferManager &buffer_manager, bool hold_head_pin_p)
    : buffer_manager(buffer_manager), hold_head_pin(hold_head_pin_p) {
}

UndoBufferReference UndoBufferAllocator::Allocate(idx_t alloc_len) {
	// Unpins the previous head after the lock is released
	BufferHandle previous_head_pin;
	lock_guard<mutex> guard(lock);
	D_ASSERT(!head || head->position <= head->capacity);
	BufferHandle handle;
	if (!head || head->position + alloc_len > head->capacity) {
		// no space in current head - allocate a new block
		auto block_size = buffer_manager.GetBlockSize();
		;
		idx_t capacity;
		if (!head && alloc_len <= 4096) {
			capacity = 4096;
		} else {
			capacity = block_size;
		}
		if (capacity < alloc_len) {
			capacity = NextPowerOfTwo(alloc_len);
		}
		auto entry = make_uniq<UndoBufferEntry>(buffer_manager);
		if (capacity < block_size) {
			entry->block = buffer_manager.RegisterSmallMemory(MemoryTag::TRANSACTION, capacity);
			handle = buffer_manager.Pin(entry->block);
		} else {
			handle = buffer_manager.Allocate(MemoryTag::TRANSACTION, capacity, false);
			entry->block = handle.GetBlockHandle();
		}
		entry->capacity = capacity;
		entry->position = 0;
		// add block to the chain
		if (head) {
			head->prev = entry.get();
			entry->next = std::move(head);
		} else {
			tail = entry.get();
		}
		head = std::move(entry);
		if (hold_head_pin) {
			previous_head_pin = std::move(head_pin);
			head_pin = buffer_manager.Pin(head->block);
		}
	} else {
		handle = buffer_manager.Pin(head->block);
	}
	idx_t current_position = head->position;
	head->position += alloc_len;
	return UndoBufferReference(*head, std::move(handle), current_position);
}

} // namespace duckdb
