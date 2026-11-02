#include "duckdb/common/allocator.hpp"

#ifndef DUCKDB_ENABLE_JEMALLOC

#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"

#ifdef DUCKDB_DEBUG_ALLOCATION
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <execinfo.h>
#endif

namespace duckdb {

data_ptr_t Allocator::DefaultAllocate(PrivateAllocatorData *private_data, idx_t size) {
	auto default_allocate_result = malloc(size);
	if (!default_allocate_result) {
		throw std::bad_alloc();
	}
	return data_ptr_cast(default_allocate_result);
}

void Allocator::DefaultFree(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
	free(pointer);
}

data_ptr_t Allocator::DefaultReallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size,
                                        idx_t size) {
	return data_ptr_cast(realloc(pointer, size));
}

optional_idx Allocator::DecayDelay() {
	return optional_idx();
}

optional_idx Allocator::GetMappedMemory() const {
	return optional_idx();
}

bool Allocator::SupportsFlush() {
#if defined(__GLIBC__)
	return true;
#else
	return false;
#endif
}

void Allocator::ThreadFlush(bool allocator_background_threads, idx_t threshold, idx_t thread_count) {
	MallocTrim(thread_count * threshold);
}

void Allocator::ThreadIdle() {
	/* no-op */
}

void Allocator::FlushAll() {
	MallocTrim(0);
}

void Allocator::SetBackgroundThreads(bool enable) {
	/* no-op */
}

} // namespace duckdb

#endif
