#include "duckdb/common/allocator.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/time_point.hpp"

#include <cstdint>
#include <new>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef DUCKDB_DEBUG_ALLOCATION
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <execinfo.h>
#endif

namespace duckdb {

constexpr const idx_t Allocator::MAXIMUM_ALLOC_SIZE;

AllocatedData::AllocatedData() : allocator(nullptr), pointer(nullptr), allocated_size(0) {
}

AllocatedData::AllocatedData(Allocator &allocator) : allocator(allocator), pointer(nullptr), allocated_size(0) {
}

AllocatedData::AllocatedData(Allocator &allocator, data_ptr_t pointer, idx_t allocated_size)
    : allocator(&allocator), pointer(pointer), allocated_size(allocated_size) {
	if (!pointer) {
		throw InternalException("AllocatedData object constructed with nullptr");
	}
}
AllocatedData::~AllocatedData() {
	Reset();
}

AllocatedData::AllocatedData(AllocatedData &&other) noexcept
    : allocator(other.allocator), pointer(nullptr), allocated_size(0) {
	std::swap(pointer, other.pointer);
	std::swap(allocated_size, other.allocated_size);
}

AllocatedData &AllocatedData::operator=(AllocatedData &&other) noexcept {
	std::swap(allocator, other.allocator);
	std::swap(pointer, other.pointer);
	std::swap(allocated_size, other.allocated_size);
	return *this;
}

void AllocatedData::Reset() {
	if (!pointer) {
		return;
	}
	D_ASSERT(allocator);
	allocator->FreeData(pointer, allocated_size);
	allocated_size = 0;
	pointer = nullptr;
}

//===--------------------------------------------------------------------===//
// Debug Info
//===--------------------------------------------------------------------===//
struct AllocatorDebugInfo {
#ifdef DEBUG
	AllocatorDebugInfo();
	~AllocatorDebugInfo();

