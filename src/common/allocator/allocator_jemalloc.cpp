#include "duckdb/common/allocator.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

#include <thread>
#include <cstdint>

#if defined(DUCKDB_ENABLE_JEMALLOC)
#if !defined(WIN32) && INTPTR_MAX == INT64_MAX

#include "jemalloc/jemalloc.h"
#include "duckdb/malloc_ncpus.h"

#else
#error "jemalloc support is only available on 64-bit Linux with DUCKDB_ENABLE_JEMALLOC enabled"
#endif

extern "C" {

unsigned duckdb_malloc_ncpus() {
#ifdef DUCKDB_NO_THREADS
	return 1;
#else
	unsigned concurrency = duckdb::NumericCast<unsigned>(std::thread::hardware_concurrency());
	return std::max(concurrency, 1u);
#endif
}
}

namespace duckdb {

static string PurgeArenaString(idx_t arena_idx) {
	return StringUtil::Format("arena.%llu.purge", arena_idx);
}

static void JemallocCTL(const char *name, void *old_ptr, size_t *old_len, void *new_ptr, size_t new_len) {
	if (duckdb_je_mallctl(name, old_ptr, old_len, new_ptr, new_len) != 0) {
#ifdef DEBUG
		throw InternalException("je_mallctl failed for setting \"%s\"", name);
#endif
	}
}

template <class T>
static void SetJemallocCTL(const char *name, T &val) {
	JemallocCTL(name, nullptr, nullptr, &val, sizeof(T));
}

static void SetJemallocCTL(const char *name) {
	JemallocCTL(name, nullptr, nullptr, nullptr, 0);
}

template <class T>
static T GetJemallocCTL(const char *name) {
	T result;
	size_t len = sizeof(T);
	JemallocCTL(name, &result, &len, nullptr, 0);
	return result;
}

data_ptr_t Allocator::DefaultAllocate(PrivateAllocatorData *private_data, idx_t size) {
	return data_ptr_cast(duckdb_je_malloc(size));
}

void Allocator::DefaultFree(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
	duckdb_je_free(pointer);
}

data_ptr_t Allocator::DefaultReallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t old_size,
                                        idx_t size) {
	return data_ptr_cast(duckdb_je_realloc(pointer, size));
}

optional_idx Allocator::DecayDelay() {
	return NumericCast<idx_t>(DUCKDB_JEMALLOC_DECAY);
}

optional_idx Allocator::GetMappedMemory() const {
	if (allocate_function != DefaultAllocate || free_function != DefaultFree ||
	    reallocate_function != DefaultReallocate) {
		return optional_idx();
	}
	uint64_t epoch = 1;
	if (duckdb_je_mallctl("epoch", nullptr, nullptr, &epoch, sizeof(epoch)) != 0) {
		return optional_idx();
	}
	size_t mapped = 0;
	size_t mapped_size = sizeof(mapped);
	if (duckdb_je_mallctl("stats.mapped", &mapped, &mapped_size, nullptr, 0) != 0) {
		return optional_idx();
	}
	return optional_idx(mapped);
}

//! A thread runs without a tcache when jemalloc had no memory for one, and flushing it then fails
static void FlushThreadCache() {
	bool enabled = false;
	size_t enabled_size = sizeof(enabled);
	if (duckdb_je_mallctl("thread.tcache.enabled", &enabled, &enabled_size, nullptr, 0) != 0 || !enabled) {
		return;
	}
	SetJemallocCTL("thread.tcache.flush");
}

bool Allocator::SupportsFlush() {
	return true;
}

void Allocator::ThreadFlush(bool allocator_background_threads, idx_t threshold, idx_t thread_count) {
	// jemalloc only manages allocation done through the Allocator interface.
	// Any allocations done directly through "malloc" or "operator new" still
	// go to the system allocator. So we also trim the system heap here.
	MallocTrim(thread_count * threshold);

	if (!allocator_background_threads) {
		// We flush after exceeding the threshold
		if (GetJemallocCTL<uint64_t>("thread.peak.read") <= threshold) {
			return;
		}

		// Flush thread-local cache
		FlushThreadCache();

		// Reset the peak after flushing
		SetJemallocCTL("thread.peak.reset");
	}
}

void Allocator::ThreadIdle() {
	// Indicate that this thread is idle
	SetJemallocCTL("thread.idle");

	// Reset the peak after resetting
	SetJemallocCTL("thread.peak.reset");
}

void Allocator::FlushAll() {
	// Flush thread-local cache
	FlushThreadCache();

	// Flush all arenas
	const auto purge_arena = PurgeArenaString(MALLCTL_ARENAS_ALL);
	SetJemallocCTL(purge_arena.c_str());

	// Reset the peak after resetting
	SetJemallocCTL("thread.peak.reset");

	// Also return the system heap (see ThreadFlush) to the OS
	MallocTrim(0);
}

void Allocator::SetBackgroundThreads(bool enable) {
#ifndef __APPLE__
	SetJemallocCTL("background_thread", enable);
#endif
}

} // namespace duckdb

#endif
