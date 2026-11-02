#include "catch.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/aggregate/minmax_n_helpers.hpp"

using namespace duckdb;

namespace {

data_ptr_t AllocateSmallHeapBuffer(PrivateAllocatorData *data, idx_t size) {
	if (size > 4096) {
		throw InternalException("Unexpected large allocation in heap overflow test");
	}
	return Allocator::DefaultAllocate(data, size);
}

data_ptr_t ReallocateSmallHeapBuffer(PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
	if (size > 4096) {
		throw InternalException("Unexpected large allocation in heap overflow test");
	}
	return Allocator::DefaultReallocate(data, pointer, old_size, size);
}

} // namespace

TEST_CASE("Min/max N string heap rejects overflowing capacities before allocation", "[aggregate][minmax_n]") {
	Allocator allocator(AllocateSmallHeapBuffer, Allocator::DefaultFree, ReallocateSmallHeapBuffer,
	                    make_uniq<PrivateAllocatorData>());
	ArenaAllocator arena(allocator);
	HeapEntry<string_t> entry;
	string initial;

	SECTION("First assignment") {
		REQUIRE(entry.allocated_data == nullptr);
	}
	SECTION("Growing an existing allocation") {
		initial = string(17, 'a');
		entry.Assign(arena, string_t(initial));
	}

	auto original_position = arena.GetPosition();
	auto original_capacity = entry.capacity;
	auto original_data = entry.allocated_data;
	for (auto size : {uint32_t(2147483649), NumericLimits<uint32_t>::Maximum()}) {
		// Only the length is needed: rejection must precede allocation or payload access.
		string_t oversized(size);
		REQUIRE_THROWS_WITH(entry.Assign(arena, oversized),
		                    Catch::Matchers::Contains("Resulting string/blob too large!"));
		REQUIRE(entry.capacity == original_capacity);
		REQUIRE(entry.allocated_data == original_data);
		REQUIRE(entry.value.GetString() == initial);
		REQUIRE(arena.GetPosition().chunk == original_position.chunk);
		REQUIRE(arena.GetPosition().current_position == original_position.current_position);
	}

	// A rejected assignment must leave the entry usable.
	string replacement(1000, 'z');
	entry.Assign(arena, string_t(replacement));
	REQUIRE(entry.value.GetString() == replacement);
}
