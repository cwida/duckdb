//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/arena_allocator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/string.hpp"
#include "duckdb/common/arena_containers/arena_ptr.hpp"

#if defined(__has_include)
#if __has_include(<sanitizer/asan_interface.h>)
#define DUCKDB_ARENA_ALLOCATOR_HAS_ASAN_INTERFACE
#endif
#elif !defined(__MINGW32__)
#define DUCKDB_ARENA_ALLOCATOR_HAS_ASAN_INTERFACE
#endif

#ifdef DUCKDB_ARENA_ALLOCATOR_HAS_ASAN_INTERFACE
#if defined(__SANITIZE_ADDRESS__)
#define DUCKDB_ARENA_ALLOCATOR_ASAN
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DUCKDB_ARENA_ALLOCATOR_ASAN
#endif
#endif
#endif

#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace duckdb {

struct ArenaChunk {
	ArenaChunk(Allocator &allocator, idx_t size);
	~ArenaChunk();

	void ResetPosition(idx_t position) {
		D_ASSERT(position <= current_position);
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
		// Discarded arena containers can leave their unused capacity poisoned.
		const auto end = MinValue(current_position, maximum_size);
		if (position < end) {
			__asan_unpoison_memory_region(data.get() + position, end - position);
		}
#endif
		current_position = position;
	}

	AllocatedData data;
	idx_t current_position;
	idx_t maximum_size;
	unsafe_unique_ptr<ArenaChunk> next;
	ArenaChunk *prev;
};

class ArenaAllocator {
public:
	static constexpr const idx_t ARENA_ALLOCATOR_INITIAL_CAPACITY = 2048;
	static constexpr const idx_t ARENA_ALLOCATOR_MAX_CAPACITY = 1ULL << 24ULL; // 16MB

public:
	DUCKDB_API explicit ArenaAllocator(Allocator &allocator, idx_t initial_capacity = ARENA_ALLOCATOR_INITIAL_CAPACITY);
	DUCKDB_API ~ArenaAllocator();

	data_ptr_t Allocate(idx_t len) {
		D_ASSERT(!head || head->current_position <= head->maximum_size);
		if (!head || head->current_position + len > head->maximum_size) {
			AllocateNewBlock(len);
		}
		D_ASSERT(head->current_position + len <= head->maximum_size);
		auto result = head->data.get() + head->current_position;
		head->current_position += len;
		return result;
	}
	//! Resize in place when possible, otherwise copy into this arena. The source may belong to another live arena.
	//! Equal sizes return the source unchanged.
	DUCKDB_API data_ptr_t Reallocate(data_ptr_t pointer, idx_t old_size, idx_t size);

	DUCKDB_API data_ptr_t AllocateAligned(idx_t size);
	DUCKDB_API data_ptr_t ReallocateAligned(data_ptr_t pointer, idx_t old_size, idx_t size);

	//! Increment the internal cursor (if required) so the next allocation is guaranteed to be aligned to 8 bytes
	DUCKDB_API void AlignNext();

	//! This shrinks the LAST allocation that was made using the allocator
	//! Note that we can ONLY safely call this method if Allocate has been called previously with a size >= shrink_size
	DUCKDB_API void ShrinkHead(idx_t shrink_size) const {
		D_ASSERT(head && head->current_position >= shrink_size);
		head->ResetPosition(head->current_position - shrink_size);
	}

	//! A position in the arena - everything allocated after it was taken can be released again with Rewind
	struct Position {
		ArenaChunk *chunk = nullptr;
		idx_t current_position = 0;
	};
	//! Returns the current position of the arena
	DUCKDB_API Position GetPosition() const;
	//! Releases everything that was allocated after the position was taken - those allocations may no longer be used
	DUCKDB_API void Rewind(const Position &position);

	//! Resets the current head and destroys all previous arena chunks
	DUCKDB_API void Reset();
	//! Like Reset, but also destroys the current chunk, so no memory stays allocated
	DUCKDB_API void FreeAll();
	DUCKDB_API void Destroy();
	DUCKDB_API void Move(ArenaAllocator &allocator);

	DUCKDB_API ArenaChunk *GetHead();
	DUCKDB_API ArenaChunk *GetTail();

	DUCKDB_API bool IsEmpty() const;
	//! Get the total *used* size (not cached)
	DUCKDB_API idx_t SizeInBytes() const;
	//! Get the currently allocated size in bytes (cached, read from "allocated_size")
	DUCKDB_API idx_t AllocationSize() const;

	//! Returns an "Allocator" wrapper for this arena allocator
	Allocator &GetAllocator() {
		return arena_allocator;
	}
	//! Returns the allocator the arena itself allocates its blocks from - for memory that must be freeable
	//! individually but should share the arena's memory accounting
	Allocator &GetUnderlyingAllocator() {
		return allocator;
	}

	template <class T, class... ARGS>
	T *Make(ARGS &&... args) {
		auto mem = AllocateAligned(sizeof(T));
		return new (mem) T(std::forward<ARGS>(args)...);
	}

	template <class T, class... ARGS>
	arena_ptr<T> MakePtr(ARGS &&... args) {
		return arena_ptr<T>(Make<T>(std::forward<ARGS>(args)...));
	}

	template <class T, class... ARGS>
	unsafe_arena_ptr<T> MakeUnsafePtr(ARGS &&... args) {
		return unsafe_arena_ptr<T>(Make<T>(std::forward<ARGS>(args)...));
	}

	String MakeString(const char *data, const size_t len) {
		data_ptr_t mem = nullptr;

		D_ASSERT(len < NumericLimits<uint32_t>::Maximum());
		const auto size = static_cast<uint32_t>(len);
		if (!String::CanBeInlined(size)) {
			// If the string can't be inlined, we allocate it on the arena allocator
			mem = AllocateAligned(sizeof(char) * size + 1); // +1 for null terminator
			memcpy(mem, data, size);
			mem[size] = '\0';
		}

		return String::Reference(mem ? reinterpret_cast<char *>(mem) : data, size);
	}

	String MakeString(const std::string &data) {
		return MakeString(data.c_str(), data.size());
	}

private:
	void AllocateNewBlock(idx_t min_size);

private:
	//! Internal allocator that is used by the arena allocator
	Allocator &allocator;
	idx_t initial_capacity;
	unsafe_unique_ptr<ArenaChunk> head;
	ArenaChunk *tail;
	//! An allocator wrapper using this arena allocator
	Allocator arena_allocator;
	//! The total allocated size
	idx_t allocated_size = 0;
};

} // namespace duckdb
