#include "duckdb/common/allocator.hpp"
#include "duckdb/storage/block_allocator.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

// Link wrapping confines failures to this executable's allocator implementation.
static thread_local bool reject_malloc = false;
static thread_local bool reject_new = false;
static thread_local bool reject_mapping = false;
static thread_local size_t allowed_new = 0;
static thread_local size_t fail_new_at = 0;
static thread_local size_t failures = 0;
static thread_local size_t new_calls = 0;
static thread_local bool track_new = false;
static thread_local int64_t new_balance = 0;

extern "C" {
void *__real_malloc(size_t size);
void *__real__Znwm(size_t size);
void __real__ZdlPv(void *pointer);
void __real__ZdlPvm(void *pointer, size_t size);
void *__real_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);

void *__wrap_malloc(size_t size) {
	if (reject_malloc) {
		failures++;
		return nullptr;
	}
	return __real_malloc(size);
}

void *__wrap__Znwm(size_t size) {
	new_calls++;
	const bool fail_once = fail_new_at > 0 && --fail_new_at == 0;
	if (fail_once || (reject_new && allowed_new == 0)) {
		failures++;
		throw std::bad_alloc();
	}
	if (allowed_new > 0) {
		allowed_new--;
	}
	auto result = __real__Znwm(size);
	if (track_new) {
		new_balance++;
	}
	return result;
}

void __wrap__ZdlPv(void *pointer) {
	if (track_new && pointer) {
		new_balance--;
	}
	__real__ZdlPv(pointer);
}

void __wrap__ZdlPvm(void *pointer, size_t size) {
	if (track_new && pointer) {
		new_balance--;
	}
	__real__ZdlPvm(pointer, size);
}

void *__wrap_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset) {
	if (reject_mapping) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return __real_mmap(address, length, protection, flags, fd, offset);
}
}

namespace duckdb {
class BlockAllocatorTestHelper {
public:
	static bool IsInPool(const BlockAllocator &allocator, data_ptr_t pointer) {
		return allocator.IsInPool(pointer);
	}
	static bool InitializePool(const BlockAllocator &allocator) {
		return allocator.InitializePool();
	}
};
} // namespace duckdb

static void Check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "BlockAllocator fault injection: %s\n", message);
		std::abort();
	}
}

template <class FUNC>
static void OnFreeingThread(FUNC function) {
#ifndef DUCKDB_NO_THREADS
	std::thread worker(function);
	worker.join();
#else
	function();
#endif
}

static void CheckAbandonedInitialization() {
	using namespace duckdb;
	constexpr idx_t BLOCK_SIZE = 65536;
	idx_t failure_points = 0;
	idx_t failed_reservations = 0;
	for (idx_t failure = 0; failure <= failure_points; failure++) {
		Allocator fallback;
		BlockAllocator allocator(fallback, BLOCK_SIZE, 257 * BLOCK_SIZE);
		new_balance = 0;
		new_calls = 0;
		track_new = true;
		reject_mapping = failure == 0;
		fail_new_at = failure;
		const auto previous_failures = failures;
		const auto initialized = BlockAllocatorTestHelper::InitializePool(allocator);
		track_new = false;
		reject_mapping = false;
		fail_new_at = 0;
		if (failure == 0) {
			Check(!initialized, "failed mapping must leave the pool unavailable");
			failure_points = new_calls;
		} else if (!initialized && failures != previous_failures) {
			failed_reservations++;
		}
		if (!initialized) {
			Check(new_balance == 0, "abandoned initialization retains queue storage");
		}
		auto pointer = allocator.AllocateData(BLOCK_SIZE);
		Check(BlockAllocatorTestHelper::IsInPool(allocator, pointer) == initialized, "allocation follows pool state");
		allocator.FreeData(pointer, BLOCK_SIZE);
	}
	Check(failed_reservations > 0, "reservation fault injection must activate");
}

static void CheckPoolOwnership() {
	using namespace duckdb;
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 2049;
	for (idx_t mode = 0; mode < 4; mode++) {
		Allocator fallback;
		BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_COUNT * BLOCK_SIZE);
		std::vector<data_ptr_t> original;
		reject_malloc = mode == 0;
		for (idx_t i = 0; i < BLOCK_COUNT; i++) {
			auto pointer = allocator.AllocateData(BLOCK_SIZE);
			Check(BlockAllocatorTestHelper::IsInPool(allocator, pointer), "initial pool capacity");
			pointer[0] = 42;
			original.push_back(pointer);
		}
		reject_malloc = false;
		size_t return_failures = 0;
		auto return_blocks = [&] {
			const auto previous_failures = failures;
			reject_malloc = true;
			reject_new = true;
			for (auto pointer : original) {
				allocator.FreeData(pointer, BLOCK_SIZE);
			}
			if (mode == 2) {
				allocator.ThreadExit();
			}
			reject_malloc = false;
			reject_new = false;
			return_failures = failures - previous_failures;
		};
		if (mode == 1) {
			allocator.ThreadExit();
			OnFreeingThread(return_blocks);
		} else {
			return_blocks();
		}
		Check(return_failures > 0, "return fault injection must activate");
		Check(allocator.GetCachedMemory() == BLOCK_COUNT * BLOCK_SIZE, "all returned blocks must stay charged");
		if (mode == 3) {
			allocator.ThreadExit();
			size_t reclaim_failures = 0;
			OnFreeingThread([&] {
				const auto previous_failures = failures;
				reject_malloc = true;
				reject_new = true;
				// Allow the reclamation batch, then reject queue growth.
				allowed_new = 1;
				allocator.FlushAll();
				reject_malloc = false;
				reject_new = false;
				allowed_new = 0;
				reclaim_failures = failures - previous_failures;
			});
			Check(reclaim_failures > 0, "reclamation fault injection must activate");
			Check(allocator.GetCachedMemory() == 0, "reclamation must preserve IDs when queue growth fails");
		}
		std::vector<data_ptr_t> reused;
		for (idx_t i = 0; i < BLOCK_COUNT; i++) {
			auto pointer = allocator.AllocateData(BLOCK_SIZE);
			Check(BlockAllocatorTestHelper::IsInPool(allocator, pointer), "pool capacity after failed returns");
			reused.push_back(pointer);
		}
		std::sort(original.begin(), original.end());
		std::sort(reused.begin(), reused.end());
		Check(original == reused, "each original pool block must be reused exactly once");
		for (auto pointer : reused) {
			allocator.FreeData(pointer, BLOCK_SIZE);
		}
		allocator.FlushAll();
		Check(allocator.GetCachedMemory() == 0, "all cached backing must be reclaimable");
	}
}

int main(int argc, char **argv) {
	Check(argc == 1 || argc == 2, "expected an optional parent process ID");
	if (argc == 2) {
		Check(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0, "parent-death signal registration");
		// The test process may have exited before signal registration.
		if (std::to_string(getppid()) != argv[1]) {
			return 1;
		}
	}
	CheckAbandonedInitialization();
	CheckPoolOwnership();
	std::puts("BlockAllocator allocation-failure checks passed");
}
