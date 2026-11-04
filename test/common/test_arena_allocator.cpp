#include "catch.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <cstdlib>
#include <cstring>

using namespace duckdb;

TEST_CASE("Arena reset preserves the bounds of an unaligned chunk", "[arena_allocator]") {
	// Use malloc directly so ASan provides a redzone at the requested size.
	Allocator allocator([](PrivateAllocatorData *, idx_t size) { return data_ptr_cast(std::malloc(size)); },
	                    [](PrivateAllocatorData *, data_ptr_t pointer, idx_t) { std::free(pointer); },
	                    [](PrivateAllocatorData *, data_ptr_t pointer, idx_t, idx_t size) {
		                    return data_ptr_cast(std::realloc(pointer, size));
	                    },
	                    nullptr);
	ArenaAllocator arena(allocator, 1);
	arena.Allocate(1);
	auto head = arena.GetHead();
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
	REQUIRE(__asan_address_is_poisoned(head->data.get() + head->maximum_size));
#endif
	arena.AlignNext();
	REQUIRE(head->current_position > head->maximum_size);
	arena.Reset();
	auto data = arena.Allocate(head->maximum_size);
	memset(data, 42, head->maximum_size);
	REQUIRE(data[0] == 42);
	REQUIRE(data[head->maximum_size - 1] == 42);
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
	REQUIRE(__asan_address_is_poisoned(data + head->maximum_size));
#endif
}

TEST_CASE("Arena reset reuses storage from discarded containers", "[arena_allocator]") {
	ArenaAllocator arena(Allocator::DefaultAllocator(), 64);
	auto values = arena.Make<arena_vector<uint64_t>>(arena);
	values->reserve(4096);
	values->push_back(42);
	auto head = arena.GetHead();
	REQUIRE(head->prev == nullptr);
	REQUIRE(head->next);
	arena.Reset();
	REQUIRE(arena.GetHead() == head);
	REQUIRE_FALSE(head->next);
	auto data = arena.Allocate(head->maximum_size);
	memset(data, 42, head->maximum_size);
	REQUIRE(data[0] == 42);
	REQUIRE(data[head->maximum_size - 1] == 42);
}

TEST_CASE("Arena rewind reuses discarded storage and preserves live containers", "[arena_allocator]") {
	ArenaAllocator arena(Allocator::DefaultAllocator());
	auto live = arena.MakePtr<arena_vector<uint64_t>>(arena);
	live->reserve(16);
	live->push_back(42);
	// Save an unaligned position to exercise the boundary with retained storage.
	memset(arena.Allocate(3), 84, 3);
	const auto position = arena.GetPosition();
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
	const auto poisoned = __asan_address_is_poisoned(live->data() + 1);
#endif
	auto discarded = arena.Make<arena_vector<uint64_t>>(arena);
	SECTION("within one chunk") {
		discarded->reserve(32);
		REQUIRE(arena.GetHead() == position.chunk);
	}
	SECTION("across chunks") {
		discarded->reserve(4096);
		REQUIRE(arena.GetHead() != position.chunk);
	}
	discarded->push_back(13);
	arena.Rewind(position);
	REQUIRE(arena.GetHead() == position.chunk);
	REQUIRE(arena.GetPosition().current_position == position.current_position);
	const auto size = position.chunk->maximum_size - position.current_position;
	memset(arena.Allocate(size), 13, size);
#ifdef DUCKDB_ARENA_ALLOCATOR_ASAN
	REQUIRE(__asan_address_is_poisoned(live->data() + 1) == poisoned);
#endif
	REQUIRE((*live)[0] == 42);
	live->push_back(84);
	REQUIRE((*live)[1] == 84);
}

TEST_CASE("Arena tail shrinking makes discarded capacity reusable", "[arena_allocator]") {
	ArenaAllocator arena(Allocator::DefaultAllocator());
	auto values = arena.Make<arena_vector<uint64_t>>(arena);
	values->reserve(128);
	values->push_back(42);
	const auto buffer = data_ptr_cast(values->data());
	const auto old_size = values->capacity() * sizeof(uint64_t);
	const auto keep_size = sizeof(uint64_t);
	bool regrow = false;
	SECTION("shrink head") {
		arena.ShrinkHead(old_size - keep_size);
	}
	SECTION("reallocate smaller") {
		REQUIRE(arena.Reallocate(buffer, old_size, keep_size) == buffer);
	}
	SECTION("reallocate smaller then grow in place") {
		REQUIRE(arena.Reallocate(buffer, old_size, keep_size) == buffer);
		regrow = true;
	}
	if (regrow) {
		REQUIRE(arena.Reallocate(buffer, keep_size, old_size) == buffer);
	} else {
		REQUIRE(arena.Allocate(old_size - keep_size) == buffer + keep_size);
	}
	REQUIRE(*reinterpret_cast<uint64_t *>(buffer) == 42);
	memset(buffer + keep_size, 84, old_size - keep_size);
	REQUIRE(buffer[old_size - 1] == 84);
}

TEST_CASE("Arena reallocations can copy allocations from another arena", "[arena_allocator]") {
	Allocator allocator([](PrivateAllocatorData *, idx_t size) { return data_ptr_cast(std::malloc(size)); },
	                    [](PrivateAllocatorData *, data_ptr_t pointer, idx_t) { std::free(pointer); },
	                    [](PrivateAllocatorData *, data_ptr_t pointer, idx_t, idx_t size) {
		                    return data_ptr_cast(std::realloc(pointer, size));
	                    },
	                    nullptr);
	ArenaAllocator source(allocator, 1);
	ArenaAllocator target(allocator, 1);
	const idx_t old_size = 64;
	const idx_t size = GENERATE(32, 128);
	const idx_t occupied = GENERATE(0, 1, 128);
	if (occupied != 0) {
		memset(target.Allocate(occupied), 13, occupied);
	}
	auto pointer = source.Allocate(old_size);
	memset(pointer, 42, old_size);
	auto result = target.Reallocate(pointer, old_size, size);
	REQUIRE(result != pointer);
	REQUIRE(pointer[0] == 42);
	REQUIRE(pointer[old_size - 1] == 42);
	source.FreeAll();
	for (idx_t i = 0; i < MinValue(old_size, size); i++) {
		REQUIRE(result[i] == 42);
	}
	memset(result, 84, size);
	REQUIRE(result[size - 1] == 84);
}