	void AllocateData(data_ptr_t pointer, idx_t size);
	void FreeData(data_ptr_t pointer, idx_t size);
	void ReallocateData(data_ptr_t pointer, data_ptr_t new_pointer, idx_t old_size, idx_t new_size);

private:
	//! The number of bytes that are outstanding (i.e. that have been allocated - but not freed)
	//! Used for debug purposes
	atomic<idx_t> allocation_count;
#ifdef DUCKDB_DEBUG_ALLOCATION
	mutex pointer_lock;
	//! Set of active outstanding pointers together with stack traces
	unordered_map<data_ptr_t, pair<idx_t, string>> pointers;
	bool track_pointers = true;
#endif
#endif
};

PrivateAllocatorData::PrivateAllocatorData() {
}

PrivateAllocatorData::~PrivateAllocatorData() {
}

//===--------------------------------------------------------------------===//
// Allocator
//===--------------------------------------------------------------------===//
Allocator::Allocator()
    : Allocator(Allocator::DefaultAllocate, Allocator::DefaultFree, Allocator::DefaultReallocate, nullptr) {
}

Allocator::Allocator(allocate_function_ptr_t allocate_function_p, free_function_ptr_t free_function_p,
                     reallocate_function_ptr_t reallocate_function_p, unique_ptr<PrivateAllocatorData> private_data_p)
    : allocate_function(allocate_function_p), free_function(free_function_p),
      reallocate_function(reallocate_function_p), private_data(std::move(private_data_p)) {
	D_ASSERT(allocate_function);
	D_ASSERT(free_function);
	D_ASSERT(reallocate_function);
#ifdef DEBUG
	if (!private_data) {
		private_data = make_uniq<PrivateAllocatorData>();
	}
	private_data->debug_info = make_uniq<AllocatorDebugInfo>();
#endif
}

Allocator::~Allocator() {
}

data_ptr_t Allocator::AllocateData(idx_t size) {
	D_ASSERT(size > 0);
	if (size >= MAXIMUM_ALLOC_SIZE) {
		D_ASSERT(false);
		throw InternalException("Requested allocation size of %llu is out of range - maximum allocation size is %llu",
		                        size, MAXIMUM_ALLOC_SIZE);
	}
	auto result = allocate_function(private_data.get(), size);
	if (!result) {
		throw OutOfMemoryException("Failed to allocate block of %llu bytes (bad allocation)", size);
	}
#ifdef DEBUG
	if (ShouldUseDebugInfo()) {
		private_data->debug_info->AllocateData(result, size);
	}
#endif
	return result;
}

void Allocator::FreeData(data_ptr_t pointer, idx_t size) {
	if (!pointer) {
		return;
	}
	D_ASSERT(size > 0);
#ifdef DEBUG
	if (ShouldUseDebugInfo()) {
		private_data->debug_info->FreeData(pointer, size);
	}
#endif
	free_function(private_data.get(), pointer, size);
}

data_ptr_t Allocator::ReallocateData(data_ptr_t pointer, idx_t old_size, idx_t size) {
	if (!pointer) {
		return nullptr;
	}
	if (size >= MAXIMUM_ALLOC_SIZE) {
		D_ASSERT(false);
		throw InternalException(
		    "Requested re-allocation size of %llu is out of range - maximum allocation size is %llu", size,
		    MAXIMUM_ALLOC_SIZE);
	}
	auto new_pointer = reallocate_function(private_data.get(), pointer, old_size, size);
	if (!new_pointer) {
		throw OutOfMemoryException("Failed to re-allocate block of %llu bytes (bad allocation)", size);
	}
#ifdef DEBUG
	if (ShouldUseDebugInfo()) {
		private_data->debug_info->ReallocateData(pointer, new_pointer, old_size, size);
	}
#endif
	return new_pointer;
}
shared_ptr<Allocator> &Allocator::DefaultAllocatorReference() {
	static shared_ptr<Allocator> DEFAULT_ALLOCATOR = make_shared_ptr<Allocator>();
	return DEFAULT_ALLOCATOR;
}

Allocator &Allocator::DefaultAllocator() {
	return *DefaultAllocatorReference();
}

void Allocator::MallocTrim(idx_t pad) {
#ifdef __GLIBC__
	static constexpr int64_t TRIM_INTERVAL_MS = 100;
	static atomic<int64_t> LAST_TRIM_TICK_MS {-TRIM_INTERVAL_MS};

	int64_t last_trim_tick_ms = LAST_TRIM_TICK_MS.load();
	auto current_tick_ms = TimePoint::GetTickMs();

	if (current_tick_ms - last_trim_tick_ms < TRIM_INTERVAL_MS) {
		return; // We trimmed less than TRIM_INTERVAL_MS ago
	}
	if (!LAST_TRIM_TICK_MS.compare_exchange_strong(last_trim_tick_ms, current_tick_ms, std::memory_order_acquire,
	                                               std::memory_order_relaxed)) {
		return; // Another thread has updated LAST_TRIM_TICK_MS since we loaded it
	}

	// We successfully updated LAST_TRIM_TICK_MS, we can trim
	malloc_trim(pad);
#endif
}

//===--------------------------------------------------------------------===//
// Debug Info (extended)
//===--------------------------------------------------------------------===//
#ifdef DEBUG
AllocatorDebugInfo::AllocatorDebugInfo() {
	allocation_count = 0;
}
AllocatorDebugInfo::~AllocatorDebugInfo() {
#ifdef DUCKDB_DEBUG_ALLOCATION
	if (allocation_count != 0) {
		printf("Outstanding allocations found for Allocator\n");
		if (!track_pointers) {
			printf("Pointer tracing was disabled after an allocation failure\n");
		}
		for (auto &entry : pointers) {
			printf("Allocation of size %llu at address %p\n", entry.second.first, (void *)entry.first);
			printf("Stack trace:\n%s\n", entry.second.second.c_str());
			printf("\n");
		}
	}
#endif
	//! Verify that there is no outstanding memory still associated with the batched allocator
	//! Only works for access to the batched allocator through the batched allocator interface
	//! If this assertion triggers, enable DUCKDB_DEBUG_ALLOCATION for more information about the allocations
	D_ASSERT(allocation_count == 0);
}

void AllocatorDebugInfo::AllocateData(data_ptr_t pointer, idx_t size) {
	allocation_count += size;
#ifdef DUCKDB_DEBUG_ALLOCATION
	lock_guard<mutex> l(pointer_lock);
	if (track_pointers) {
		try {
			pointers[pointer] = make_pair(size, Exception::GetStackTrace());
		} catch (const std::bad_alloc &) {
			// Bookkeeping must not make an already committed allocation or reallocation fail.
			pointers.clear();
			track_pointers = false;
		}
	}
#endif
}

void AllocatorDebugInfo::FreeData(data_ptr_t pointer, idx_t size) {
	D_ASSERT(allocation_count >= size);
	allocation_count -= size;
#ifdef DUCKDB_DEBUG_ALLOCATION
	lock_guard<mutex> l(pointer_lock);
	if (!track_pointers) {
		return;
	}
	// verify that the pointer exists
	D_ASSERT(pointers.find(pointer) != pointers.end());
	// verify that the stored size matches the passed in size
	D_ASSERT(pointers[pointer].first == size);
	// erase the pointer
	pointers.erase(pointer);
#endif
}

void AllocatorDebugInfo::ReallocateData(data_ptr_t pointer, data_ptr_t new_pointer, idx_t old_size, idx_t new_size) {
	FreeData(pointer, old_size);
	AllocateData(new_pointer, new_size);
}

#endif

} // namespace duckdb
