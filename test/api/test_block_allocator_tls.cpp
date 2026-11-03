#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <new>
#include <thread>
#include <unordered_set>

#if defined(__linux__)
#include <sys/mman.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

using namespace duckdb;

namespace duckdb {
class BlockAllocatorTestHelper {
public:
	explicit BlockAllocatorTestHelper(const BlockAllocator &allocator, idx_t now_ms = 0) : allocator(allocator) {
		allocator.InitializeRetention(now_ms);
	}
	void Add(idx_t count, idx_t now_ms) {
		allocator.AddRetention(count, now_ms);
	}
	void Reuse(idx_t count, idx_t now_ms) {
		allocator.ReuseRetention(count, now_ms);
	}
	idx_t Target(idx_t now_ms) {
		return allocator.RetentionTarget(now_ms);
	}
	bool IsLockFree() const {
		for (auto &bucket : allocator.retention_buckets) {
			if (!bucket.is_lock_free()) {
				return false;
			}
		}
		return true;
	}
	annotated_mutex &MaintenanceLock() {
		return allocator.flush_lock;
	}
	static void Expire(const BlockAllocator &allocator) {
		allocator.ThreadFlush(false, 0, 1);
		annotated_lock_guard<annotated_mutex> guard(allocator.flush_lock);
		allocator.InitializeRetention(Now());
	}
	static idx_t FreeBlocks(const BlockAllocator &allocator) {
		annotated_lock_guard<annotated_mutex> guard(allocator.flush_lock);
		return allocator.GetFreeBlockCount();
	}
	static idx_t RetainedBlocks(const BlockAllocator &allocator) {
		annotated_lock_guard<annotated_mutex> guard(allocator.flush_lock);
		return allocator.RetentionTarget(Now());
	}
	static bool Drained(const BlockAllocator &allocator) {
		annotated_lock_guard<annotated_mutex> guard(allocator.flush_lock);
		return allocator.GetFreeBlockCount() == 0 && allocator.flush_state == BlockAllocator::FlushState::IDLE;
	}
	static void FlushBytes(const BlockAllocator &allocator, idx_t bytes) {
		allocator.FlushPool(allocator.DivBlockSize(bytes));
	}
	static idx_t Deallocated(const BlockAllocator &allocator) {
		return allocator.estimated_unflushed_memory.load(std::memory_order_relaxed);
	}
	static bool IsInPool(const BlockAllocator &allocator, data_ptr_t pointer) {
		return allocator.IsInPool(pointer);
	}
	static bool IsInitialized(const BlockAllocator &allocator) {
		return allocator.IsInitialized();
	}
	static data_ptr_t AllocateHuge(const BlockAllocator &allocator, idx_t size) {
		return allocator.AllocateHugeData(size);
	}
	static void FreeHuge(const BlockAllocator &allocator, data_ptr_t pointer, idx_t size) {
		allocator.FreeHugeData(pointer, size);
	}

private:
	static idx_t Now() {
		return NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
		        .count());
	}

	const BlockAllocator &allocator;
};
} // namespace duckdb

TEST_CASE("BlockAllocator initializes only for eligible allocations and falls back without a pool",
          "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	const auto pool_size = GENERATE_COPY(idx_t(0), NumericLimits<idx_t>::Maximum(), 3 * BLOCK_SIZE + 1);
	const bool available = sizeof(void *) == 8 && pool_size == 3 * BLOCK_SIZE + 1;
	Allocator fallback;
	BlockAllocator allocator(fallback, BLOCK_SIZE, pool_size);
	CHECK(allocator.IsEnabled());
	auto small = allocator.AllocateData(1024);
	small = allocator.ReallocateData(small, 1024, 2048);
	allocator.FreeData(small, 2048);
	auto huge = BlockAllocatorTestHelper::AllocateHuge(allocator, 2 * BLOCK_SIZE);
	BlockAllocatorTestHelper::FreeHuge(allocator, huge, 2 * BLOCK_SIZE);
	allocator.ThreadExit();
	allocator.ThreadIdle();
	allocator.FlushAll();
	CHECK(allocator.GetCachedMemory() == 0);
	CHECK_FALSE(BlockAllocatorTestHelper::IsInitialized(allocator));

	data_ptr_t block = nullptr;
	bool huge_allocation = false;
	SECTION("Ordinary allocation") {
		block = allocator.AllocateData(BLOCK_SIZE);
	}
	SECTION("Huge allocation") {
		block = BlockAllocatorTestHelper::AllocateHuge(allocator, BLOCK_SIZE);
		huge_allocation = true;
	}
	SECTION("Reallocation into the pool") {
		block = allocator.AllocateData(1024);
		memset(block, 42, 1024);
		block = allocator.ReallocateData(block, 1024, BLOCK_SIZE);
		for (idx_t i = 0; i < 1024; i++) {
			CHECK(block[i] == 42);
		}
	}
	CHECK(BlockAllocatorTestHelper::IsInitialized(allocator) == available);
	CHECK(BlockAllocatorTestHelper::IsInPool(allocator, block) == available);
	auto second = allocator.AllocateData(BLOCK_SIZE);
	auto third = allocator.AllocateData(BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::IsInPool(allocator, second) == available);
	CHECK(BlockAllocatorTestHelper::IsInPool(allocator, third) == available);
	auto overflow = allocator.AllocateData(BLOCK_SIZE);
	CHECK_FALSE(BlockAllocatorTestHelper::IsInPool(allocator, overflow));
	allocator.FreeData(overflow, BLOCK_SIZE);
	allocator.FreeData(third, BLOCK_SIZE);
	allocator.FreeData(second, BLOCK_SIZE);
	if (huge_allocation) {
		BlockAllocatorTestHelper::FreeHuge(allocator, block, BLOCK_SIZE);
	} else {
		allocator.FreeData(block, BLOCK_SIZE);
	}
}

namespace {
#if defined(__linux__)
class AllocationCacheTestAffinity {
public:
	AllocationCacheTestAffinity() {
		valid = sched_getaffinity(0, sizeof(original), &original) == 0;
	}
	~AllocationCacheTestAffinity() {
		if (valid) {
			sched_setaffinity(0, sizeof(original), &original);
		}
	}
	vector<idx_t> CPUs() const {
		vector<idx_t> result;
		if (valid) {
			for (idx_t cpu = 0; cpu < CPU_SETSIZE; cpu++) {
				if (CPU_ISSET(cpu, &original)) {
					result.push_back(cpu);
				}
			}
		}
		return result;
	}
	bool Pin(idx_t cpu) {
		cpu_set_t mask;
		CPU_ZERO(&mask);
		CPU_SET(cpu, &mask);
		return sched_setaffinity(0, sizeof(mask), &mask) == 0;
	}

private:
	cpu_set_t original;
	bool valid;
};
#endif

struct AllocationCacheTestData : PrivateAllocatorData {
	explicit AllocationCacheTestData(atomic<idx_t> &live_bytes) : live_bytes(live_bytes) {
	}
	atomic<idx_t> &live_bytes;
	bool fail_allocate = false;
	bool fail_reallocate = false;
	idx_t memory_limit = NumericLimits<idx_t>::Maximum();
	idx_t failed_requests = 0;
	bool throw_bad_alloc = false;

	bool CanAllocate(idx_t size, idx_t old_size = 0) {
		const auto remaining = live_bytes.load() - old_size;
		if (remaining <= memory_limit && size <= memory_limit - remaining) {
			return true;
		}
		failed_requests++;
		if (throw_bad_alloc) {
			throw std::bad_alloc();
		}
		return false;
	}
};

data_ptr_t CacheTestAllocate(PrivateAllocatorData *data, idx_t size) {
	auto &state = data->Cast<AllocationCacheTestData>();
	if (state.fail_allocate || !state.CanAllocate(size)) {
		return nullptr;
	}
	auto pointer = Allocator::DefaultAllocate(data, size);
	data->Cast<AllocationCacheTestData>().live_bytes += size;
	return pointer;
}

void CacheTestFree(PrivateAllocatorData *data, data_ptr_t pointer, idx_t size) {
	data->Cast<AllocationCacheTestData>().live_bytes -= size;
	Allocator::DefaultFree(data, pointer, size);
}

data_ptr_t CacheTestReallocate(PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
	auto &state = data->Cast<AllocationCacheTestData>();
	if (state.fail_reallocate || !state.CanAllocate(size, old_size)) {
		return nullptr;
	}
	auto result = Allocator::DefaultReallocate(data, pointer, old_size, size);
	if (result) {
		state.live_bytes += size - old_size;
	}
	return result;
}
} // namespace

TEST_CASE("BlockAllocator reclaims cached backing before retrying native allocation failures",
          "[api][block_allocator][allocation_cache]") {
	enum class Operation { ALLOCATE, REALLOCATE, HUGE_ALLOCATION, EXHAUSTED_POOL };
	const auto operation =
	    GENERATE(Operation::ALLOCATE, Operation::REALLOCATE, Operation::HUGE_ALLOCATION, Operation::EXHAUSTED_POOL);
	const bool throw_bad_alloc = GENERATE(false, true);
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t SIZE = 512 * 1024;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	auto &control = fallback.GetPrivateData()->Cast<AllocationCacheTestData>();
	control.throw_bad_alloc = throw_bad_alloc;
	{
		BlockAllocator allocator(fallback, BLOCK_SIZE, 2 * BLOCK_SIZE);
		const auto free_pool = [&](data_ptr_t pointer) {
			allocator.FreeData(pointer, BLOCK_SIZE);
		};
		vector<unique_ptr<data_t, decltype(free_pool)>> pooled;
		if (operation == Operation::EXHAUSTED_POOL) {
			for (idx_t i = 0; i < 2; i++) {
				pooled.emplace_back(allocator.AllocateData(BLOCK_SIZE), free_pool);
			}
		}
		const auto free_live = [&](data_ptr_t pointer) {
			allocator.FreeData(pointer, SIZE);
		};
		unique_ptr<data_t, decltype(free_live)> live(allocator.AllocateData(SIZE), free_live);
		live.get()[0] = 42;
		live.get()[SIZE - 1] = 84;
		auto cached = allocator.AllocateData(2 * SIZE);
		allocator.FreeData(cached, 2 * SIZE);
		REQUIRE(allocator.GetCachedMemory() == 2 * SIZE);
		control.memory_limit = live_bytes.load();
		const auto request_size = operation == Operation::REALLOCATE       ? 2 * SIZE
		                          : operation == Operation::EXHAUSTED_POOL ? BLOCK_SIZE
		                                                                   : SIZE;
		const auto free_result = [&](data_ptr_t pointer) {
			allocator.FreeData(pointer, request_size);
		};
		unique_ptr<data_t, decltype(free_result)> result(nullptr, free_result);
		CHECK_NOTHROW([&] {
			if (operation == Operation::REALLOCATE) {
				auto pointer = allocator.ReallocateData(live.get(), SIZE, request_size);
				live.release();
				result.reset(pointer);
			} else if (operation == Operation::HUGE_ALLOCATION) {
				result.reset(BlockAllocatorTestHelper::AllocateHuge(allocator, request_size));
			} else {
				result.reset(allocator.AllocateData(request_size));
			}
		}());
		CHECK(result.get() != nullptr);
		CHECK(control.failed_requests == 1);
		CHECK(allocator.GetCachedMemory() == 0);
		auto original = live ? live.get() : result.get();
		if (original) {
			CHECK(original[0] == 42);
			CHECK(original[SIZE - 1] == 84);
		}
		if (result) {
			result.get()[0] = 21;
			control.memory_limit = 0;
			const auto failures = control.failed_requests;
			CHECK_THROWS(allocator.ReallocateData(result.get(), request_size, 2 * request_size));
			CHECK(control.failed_requests == failures + 2);
			CHECK(result.get()[0] == 21);
		}
	}
	CHECK(live_bytes.load() == 0);
}

#if INTPTR_MAX == INT64_MAX
#ifndef DUCKDB_NO_THREADS
TEST_CASE("BlockAllocator publishes the complete pool to concurrent first allocations", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t WORKERS = 4;
	constexpr idx_t BLOCKS_PER_WORKER = 16;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	BlockAllocator allocator(fallback, BLOCK_SIZE, WORKERS * BLOCKS_PER_WORKER * BLOCK_SIZE);
	atomic<idx_t> ready {0};
	atomic<bool> start {false};
	array<array<data_ptr_t, BLOCKS_PER_WORKER>, WORKERS> blocks;
	vector<std::thread> workers;
	for (idx_t t = 0; t < WORKERS; t++) {
		workers.emplace_back([&, t] {
			ready++;
			while (!start.load()) {
				std::this_thread::yield();
			}
			for (auto &block : blocks[t]) {
				block = allocator.AllocateData(BLOCK_SIZE);
				memset(block, static_cast<uint8_t>(t), BLOCK_SIZE);
			}
		});
	}
	while (ready.load() < WORKERS) {
		std::this_thread::yield();
	}
	start = true;
	for (auto &worker : workers) {
		worker.join();
	}
	std::unordered_set<data_ptr_t> allocated;
	for (idx_t t = 0; t < WORKERS; t++) {
		for (auto block : blocks[t]) {
			CHECK(allocated.insert(block).second);
			CHECK(BlockAllocatorTestHelper::IsInPool(allocator, block));
			CHECK(block[0] == t);
			CHECK(block[BLOCK_SIZE - 1] == t);
		}
	}
	CHECK(live_bytes.load() == 0);
	for (auto block : allocated) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
}
#endif

TEST_CASE("BlockAllocator toggles allocation permission without changing pool ownership", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	const bool huge = GENERATE(false, true);
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	config.SetOptionByName("enable_block_allocator", Value::BOOLEAN(false));
	config.block_allocator = make_uniq<BlockAllocator>(fallback, BLOCK_SIZE, 2 * BLOCK_SIZE);
	DuckDB db(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db.instance);
	auto &db_config = DBConfig::GetConfig(*db.instance);
	const auto baseline_live_bytes = live_bytes.load();
	const auto option = DBConfig::GetOptionByName("enable_block_allocator");
	REQUIRE(option);
	auto allocate = [&] {
		return huge ? BlockAllocatorTestHelper::AllocateHuge(allocator, BLOCK_SIZE)
		            : allocator.AllocateData(BLOCK_SIZE);
	};
	auto free = [&](data_ptr_t pointer) {
		if (huge) {
			BlockAllocatorTestHelper::FreeHuge(allocator, pointer, BLOCK_SIZE);
		} else {
			allocator.FreeData(pointer, BLOCK_SIZE);
		}
	};
	CHECK_FALSE(allocator.IsEnabled());
	auto before_initialization = allocate();
	if (!huge) {
		before_initialization[0] = 42;
		before_initialization = allocator.ReallocateData(before_initialization, BLOCK_SIZE, BLOCK_SIZE / 2);
		before_initialization = allocator.ReallocateData(before_initialization, BLOCK_SIZE / 2, BLOCK_SIZE);
		CHECK(before_initialization[0] == 42);
	}
	CHECK_FALSE(BlockAllocatorTestHelper::IsInitialized(allocator));
	CHECK_FALSE(BlockAllocatorTestHelper::IsInPool(allocator, before_initialization));
	db_config.SetOption(db.instance.get(), *option, Value::BOOLEAN(true));
	CHECK(allocator.IsEnabled());
	CHECK_FALSE(BlockAllocatorTestHelper::IsInitialized(allocator));
	auto pooled = allocate();
	pooled[0] = 42;
	pooled[BLOCK_SIZE - 1] = 84;
	REQUIRE(BlockAllocatorTestHelper::IsInPool(allocator, pooled));
	free(before_initialization);
	CHECK(live_bytes.load() == baseline_live_bytes);
	auto spare = allocate();
	free(spare);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);

	db_config.SetOption(db.instance.get(), *option, Value::BOOLEAN(false));
	CHECK_FALSE(allocator.IsEnabled());
	auto disabled = allocate();
	CHECK_FALSE(BlockAllocatorTestHelper::IsInPool(allocator, disabled));
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
	CHECK(pooled[0] == 42);
	CHECK(pooled[BLOCK_SIZE - 1] == 84);
	if (huge) {
		free(pooled);
	} else {
		auto resized = allocator.ReallocateData(pooled, BLOCK_SIZE, 2 * BLOCK_SIZE);
		CHECK_FALSE(BlockAllocatorTestHelper::IsInPool(allocator, resized));
		CHECK(resized[0] == 42);
		CHECK(resized[BLOCK_SIZE - 1] == 84);
		resized = allocator.ReallocateData(resized, 2 * BLOCK_SIZE, BLOCK_SIZE);
		CHECK_FALSE(BlockAllocatorTestHelper::IsInPool(allocator, resized));
		CHECK(resized[0] == 42);
		CHECK(resized[BLOCK_SIZE - 1] == 84);
		allocator.FreeData(resized, BLOCK_SIZE);
	}
	CHECK(allocator.GetCachedMemory() == 2 * BLOCK_SIZE);
	allocator.FlushAll();
	CHECK(allocator.GetCachedMemory() == 0);
	CHECK(BlockAllocatorTestHelper::IsInitialized(allocator));
	CHECK(live_bytes.load() == baseline_live_bytes + BLOCK_SIZE);

	db_config.ResetOption(db.instance.get(), *option);
	CHECK(allocator.IsEnabled() == DBConfigOptions().enable_block_allocator);
	db_config.SetOption(db.instance.get(), *option, Value::BOOLEAN(true));
	free(disabled);
	CHECK(live_bytes.load() == baseline_live_bytes);
	auto reused = allocate();
	CHECK((reused == pooled || reused == spare));
	CHECK(BlockAllocatorTestHelper::IsInPool(allocator, reused));
	free(reused);
}

TEST_CASE("Allocation cache reuses compatible allocations and releases retained backing", "[api][allocation_cache]") {
	bool pinned = false;
#if defined(__linux__)
	AllocationCacheTestAffinity affinity;
	const auto cpus = affinity.CPUs();
	pinned = !cpus.empty() && affinity.Pin(cpus[0]);
#endif
	constexpr idx_t SIZE = 512 * 1024;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	{
		BlockAllocator allocator(fallback, 256 * 1024, 0);
		auto uncached = allocator.AllocateData(SIZE + 4096);
		allocator.FreeData(uncached, SIZE + 4096);
		REQUIRE(live_bytes.load() == 0);
		auto first = allocator.AllocateData(SIZE);
		first[0] = 42;
		first[SIZE - 1] = 84;
		allocator.FreeData(first, SIZE);
		REQUIRE(live_bytes.load() == SIZE);

		auto reused = allocator.AllocateData(SIZE);
		if (pinned) {
			REQUIRE(reused == first);
			REQUIRE(reused[0] == 42);
			REQUIRE(reused[SIZE - 1] == 84);
		}
		reused[0] = 42;
		reused[SIZE - 1] = 84;
		auto second = allocator.AllocateData(SIZE);
		REQUIRE(second != reused);
		const auto backing_before = live_bytes.load();
		// A cached allocation must still be compatible with fallback reallocation.
		reused = allocator.ReallocateData(reused, SIZE, 2 * SIZE);
		REQUIRE(reused[0] == 42);
		REQUIRE(reused[SIZE - 1] == 84);
		allocator.FreeData(reused, 2 * SIZE);
		allocator.FreeData(second, SIZE);
		REQUIRE(live_bytes.load() == backing_before + SIZE);
		allocator.ThreadExit();
		allocator.ThreadFlush(false, 0, 1);
		CHECK(allocator.GetCachedMemory() == live_bytes.load());
		CHECK(allocator.SupportsFlush());
		CHECK(allocator.DecayDelay().IsValid());
		SECTION("Owner destruction releases retained allocations") {
		}
		SECTION("Explicit reclamation works without a fixed pool") {
			allocator.FlushAll();
			CHECK(allocator.GetCachedMemory() == 0);
			CHECK(live_bytes.load() == 0);
		}
		SECTION("Standalone idle maintenance releases retained allocations") {
			allocator.ThreadIdle();
			CHECK(live_bytes.load() == 0);
		}
		SECTION("Shutdown releases retained allocations") {
			allocator.FlushOnShutdown();
			CHECK(live_bytes.load() == 0);
		}
	}
	REQUIRE(live_bytes.load() == 0);
}

TEST_CASE("Huge allocation retention shares cache ownership and pressure reclamation", "[api][allocation_cache]") {
	const auto size = GENERATE(idx_t(8 * 1024 * 1024), idx_t(128 * 1024 * 1024));
	bool pinned = false;
#if defined(__linux__)
	AllocationCacheTestAffinity affinity;
	const auto cpus = affinity.CPUs();
	pinned = !cpus.empty() && affinity.Pin(cpus[0]);
#endif
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	{
		BlockAllocator allocator(fallback, 256 * 1024, 0);
		auto first = BlockAllocatorTestHelper::AllocateHuge(allocator, size);
		first[0] = 42;
		first[size - 1] = 84;
		BlockAllocatorTestHelper::FreeHuge(allocator, first, size);
		REQUIRE(allocator.GetCachedMemory() == size);
		REQUIRE(live_bytes.load() == size);

		auto live = BlockAllocatorTestHelper::AllocateHuge(allocator, size);
		if (pinned) {
			REQUIRE(live == first);
			REQUIRE(live[0] == 42);
			REQUIRE(live[size - 1] == 84);
		}
		live[0] = 99;
		live[size - 1] = 77;
		auto spare = BlockAllocatorTestHelper::AllocateHuge(allocator, size);
		BlockAllocatorTestHelper::FreeHuge(allocator, spare, size);
		REQUIRE(allocator.GetCachedMemory() >= size);
		allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), 0);
		CHECK(allocator.GetCachedMemory() == 0);
		CHECK(live_bytes.load() == size);
		CHECK(live[0] == 99);
		CHECK(live[size - 1] == 77);
		BlockAllocatorTestHelper::FreeHuge(allocator, live, size);
		CHECK(allocator.GetCachedMemory() == size);
	}
	CHECK(live_bytes.load() == 0);
}

TEST_CASE("Allocation cache can reuse arena storage without container destruction", "[api][allocation_cache]") {
	constexpr idx_t SIZE = 512 * 1024;
	bool pinned = true;
#if defined(__linux__)
	AllocationCacheTestAffinity affinity;
	const auto cpus = affinity.CPUs();
	pinned = !cpus.empty() && affinity.Pin(cpus[0]);
#endif
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	config.block_allocator = make_uniq<BlockAllocator>(Allocator::DefaultAllocator(), 256 * 1024, 0);
	DuckDB db(nullptr, &config);
	auto &allocator = BufferManager::GetBufferManager(*db.instance).GetBufferAllocator();
	data_ptr_t retired;
	{
		ArenaAllocator arena(allocator, SIZE / 2);
		auto &values = *arena.Make<arena_vector<uint64_t>>(arena);
		values.reserve(1024);
		values.push_back(42);
		retired = arena.GetHead()->data.get();
	}
	auto data = allocator.Allocate(SIZE);
	if (pinned) {
		REQUIRE(data.get() == retired);
	}
	// Reusing the entire chunk must not inherit the vector's poisoned spare capacity.
	memset(data.get(), 42, SIZE);
	REQUIRE(data.get()[0] == 42);
	REQUIRE(data.get()[SIZE - 1] == 42);
}

#if defined(__linux__)
// Compute shards and construct the cache before pinning: musl's CPU count follows affinity.
static idx_t AllocationCacheTestShard(idx_t cpu) {
#ifndef DUCKDB_NO_THREADS
	const auto count = MaxValue<idx_t>(1, MinValue<idx_t>(256, (std::thread::hardware_concurrency() + 3) / 4));
	return (cpu / 4) % count;
#else
	return 0;
#endif
}

TEST_CASE("Allocation cache bounds retained bytes and shares surplus across CPU groups", "[api][allocation_cache]") {
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	{
		BlockAllocator allocator(fallback, 256 * 1024, 0);
		SECTION("A size class cannot retain more than its global byte limit") {
			constexpr idx_t SIZE = 64 * 1024 * 1024;
			vector<data_ptr_t> pointers;
			for (idx_t i = 0; i < 17; i++) {
				pointers.push_back(allocator.AllocateData(SIZE));
			}
			for (auto pointer : pointers) {
				allocator.FreeData(pointer, SIZE);
			}
			CHECK(live_bytes.load() == 16 * SIZE);
		}
		SECTION("Another CPU group borrows surplus but leaves local spares") {
			AllocationCacheTestAffinity affinity;
			const auto cpus = affinity.CPUs();
			if (cpus.empty()) {
				return;
			}
			auto other_cpu = cpus[0];
			for (auto cpu : cpus) {
				if (AllocationCacheTestShard(cpu) != AllocationCacheTestShard(cpus[0])) {
					other_cpu = cpu;
					break;
				}
			}
			if (other_cpu == cpus[0] || !affinity.Pin(cpus[0])) {
				return;
			}
			constexpr idx_t SIZE = 512 * 1024;
			constexpr idx_t COUNT = 64;
			std::unordered_set<data_ptr_t> original;
			for (idx_t i = 0; i < COUNT; i++) {
				original.insert(allocator.AllocateData(SIZE));
			}
			for (auto pointer : original) {
				allocator.FreeData(pointer, SIZE);
			}
			REQUIRE(affinity.Pin(other_cpu));
			vector<data_ptr_t> borrowed;
			idx_t reused = 0;
			for (idx_t i = 0; i <= COUNT; i++) {
				auto pointer = allocator.AllocateData(SIZE);
				reused += original.count(pointer);
				borrowed.push_back(pointer);
			}
			CHECK(reused > 0);
			CHECK(reused < COUNT);
			for (auto pointer : borrowed) {
				allocator.FreeData(pointer, SIZE);
			}
			REQUIRE(affinity.Pin(cpus[0]));
			auto local = allocator.AllocateData(SIZE);
			CHECK(original.count(local) == 1);
			allocator.FreeData(local, SIZE);
		}
	}
	CHECK(live_bytes.load() == 0);
}
#endif

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Block and allocation caches preserve ownership during concurrent reuse and reclamation",
          "[api][block_allocator][allocation_cache]") {
	const bool fixed_pool = GENERATE(false, true);
	bool toggle_pool = false;
	if (fixed_pool) {
		SECTION("Reclamation with the pool enabled") {
		}
		SECTION("Reclamation while the pool is disabled and re-enabled") {
			toggle_pool = true;
		}
	}
	const idx_t size = fixed_pool ? 65536 : 512 * 1024;
	constexpr idx_t WORKERS = 4;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	{
		BlockAllocator allocator(fallback, size, fixed_pool ? 128 * size : 0);
		mutex active_lock;
		std::unordered_set<data_ptr_t> active;
		atomic<bool> correct {true};
		atomic<bool> running {true};
		atomic<idx_t> ready {0};
		array<data_ptr_t, WORKERS> handed_off;
		auto live = allocator.AllocateData(size);
		memset(live, 84, size);
		active.insert(live);
		auto allocate = [&](idx_t id) {
			auto pointer = allocator.AllocateData(size);
			lock_guard<mutex> guard(active_lock);
			if (!active.insert(pointer).second) {
				correct = false;
				return data_ptr_t(nullptr);
			}
			if (fixed_pool && !toggle_pool && !BlockAllocatorTestHelper::IsInPool(allocator, pointer)) {
				correct = false;
			}
			pointer[0] = pointer[size - 1] = static_cast<uint8_t>(id);
			return pointer;
		};
		auto release = [&](data_ptr_t pointer, idx_t id) {
			if (!pointer) {
				return;
			}
			if (pointer[0] != id || pointer[size - 1] != id) {
				correct = false;
			}
			{
				lock_guard<mutex> guard(active_lock);
				if (active.erase(pointer) != 1) {
					correct = false;
				}
			}
			allocator.FreeData(pointer, size);
		};
		std::thread maintenance([&] {
			while (running.load()) {
				if (toggle_pool) {
					allocator.SetEnabled(false);
				}
				allocator.TryFlushDeallocated(NumericLimits<idx_t>::Maximum(), size);
				allocator.FlushAll();
				if (toggle_pool) {
					allocator.SetEnabled(true);
				}
				std::this_thread::yield();
			}
		});
		vector<std::thread> workers;
		for (idx_t id = 0; id < WORKERS; id++) {
			workers.emplace_back([&, id] {
				handed_off[id] = allocate(id);
				ready++;
				while (ready.load() != WORKERS) {
					std::this_thread::yield();
				}
				const auto other = (id + 1) % WORKERS;
				release(handed_off[other], other);
				for (idx_t i = 0; i < 64; i++) {
					auto pointer = allocate(id);
					std::this_thread::yield();
					release(pointer, id);
				}
			});
		}
		for (auto &worker : workers) {
			worker.join();
		}
		running = false;
		maintenance.join();
		CHECK(correct.load());
		CHECK(active.size() == 1);
		CHECK(live[0] == 84);
		CHECK(live[size - 1] == 84);
		allocator.FreeData(live, size);
	}
	CHECK(live_bytes.load() == 0);
}

#endif

TEST_CASE("Allocation cache pressure accounts for fixed backing and preserves live allocations",
          "[api][allocation_cache]") {
	constexpr idx_t BLOCK_SIZE = 256 * 1024;
	constexpr idx_t SIZE = 2 * BLOCK_SIZE;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	BlockAllocator allocator(fallback, BLOCK_SIZE, 4 * BLOCK_SIZE);
	auto pooled = allocator.AllocateData(BLOCK_SIZE);
	auto small = allocator.AllocateData(SIZE);
	auto large = allocator.AllocateData(2 * SIZE);
	auto live = allocator.AllocateData(4 * SIZE);
	live[0] = 42;
	live[4 * SIZE - 1] = 84;
	allocator.FreeData(pooled, BLOCK_SIZE);
	allocator.FreeData(small, SIZE);
	allocator.FreeData(large, 2 * SIZE);
	REQUIRE(allocator.GetCachedMemory() == BLOCK_SIZE + 3 * SIZE);
	REQUIRE(BlockAllocatorTestHelper::Deallocated(allocator) == 0);

	CHECK_FALSE(allocator.TryFlushDeallocated(1, BLOCK_SIZE + 3 * SIZE));
	CHECK(allocator.TryFlushDeallocated(NumericLimits<idx_t>::Maximum(), BLOCK_SIZE + SIZE));
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE + SIZE);
	CHECK(live_bytes.load() == 5 * SIZE);
	CHECK(live[0] == 42);
	CHECK(live[4 * SIZE - 1] == 84);
	allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), 0);
	CHECK(allocator.GetCachedMemory() == 0);
	CHECK(live_bytes.load() == 4 * SIZE);
	CHECK(live[0] == 42);
	CHECK(live[4 * SIZE - 1] == 84);
	allocator.FreeData(live, 4 * SIZE);
	allocator.FlushAll();
	CHECK(live_bytes.load() == 0);
}

TEST_CASE("BufferPool reclaims retained backing on small reservations and memory limit changes",
          "[api][block_allocator][allocation_cache]") {
	class TestBufferPool : public BufferPool {
	public:
		using BufferPool::BufferPool;
		using BufferPool::EvictBlocks;
	};
	constexpr idx_t MEMORY_LIMIT = 16 * 1024 * 1024;
	const bool cached = GENERATE(false, true);
	const idx_t freed_size = cached ? 512 * 1024 : MEMORY_LIMIT / 16 + 4096;
	Allocator fallback;
	BlockAllocator allocator(fallback, 65536, 0);
	TestBufferPool pool(allocator, MEMORY_LIMIT, false, MEMORY_LIMIT);
	auto pointer = allocator.AllocateData(freed_size);
	allocator.FreeData(pointer, freed_size);
	CHECK(allocator.GetCachedMemory() == (cached ? freed_size : 0));
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == (cached ? 0 : freed_size));
	pool.UpdateUsedMemory(MemoryTag::ALLOCATOR, MEMORY_LIMIT - freed_size);
	SECTION("A small reservation consumes retained backing's headroom") {
		auto result = pool.EvictBlocks(QueryContext(), MemoryTag::ALLOCATOR, 65536, MEMORY_LIMIT);
		CHECK(result.success);
		CHECK(allocator.GetCachedMemory() == 0);
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) ==
		      (cached || Allocator::SupportsFlush() ? 0 : freed_size));
	}
	SECTION("Lowering the limit releases retained backing") {
		pool.SetLimit(MEMORY_LIMIT - freed_size, "");
		CHECK(allocator.GetCachedMemory() == 0);
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 0);
	}
	pool.UpdateUsedMemory(MemoryTag::ALLOCATOR, -static_cast<int64_t>(MEMORY_LIMIT - freed_size));
	CHECK(pool.GetUsedMemory() == 0);
}

TEST_CASE("BlockAllocator fallback pressure shares headroom with cached blocks", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	Allocator fallback;
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_SIZE);
	auto pooled = allocator.AllocateData(BLOCK_SIZE);
	pooled[0] = 42;
	allocator.FreeData(pooled, BLOCK_SIZE);
	auto other = allocator.AllocateData(2 * BLOCK_SIZE);
	allocator.FreeData(other, 2 * BLOCK_SIZE);

	SECTION("The free threshold is independent of headroom") {
		CHECK_FALSE(allocator.TryFlushDeallocated(2 * BLOCK_SIZE + 1, BLOCK_SIZE));
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 2 * BLOCK_SIZE);
	}
	SECTION("Combined cached and fallback memory fits") {
		CHECK_FALSE(allocator.TryFlushDeallocated(2 * BLOCK_SIZE, 3 * BLOCK_SIZE));
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 2 * BLOCK_SIZE);
	}
	SECTION("Fallback purge preserves pooled backing that still fits") {
		CHECK(allocator.TryFlushDeallocated(2 * BLOCK_SIZE, 3 * BLOCK_SIZE - 1) == Allocator::SupportsFlush());
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == (Allocator::SupportsFlush() ? 0 : 2 * BLOCK_SIZE));
		CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
		auto reused = allocator.AllocateData(BLOCK_SIZE);
		CHECK(reused == pooled);
		CHECK(reused[0] == 42);
		allocator.FreeData(reused, BLOCK_SIZE);
	}
	SECTION("Pool-only reclamation does not reset fallback pressure") {
		BlockAllocatorTestHelper::FlushBytes(allocator, BLOCK_SIZE);
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 2 * BLOCK_SIZE);
	}
}

TEST_CASE("BlockAllocator bounds fallback pressure using available footprint statistics", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	// Keep this test on jemalloc rather than the power-of-two allocation cache.
	constexpr idx_t FALLBACK_SIZE = 1024 * 1024 + 4096;
	const bool mapped_statistics = GENERATE(false, true);
	allocate_function_ptr_t allocate = [](PrivateAllocatorData *data, idx_t size) {
		return Allocator::DefaultAllocate(data, size);
	};
	Allocator fallback(mapped_statistics ? Allocator::DefaultAllocate : allocate, Allocator::DefaultFree,
	                   Allocator::DefaultReallocate, nullptr);
	if (!mapped_statistics) {
		CHECK_FALSE(fallback.GetMappedMemory().IsValid());
		BlockAllocator allocator(fallback, BLOCK_SIZE, 0);
		for (idx_t i = 0; i < 4; i++) {
			auto pointer = allocator.AllocateData(BLOCK_SIZE);
			allocator.FreeData(pointer, BLOCK_SIZE);
		}
		CHECK(allocator.TryFlushDeallocated(BLOCK_SIZE, 3 * BLOCK_SIZE) == Allocator::SupportsFlush());
		CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == (Allocator::SupportsFlush() ? 0 : 4 * BLOCK_SIZE));
		return;
	}
	if (!fallback.GetMappedMemory().IsValid()) {
		return;
	}
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_SIZE);
	auto pooled = allocator.AllocateData(BLOCK_SIZE);
	pooled[0] = 42;
	allocator.FreeData(pooled, BLOCK_SIZE);
	// Allow room for arena initialization while repeatedly reusing the same small allocation.
	const auto headroom = 2 * fallback.GetMappedMemory().GetIndex() + 64 * FALLBACK_SIZE;
	for (idx_t freed = 0; freed <= headroom; freed += FALLBACK_SIZE) {
		auto pointer = allocator.AllocateData(FALLBACK_SIZE);
		pointer[0] = 42;
		allocator.FreeData(pointer, FALLBACK_SIZE);
	}
	REQUIRE(BlockAllocatorTestHelper::Deallocated(allocator) > headroom);
	CHECK_FALSE(allocator.TryFlushDeallocated(FALLBACK_SIZE, headroom));
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) >= FALLBACK_SIZE);
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) <= headroom - allocator.GetCachedMemory());

#ifndef DUCKDB_NO_THREADS
	atomic<bool> start {false};
	vector<std::thread> workers;
	for (idx_t t = 0; t < 4; t++) {
		workers.emplace_back([&] {
			while (!start.load()) {
				std::this_thread::yield();
			}
			for (idx_t i = 0; i < 16; i++) {
				auto pointer = allocator.AllocateData(FALLBACK_SIZE);
				allocator.FreeData(pointer, FALLBACK_SIZE);
			}
		});
	}
	start = true;
	for (idx_t i = 0; i < 16; i++) {
		CHECK_FALSE(allocator.TryFlushDeallocated(FALLBACK_SIZE, headroom));
	}
	for (auto &worker : workers) {
		worker.join();
	}
	CHECK_FALSE(allocator.TryFlushDeallocated(FALLBACK_SIZE, headroom));
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) >= FALLBACK_SIZE);
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) <= headroom - allocator.GetCachedMemory());
#endif
	// Headroom occupied by pooled backing cannot also cover the fallback's retained memory.
	CHECK(allocator.TryFlushDeallocated(FALLBACK_SIZE, BLOCK_SIZE));
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 0);
	pooled = allocator.AllocateData(BLOCK_SIZE);
	CHECK(pooled[0] == 42);
	allocator.FreeData(pooled, BLOCK_SIZE);
}

TEST_CASE("HugeAllocation moves retain the originating buffer manager", "[api][huge_route]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	DuckDB first_db(nullptr, &config);
	DuckDB second_db(nullptr, &config);
	Connection connection(first_db);
	auto &first_manager = BufferManager::GetBufferManager(*first_db.instance);
	auto &second_manager = BufferManager::GetBufferManager(*second_db.instance);
	auto &client_manager = BufferManager::GetBufferManager(*connection.context);
	const auto first_memory = first_manager.GetUsedMemory();
	const auto second_memory = second_manager.GetUsedMemory();
	constexpr idx_t SIZE = 512 * 1024;
	HugeAllocation empty;
	CHECK_FALSE(empty.IsSet());
	CHECK(empty.GetSize() == 0);
	empty.Reset();
	{
		auto first = client_manager.GetBufferAllocator().AllocateHuge(SIZE);
		auto second = second_manager.GetBufferAllocator().AllocateHuge(2 * SIZE);
		CHECK(first_manager.GetUsedMemory() == first_memory + SIZE);
		CHECK(second_manager.GetUsedMemory() == second_memory + 2 * SIZE);
		first.get()[0] = 42;
		first.get()[SIZE - 1] = 84;
		HugeAllocation moved(std::move(first));
		CHECK_FALSE(first.IsSet());
		CHECK(first.GetSize() == 0);
		second = std::move(moved);
		CHECK(second.GetSize() == SIZE);
		CHECK(second.get()[0] == 42);
		CHECK(second.get()[SIZE - 1] == 84);
		second.Reset();
		CHECK_FALSE(second.IsSet());
		CHECK(second.GetSize() == 0);
		second.Reset();
		CHECK(first_manager.GetUsedMemory() == first_memory);
		// Move assignment swaps ownership; moved still owns the second manager's allocation.
		CHECK(second_manager.GetUsedMemory() == second_memory + 2 * SIZE);
	}
	CHECK(second_manager.GetUsedMemory() == second_memory);
#ifndef DUCKDB_NO_THREADS
	auto cross_thread = client_manager.GetBufferAllocator().AllocateHuge(SIZE);
	std::thread receiver([owned = std::move(cross_thread)]() mutable { owned.Reset(); });
	receiver.join();
	CHECK_FALSE(cross_thread.IsSet());
	CHECK(first_manager.GetUsedMemory() == first_memory);
#endif
}

TEST_CASE("BufferAllocator accounts for ordinary and huge backing operations", "[api][huge_route]") {
	constexpr idx_t SIZE = 8 * 1024 * 1024 + 4096;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	auto &control = fallback.GetPrivateData()->Cast<AllocationCacheTestData>();
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	config.block_allocator = make_uniq<BlockAllocator>(fallback, 256 * 1024, 0);
	DuckDB db(nullptr, &config);
	Connection connection(db);
	auto &manager = BufferManager::GetBufferManager(*db.instance);
	auto &allocator = manager.GetBufferAllocator();
	const auto initial_memory = manager.GetUsedMemory();

	SECTION("Ordinary reallocation") {
		control.fail_allocate = true;
		REQUIRE_THROWS_AS(allocator.Allocate(SIZE), OutOfMemoryException);
		REQUIRE(manager.GetUsedMemory() == initial_memory);
		control.fail_allocate = false;
		auto owned = allocator.Allocate(SIZE);
		auto pointer = owned.get();
		auto &owner = *owned.GetAllocator();
		pointer[0] = 42;
		pointer[SIZE - 1] = 84;
		control.fail_reallocate = true;
		for (auto new_size : {SIZE / 2, SIZE * 2}) {
			REQUIRE_THROWS_AS(owner.ReallocateData(pointer, SIZE, new_size), OutOfMemoryException);
			REQUIRE(manager.GetUsedMemory() == initial_memory + SIZE);
			REQUIRE(owned.get() == pointer);
			REQUIRE(owned.GetAllocator().get() == &owner);
			REQUIRE(pointer[0] == 42);
			REQUIRE(pointer[SIZE - 1] == 84);
		}
		control.fail_reallocate = false;
		owned.Reset();
		REQUIRE(manager.GetUsedMemory() == initial_memory);
	}
	SECTION("Ordinary reallocation can grow and shrink above the memory limit") {
		REQUIRE_NO_FAIL(connection.Query("SET memory_limit='12MiB'"));
		auto current_size = SIZE;
		auto free_allocation = [&](data_ptr_t pointer) {
			allocator.FreeData(pointer, current_size);
		};
		unique_ptr<data_t, decltype(free_allocation)> owned(allocator.AllocateData(SIZE), free_allocation);
		owned.get()[0] = 42;
		owned.get()[SIZE / 2 - 1] = 84;
		for (auto new_size : {SIZE * 2, SIZE + SIZE / 2, SIZE / 2}) {
			auto pointer = allocator.ReallocateData(owned.get(), current_size, new_size);
			owned.release();
			owned.reset(pointer);
			current_size = new_size;
			CHECK(manager.GetUsedMemory() == initial_memory + current_size);
			CHECK(live_bytes.load() == current_size);
			CHECK(owned.get()[0] == 42);
			CHECK(owned.get()[SIZE / 2 - 1] == 84);
		}
		owned.reset();
		REQUIRE(manager.GetUsedMemory() == initial_memory);
		REQUIRE(live_bytes.load() == 0);
		REQUIRE_NO_FAIL(connection.Query("RESET memory_limit"));
	}
	SECTION("Huge replacement") {
		control.fail_allocate = true;
		REQUIRE_THROWS_AS(allocator.AllocateHuge(SIZE), OutOfMemoryException);
		REQUIRE(manager.GetUsedMemory() == initial_memory);
		REQUIRE(live_bytes.load() == 0);
		control.fail_allocate = false;
		auto owned = allocator.AllocateHuge(SIZE);
		auto pointer = owned.get();
		pointer[0] = 42;
		pointer[SIZE - 1] = 84;
		control.fail_allocate = true;
		for (auto new_size : {SIZE / 2, SIZE * 2}) {
			REQUIRE_THROWS_AS(owned = allocator.AllocateHuge(new_size), OutOfMemoryException);
			REQUIRE(manager.GetUsedMemory() == initial_memory + SIZE);
			REQUIRE(live_bytes.load() == SIZE);
			REQUIRE(owned.GetSize() == SIZE);
			REQUIRE(owned.get() == pointer);
			REQUIRE(pointer[0] == 42);
			REQUIRE(pointer[SIZE - 1] == 84);
		}
		control.fail_allocate = false;
		REQUIRE_NO_FAIL(connection.Query("SET memory_limit='12MiB'"));
		REQUIRE_THROWS_AS(owned = allocator.AllocateHuge(2 * SIZE), OutOfMemoryException);
		REQUIRE(manager.GetUsedMemory() == initial_memory + SIZE);
		REQUIRE(live_bytes.load() == SIZE);
		REQUIRE(owned.get() == pointer);
		REQUIRE(pointer[0] == 42);
		REQUIRE_NO_FAIL(connection.Query("RESET memory_limit"));
		owned = allocator.AllocateHuge(2 * SIZE);
		REQUIRE(owned.GetSize() == 2 * SIZE);
		REQUIRE(manager.GetUsedMemory() == initial_memory + 2 * SIZE);
		REQUIRE(live_bytes.load() == 2 * SIZE);
		owned.Reset();
		REQUIRE(manager.GetUsedMemory() == initial_memory);
		REQUIRE(live_bytes.load() == 0);
	}
}

TEST_CASE("BufferAllocator routes allocation transitions through BlockAllocator",
          "[api][block_allocator][huge_route]") {
	bool pinned = false;
#if defined(__linux__)
	AllocationCacheTestAffinity affinity;
	const auto cpus = affinity.CPUs();
	pinned = !cpus.empty() && affinity.Pin(cpus[0]);
#endif
	constexpr idx_t BLOCK_SIZE = 65536;
	Allocator fallback;
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	config.options.enable_block_allocator = true;
	config.block_allocator = make_uniq<BlockAllocator>(fallback, BLOCK_SIZE, BLOCK_SIZE);
	DuckDB db(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db.instance);
	auto &manager = BufferManager::GetBufferManager(*db.instance);
	auto &buffer_allocator = manager.GetBufferAllocator();
	allocator.FlushAll();
	const auto initial_memory = manager.GetUsedMemory();

	auto pointer = buffer_allocator.AllocateData(BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::IsInPool(allocator, pointer));
	CHECK(manager.GetUsedMemory() == initial_memory + BLOCK_SIZE);
	pointer[0] = 42;

	pointer = buffer_allocator.ReallocateData(pointer, BLOCK_SIZE, 2 * BLOCK_SIZE);
	CHECK_FALSE(BlockAllocatorTestHelper::IsInPool(allocator, pointer));
	CHECK(pointer[0] == 42);
	CHECK(manager.GetUsedMemory() == initial_memory + 2 * BLOCK_SIZE);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);

	pointer = buffer_allocator.ReallocateData(pointer, 2 * BLOCK_SIZE, BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::IsInPool(allocator, pointer));
	CHECK(pointer[0] == 42);
	CHECK(manager.GetUsedMemory() == initial_memory + BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 2 * BLOCK_SIZE);
	buffer_allocator.FreeData(pointer, BLOCK_SIZE);
	CHECK(manager.GetUsedMemory() == initial_memory);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);

	auto huge_pooled = buffer_allocator.AllocateHuge(BLOCK_SIZE);
	CHECK(huge_pooled.get() == pointer);
	huge_pooled.Reset();
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 2 * BLOCK_SIZE);
	constexpr idx_t CACHE_SIZE = 512 * 1024;
	auto cached = buffer_allocator.AllocateData(CACHE_SIZE);
	cached[0] = 42;
	buffer_allocator.FreeData(cached, CACHE_SIZE);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE + CACHE_SIZE);
	auto huge = buffer_allocator.AllocateHuge(CACHE_SIZE);
	if (pinned) {
		CHECK(huge.get() == cached);
		CHECK(huge.get()[0] == 42);
	}
	const auto retained = huge.get() == cached ? BLOCK_SIZE : BLOCK_SIZE + CACHE_SIZE;
	CHECK(allocator.GetCachedMemory() == retained);
	CHECK(manager.GetUsedMemory() == initial_memory + CACHE_SIZE);
	huge.Reset();
	CHECK(allocator.GetCachedMemory() == retained + CACHE_SIZE);
	CHECK(BlockAllocatorTestHelper::Deallocated(allocator) == 2 * BLOCK_SIZE);
	auto ordinary = buffer_allocator.AllocateData(CACHE_SIZE);
	if (pinned) {
		CHECK(ordinary == cached);
		CHECK(ordinary[0] == 42);
		CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
	}
	CHECK(manager.GetUsedMemory() == initial_memory + CACHE_SIZE);
	buffer_allocator.FreeData(ordinary, CACHE_SIZE);
	CHECK(manager.GetUsedMemory() == initial_memory);
}

TEST_CASE("BlockAllocator retention ages free volume", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 4096;
	constexpr idx_t CAPACITY = 4096;
	Allocator fallback;
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_SIZE * CAPACITY);
	BlockAllocatorTestHelper retention(allocator);
	CHECK(retention.IsLockFree());
	CHECK(allocator.DecayDelay().GetIndex() == 1);
	retention.Add(1024, 1);

	SECTION("One-second linear decay starts at the bucket ending boundary") {
		CHECK(retention.Target(500) == 1024);
		CHECK(retention.Target(501) == 1022);
		CHECK(retention.Target(1000) == 512);
		CHECK(retention.Target(1500) == 0);
	}
	SECTION("A return just before a boundary gets the full decay interval") {
		retention.Add(1024, 499);
		CHECK(retention.Target(500) == 2048);
		CHECK(retention.Target(501) == 2045);
		CHECK(retention.Target(1499) == 2);
		CHECK(retention.Target(1500) == 0);
	}
	SECTION("A full-pool burst decays without a percentage cutoff") {
		retention.Add(CAPACITY, 200);
		CHECK(retention.Target(500) == CAPACITY);
		CHECK(retention.Target(501) == 4091);
		CHECK(retention.Target(1000) == 2048);
		CHECK(retention.Target(1500) == 0);
	}
	SECTION("A small new burst does not refresh the old allowance") {
		retention.Add(16, 500);
		CHECK(retention.Target(750) == 768 + 16);
		CHECK(retention.Target(1500) == 8);
		CHECK(retention.Target(2000) == 0);
	}
	SECTION("Reuse consumes the youngest allowance first") {
		retention.Add(16, 500);
		retention.Reuse(16, 500);
		CHECK(retention.Target(750) == 768);
		CHECK(retention.Target(1500) == 0);
	}
	SECTION("Repeated small reuse cannot accumulate allowance") {
		retention.Add(16, 500);
		for (idx_t now = 600; now <= 3000; now += 100) {
			retention.Reuse(16, now);
			retention.Add(16, now);
		}
		CHECK(retention.Target(3000) == 16);
		CHECK(retention.Target(4500) == 0);
	}
	SECTION("A complete refill consumes the allowance including unused TLS blocks") {
		retention.Reuse(CAPACITY, 1);
		CHECK(retention.Target(200) == 0);
	}
	SECTION("Long gaps expire the whole ring without repeated advancement") {
		CHECK(retention.Target(1000000000) == 0);
		retention.Add(16, 1000000001);
		CHECK(retention.Target(1000000001) == 16);
		CHECK(retention.Target(1000001500) == 0);
	}
	SECTION("Historical purged volume cannot reject a new burst") {
		retention.Add(CAPACITY, 500);
		retention.Add(CAPACITY, 501);
		CHECK(retention.Target(501) == 1022 + CAPACITY);
		retention.Reuse(CAPACITY, 501);
		CHECK(retention.Target(750) == 768);
	}
	SECTION("Rollover rejects stale updates without reviving consumed generations") {
		constexpr idx_t ROLLOVER = (idx_t(1) << 32) * 500;
		BlockAllocatorTestHelper rollover(allocator, ROLLOVER - 500);
		rollover.Add(32, ROLLOVER - 499);
		rollover.Add(16, ROLLOVER);
		CHECK(rollover.Target(ROLLOVER) == 48);
		CHECK(rollover.Target(ROLLOVER + 500) == 32);
		CHECK(rollover.Target(ROLLOVER + 1000) == 8);
		CHECK(rollover.Target(ROLLOVER + 1500) == 0);
		rollover.Add(64, ROLLOVER + 1000);
		rollover.Add(CAPACITY, ROLLOVER - 500);
		CHECK(rollover.Target(ROLLOVER + 1000) == 72);
		rollover.Reuse(CAPACITY, ROLLOVER - 500);
		CHECK(rollover.Target(ROLLOVER + 1000) == 72);
		rollover.Reuse(64, ROLLOVER + 1000);
		rollover.Add(CAPACITY, ROLLOVER - 500);
		CHECK(rollover.Target(ROLLOVER + 1000) == 8);
		rollover.Add(32, ROLLOVER + 1000);
		CHECK(rollover.Target(ROLLOVER + 1000) == 40);
	}
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("BlockAllocator retention accounts concurrent batches", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t CAPACITY = 2048;
	constexpr idx_t WORKERS = 4;
	constexpr idx_t ITERATIONS = 16;
	constexpr idx_t BATCH = 16;
	Allocator fallback;
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_SIZE * CAPACITY);
	BlockAllocatorTestHelper retention(allocator);
	vector<std::thread> workers;
	for (idx_t i = 0; i < WORKERS; i++) {
		workers.emplace_back([&]() {
			for (idx_t batch = 0; batch < ITERATIONS; batch++) {
				retention.Add(BATCH, 1);
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	CHECK(retention.Target(1) == WORKERS * ITERATIONS * BATCH);
	workers.clear();
	for (idx_t i = 0; i < WORKERS; i++) {
		workers.emplace_back([&]() {
			for (idx_t batch = 0; batch < ITERATIONS; batch++) {
				retention.Reuse(BATCH, 1);
				retention.Add(BATCH, 1);
			}
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}
	CHECK(retention.Target(1) == WORKERS * ITERATIONS * BATCH);
	retention.Reuse(CAPACITY, 1);
	CHECK(retention.Target(1) == 0);
}
#endif

namespace {
struct BlockAllocatorFallbackData : public PrivateAllocatorData {
	atomic<idx_t> allocation_count {0};

	static data_ptr_t Allocate(PrivateAllocatorData *private_data, idx_t size) {
		private_data->Cast<BlockAllocatorFallbackData>().allocation_count++;
		return Allocator::DefaultAllocate(private_data, size);
	}
};
} // namespace

#ifndef DUCKDB_NO_THREADS
TEST_CASE("BlockAllocator concurrent trimming preserves the cache target", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 256;
	constexpr idx_t TARGET_BLOCKS = 32;
	Allocator fallback;
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_COUNT * BLOCK_SIZE);
	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < BLOCK_COUNT; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		block[0] = 42;
		blocks.push_back(block);
	}
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
	allocator.ThreadFlush(false, 0, 1);
	CHECK(allocator.GetCachedMemory() == BLOCK_COUNT * BLOCK_SIZE);
	atomic<idx_t> ready {0};
	atomic<bool> start {false};
	vector<std::thread> workers;
	for (idx_t t = 0; t < 4; t++) {
		workers.emplace_back([&]() {
			ready++;
			while (!start) {
				std::this_thread::yield();
			}
			allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), TARGET_BLOCKS * BLOCK_SIZE);
		});
	}
	while (ready != workers.size()) {
		std::this_thread::yield();
	}
	start = true;
	for (auto &worker : workers) {
		worker.join();
	}
	CHECK(allocator.GetCachedMemory() == TARGET_BLOCKS * BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == TARGET_BLOCKS);
	allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), 0);
	CHECK(allocator.GetCachedMemory() == 0);
}

TEST_CASE("BlockAllocator accounts for cached backing through reuse, thread exit and discard",
          "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t THREADS = 2;
	constexpr idx_t BLOCKS_PER_THREAD = 8;
	Allocator fallback;
	BlockAllocator allocator(fallback, BLOCK_SIZE, 64 * BLOCK_SIZE);
	CHECK(allocator.GetCachedMemory() == 0);
	auto block = allocator.AllocateData(BLOCK_SIZE);
	block[0] = 42;
	CHECK(allocator.GetCachedMemory() == 0);
	allocator.FreeData(block, BLOCK_SIZE);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == 0);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
	auto reused = allocator.AllocateData(BLOCK_SIZE);
	REQUIRE(reused == block);
	CHECK(reused[0] == 42);
	CHECK(allocator.GetCachedMemory() == 0);
	allocator.FreeData(reused, BLOCK_SIZE);
	allocator.ThreadFlush(false, 0, 1);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == 1);
	allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), BLOCK_SIZE);
	CHECK(allocator.GetCachedMemory() == BLOCK_SIZE);
	allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), 0);
	CHECK(allocator.GetCachedMemory() == 0);

	mutex gate;
	std::condition_variable cv;
	idx_t ready = 0;
	bool release = false;
	vector<std::thread> workers;
	for (idx_t t = 0; t < THREADS; t++) {
		workers.emplace_back([&]() {
			vector<data_ptr_t> blocks;
			for (idx_t b = 0; b < BLOCKS_PER_THREAD; b++) {
				blocks.push_back(allocator.AllocateData(BLOCK_SIZE));
			}
			for (auto block : blocks) {
				allocator.FreeData(block, BLOCK_SIZE);
			}
			unique_lock<mutex> guard(gate);
			ready++;
			cv.notify_all();
			cv.wait(guard, [&]() { return release; });
		});
	}
	{
		unique_lock<mutex> guard(gate);
		CHECK(cv.wait_for(guard, std::chrono::seconds(10), [&]() { return ready == THREADS; }));
	}
	CHECK(allocator.GetCachedMemory() == THREADS * BLOCKS_PER_THREAD * BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == 0);
	allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), 0);
	CHECK(allocator.GetCachedMemory() == THREADS * BLOCKS_PER_THREAD * BLOCK_SIZE);
	{
		lock_guard<mutex> guard(gate);
		release = true;
	}
	cv.notify_all();
	for (auto &worker : workers) {
		worker.join();
	}
	CHECK(allocator.GetCachedMemory() == THREADS * BLOCKS_PER_THREAD * BLOCK_SIZE);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == THREADS * BLOCKS_PER_THREAD);
	allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), 0);
	CHECK(allocator.GetCachedMemory() == 0);
}

TEST_CASE("BlockAllocator allocation bounds reclamation and does not wait for maintenance", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 256;
	auto private_data = make_uniq<BlockAllocatorFallbackData>();
	auto &fallback_data = *private_data;
	Allocator fallback(BlockAllocatorFallbackData::Allocate, Allocator::DefaultFree, Allocator::DefaultReallocate,
	                   std::move(private_data));
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_SIZE * BLOCK_COUNT);
	auto live = allocator.AllocateData(BLOCK_SIZE);
	live[0] = 84;
	live[BLOCK_SIZE - 1] = 84;
	vector<data_ptr_t> blocks;
	for (idx_t i = 1; i < BLOCK_COUNT; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		block[0] = 42;
		block[BLOCK_SIZE - 1] = 42;
		blocks.push_back(block);
	}
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
	BlockAllocatorTestHelper helper(allocator);
	SECTION("Allocation performs bounded opportunistic reclamation") {
		blocks.clear();
		BlockAllocatorTestHelper::Expire(allocator);
		blocks.push_back(allocator.AllocateData(BLOCK_SIZE));
		CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) > BLOCK_COUNT / 2);
		for (idx_t i = 2; i < BLOCK_COUNT; i++) {
			blocks.push_back(allocator.AllocateData(BLOCK_SIZE));
		}
		std::unordered_set<data_ptr_t> allocated {live};
		idx_t reclaimed = 0;
		for (auto block : blocks) {
			CHECK(allocated.insert(block).second);
			if (block[0] == 0 && block[BLOCK_SIZE - 1] == 0) {
				reclaimed++;
			}
		}
#if defined(__linux__) || defined(_WIN32)
		CHECK(reclaimed > 0);
#endif
		CHECK(reclaimed < BLOCK_COUNT - 1);
		CHECK(blocks[0][0] == 42);
		CHECK(blocks[0][BLOCK_SIZE - 1] == 42);
		for (auto block : blocks) {
			allocator.FreeData(block, BLOCK_SIZE);
		}
	}
	SECTION("Batch transfers continue while maintenance holds its lock") {
		BlockAllocatorTestHelper::Expire(allocator);
		mutex gate;
		std::condition_variable cv;
		bool finished = false;
		bool preserved = true;
		bool progressed;
		std::thread worker;
		{
			annotated_lock_guard<annotated_mutex> maintenance_guard(helper.MaintenanceLock());
			worker = std::thread([&] {
				for (idx_t round = 0; round < 2; round++) {
					vector<data_ptr_t> reused;
					for (idx_t i = 1; i < BLOCK_COUNT; i++) {
						auto block = allocator.AllocateData(BLOCK_SIZE);
						preserved &= block[0] == 42 && block[BLOCK_SIZE - 1] == 42;
						reused.push_back(block);
					}
					for (auto block : reused) {
						allocator.FreeData(block, BLOCK_SIZE);
					}
				}
				lock_guard<mutex> guard(gate);
				finished = true;
				cv.notify_all();
			});
			unique_lock<mutex> guard(gate);
			progressed = cv.wait_for(guard, std::chrono::seconds(10), [&] { return finished; });
		}
		// Release maintenance before joining, including if a transfer blocked.
		worker.join();
		CHECK(progressed);
		CHECK(preserved);
		CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == BLOCK_COUNT - 1);
	}
	CHECK(live[0] == 84);
	CHECK(live[BLOCK_SIZE - 1] == 84);
	CHECK(fallback_data.allocation_count == 0);
	allocator.FreeData(live, BLOCK_SIZE);
}
#endif

#if defined(__APPLE__)
TEST_CASE("BlockAllocator marks reused macOS pages as live", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 33;
	constexpr idx_t POOL_SIZE = BLOCK_SIZE * BLOCK_COUNT;
	auto private_data = make_uniq<BlockAllocatorFallbackData>();
	auto &fallback_data = *private_data;
	Allocator fallback(BlockAllocatorFallbackData::Allocate, Allocator::DefaultFree, Allocator::DefaultReallocate,
	                   std::move(private_data));
	BlockAllocator allocator(fallback, BLOCK_SIZE, POOL_SIZE);
	vector<data_ptr_t> blocks;
	for (idx_t iteration = 0; iteration < 2; iteration++) {
		const auto value = uint8_t(iteration + 1);
		for (idx_t i = 0; i < BLOCK_COUNT; i++) {
			auto block = allocator.AllocateData(BLOCK_SIZE);
			memset(block, value, BLOCK_SIZE);
			blocks.push_back(block);
		}
		for (auto block : blocks) {
			for (idx_t offset = 0; offset < BLOCK_SIZE; offset += vm_page_size) {
				int32_t disposition;
				int32_t reference_count;
				REQUIRE(mach_vm_page_query(mach_task_self(), reinterpret_cast<mach_vm_address_t>(block + offset),
				                           &disposition, &reference_count) == KERN_SUCCESS);
				CHECK((disposition & VM_PAGE_QUERY_PAGE_REUSABLE) == 0);
				CHECK(block[offset] == value);
			}
			allocator.FreeData(block, BLOCK_SIZE);
		}
		blocks.clear();
		allocator.FlushAll();
		CHECK(fallback_data.allocation_count == 0);
	}
}
#endif

TEST_CASE("BlockAllocator preserves cached blocks across allocator switches", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 4096;
	constexpr idx_t BLOCK_COUNT = 16;
	constexpr idx_t POOL_SIZE = BLOCK_SIZE * BLOCK_COUNT;

	auto private_data = make_uniq<BlockAllocatorFallbackData>();
	auto &fallback_data = *private_data;
	Allocator fallback(BlockAllocatorFallbackData::Allocate, Allocator::DefaultFree, Allocator::DefaultReallocate,
	                   std::move(private_data));
	BlockAllocator first(fallback, BLOCK_SIZE, POOL_SIZE);

	SECTION("Switch allocations and frees between live allocators") {
		BlockAllocator second(fallback, BLOCK_SIZE, POOL_SIZE);
		for (idx_t iteration = 0; iteration < 2; iteration++) {
			vector<data_ptr_t> first_blocks;
			vector<data_ptr_t> second_blocks;
			for (idx_t i = 0; i < BLOCK_COUNT; i++) {
				first_blocks.push_back(first.AllocateData(BLOCK_SIZE));
				second_blocks.push_back(second.AllocateData(BLOCK_SIZE));
				first_blocks.back()[0] = uint8_t(i + 1);
				second_blocks.back()[0] = uint8_t(i + BLOCK_COUNT + 1);
			}
			for (idx_t i = 0; i < BLOCK_COUNT; i++) {
				CHECK(first_blocks[i][0] == i + 1);
				CHECK(second_blocks[i][0] == i + BLOCK_COUNT + 1);
				first.FreeData(first_blocks[i], BLOCK_SIZE);
				second.FreeData(second_blocks[i], BLOCK_SIZE);
			}
			CHECK(fallback_data.allocation_count == 0);
		}
	}

	SECTION("Destroy another allocator while the first has cached blocks") {
		auto pointer = first.AllocateData(BLOCK_SIZE);
		first.FreeData(pointer, BLOCK_SIZE);
		{ BlockAllocator second(fallback, BLOCK_SIZE, POOL_SIZE); }
		vector<data_ptr_t> blocks;
		for (idx_t i = 0; i < BLOCK_COUNT; i++) {
			blocks.push_back(first.AllocateData(BLOCK_SIZE));
		}
		for (auto block : blocks) {
			first.FreeData(block, BLOCK_SIZE);
		}
		CHECK(fallback_data.allocation_count == 0);
	}
}

TEST_CASE("BlockAllocator reclaims only free blocks and preserves pool capacity", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 1103;
	constexpr idx_t POOL_SIZE = BLOCK_SIZE * BLOCK_COUNT;
	auto private_data = make_uniq<BlockAllocatorFallbackData>();
	auto &fallback_data = *private_data;
	Allocator fallback(BlockAllocatorFallbackData::Allocate, Allocator::DefaultFree, Allocator::DefaultReallocate,
	                   std::move(private_data));
	BlockAllocator allocator(fallback, BLOCK_SIZE, POOL_SIZE);
	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < BLOCK_COUNT; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		block[0] = 42;
		block[BLOCK_SIZE - 1] = 42;
		blocks.push_back(block);
	}
	auto live_first = blocks[3];
	auto live_second = blocks[1030];
	for (auto block : blocks) {
		if (block != live_first && block != live_second) {
			allocator.FreeData(block, BLOCK_SIZE);
		}
	}

	allocator.ThreadFlush(false, 0, 1);
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 2) * BLOCK_SIZE);
	BlockAllocatorTestHelper::FlushBytes(allocator, 0);
	BlockAllocatorTestHelper::FlushBytes(allocator, BLOCK_SIZE - 1);
	allocator.FlushForAllocation(BLOCK_SIZE - 1, 0);
	allocator.FlushForAllocation(POOL_SIZE, POOL_SIZE);
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 2) * BLOCK_SIZE);

	idx_t expected_reclaimed = 0;
	SECTION("Byte-limited flush rounds down to whole blocks") {
		expected_reclaimed = 17;
		BlockAllocatorTestHelper::FlushBytes(allocator, expected_reclaimed * BLOCK_SIZE + BLOCK_SIZE / 2);
	}
	SECTION("Allocation reclaims only backing exceeding headroom") {
		expected_reclaimed = 17;
		allocator.FlushForAllocation(POOL_SIZE, (BLOCK_COUNT - 2 - expected_reclaimed) * BLOCK_SIZE + BLOCK_SIZE / 2);
	}
	SECTION("Allocation reclamation is bounded by the requested size") {
		expected_reclaimed = 17;
		allocator.FlushForAllocation(expected_reclaimed * BLOCK_SIZE + BLOCK_SIZE / 2, 0);
	}
	SECTION("A target smaller than a block releases all free backing") {
		expected_reclaimed = BLOCK_COUNT - 2;
		allocator.FlushForAllocation(NumericLimits<idx_t>::Maximum(), BLOCK_SIZE - 1);
	}
	SECTION("A complete flush spans reclamation batches") {
		expected_reclaimed = BLOCK_COUNT - 2;
		allocator.FlushAll();
	}
	SECTION("Discard does not consume partially decayed retention history") {
		BlockAllocatorTestHelper retention(allocator);
		retention.Add(BLOCK_COUNT - 2, 1);
		const auto target = retention.Target(1000);
		expected_reclaimed = BLOCK_COUNT - 2 - target;
		BlockAllocatorTestHelper::FlushBytes(allocator, expected_reclaimed * BLOCK_SIZE);
		CHECK(retention.Target(1000) == target);
		CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == target);
		CHECK(allocator.GetCachedMemory() == target * BLOCK_SIZE);
		allocator.FreeData(live_first, BLOCK_SIZE);
		allocator.FreeData(live_second, BLOCK_SIZE);
		return;
	}
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 2 - expected_reclaimed) * BLOCK_SIZE);

	std::unordered_set<data_ptr_t> allocated {live_first, live_second};
	blocks.clear();
	idx_t reclaimed = 0;
	for (idx_t i = 0; i < BLOCK_COUNT - 2; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		CHECK(allocated.insert(block).second);
		if (block[0] == 0 && block[BLOCK_SIZE - 1] == 0) {
			reclaimed++;
		}
		blocks.push_back(block);
	}
#if defined(__linux__) || defined(_WIN32)
	CHECK(reclaimed == expected_reclaimed);
#else
	CHECK(reclaimed <= expected_reclaimed);
#endif
	CHECK(live_first[0] == 42);
	CHECK(live_first[BLOCK_SIZE - 1] == 42);
	CHECK(live_second[0] == 42);
	CHECK(live_second[BLOCK_SIZE - 1] == 42);
	CHECK(fallback_data.allocation_count == 0);
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
	allocator.FreeData(live_first, BLOCK_SIZE);
	allocator.FreeData(live_second, BLOCK_SIZE);
}

#ifndef DUCKDB_NO_THREADS

namespace {
class BlockAllocatorAsyncGate {
public:
	explicit BlockAllocatorAsyncGate(TaskScheduler &scheduler) : executor(scheduler, TaskSchedulerType::ASYNC) {
		executor.ScheduleTask(make_uniq<GateTask>(executor, *this));
	}
	~BlockAllocatorAsyncGate() {
		Release();
		executor.CancelAndDrain();
	}

	bool WaitUntilBlocked() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(10), [&]() { return blocked; });
	}
	void Release() {
		lock_guard<mutex> guard(lock);
		released = true;
		cv.notify_all();
	}

private:
	class GateTask : public BaseExecutorTask {
	public:
		GateTask(TaskExecutor &executor, BlockAllocatorAsyncGate &gate) : BaseExecutorTask(executor), gate(gate) {
		}
		void ExecuteTask() override {
			unique_lock<mutex> guard(gate.lock);
			gate.blocked = true;
			gate.cv.notify_all();
			gate.cv.wait(guard, [&]() { return gate.released; });
		}

	private:
		BlockAllocatorAsyncGate &gate;
	};

	TaskExecutor executor;
	mutex lock;
	std::condition_variable cv;
	bool blocked = false;
	bool released = false;
};
} // namespace

TEST_CASE("Allocation cache idle decay uses bounded tasks and preserves active size classes",
          "[api][allocation_cache]") {
	constexpr idx_t SIZE = 512 * 1024;
	constexpr idx_t COUNT = 40;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 1;
	config.block_allocator = make_uniq<BlockAllocator>(fallback, 256 * 1024, 0);
	DuckDB db(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db.instance);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	SECTION("The idle worker revisits a return after its last task") {
		auto pointer = allocator.AllocateData(SIZE);
		{
			BlockAllocatorAsyncGate gate(scheduler);
			REQUIRE(gate.WaitUntilBlocked());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(650));
		allocator.FreeData(pointer, SIZE);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
		while (allocator.GetCachedMemory() != 0 && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		CHECK(allocator.GetCachedMemory() == 0);
	}
	SECTION("Controlled execution bounds reclamation and protects refreshed size classes") {
		BlockAllocatorAsyncGate gate(scheduler);
		REQUIRE(gate.WaitUntilBlocked());

		vector<data_ptr_t> blocks;
		for (idx_t i = 0; i < COUNT; i++) {
			blocks.push_back(allocator.AllocateData(SIZE));
		}
		for (auto block : blocks) {
			allocator.FreeData(block, SIZE);
		}
		REQUIRE(allocator.GetCachedMemory() == COUNT * SIZE);
		allocator.ThreadIdle();
		CHECK(scheduler.GetNumberOfTasks() == 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		allocator.ThreadIdle();
		REQUIRE(scheduler.GetNumberOfTasks() == 1);
		atomic<bool> execute {true};
		REQUIRE(scheduler.ExecuteTasks(&execute, 1) == 1);
		const auto remaining = allocator.GetCachedMemory();
		CHECK(remaining > 0);
		CHECK(remaining < COUNT * SIZE);
		REQUIRE(scheduler.GetNumberOfTasks() == 1);
		SECTION("Continuations drain expired backing") {
			CHECK(scheduler.ExecuteTasks(&execute, COUNT) > 0);
			CHECK(allocator.GetCachedMemory() == 0);
		}
		SECTION("A new return keeps its size class available") {
			auto pointer = allocator.AllocateData(SIZE);
			allocator.FreeData(pointer, SIZE);
			const auto refreshed = allocator.GetCachedMemory();
			CHECK(scheduler.ExecuteTasks(&execute, COUNT) == 1);
			CHECK(allocator.GetCachedMemory() == refreshed);
			allocator.FlushAll();
		}
		CHECK(scheduler.GetNumberOfTasks() == 0);
		CHECK(live_bytes.load() == 0);
	}
}

TEST_CASE("BlockAllocator coalesces idle requests and yields between bounded async tasks", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 257;
	constexpr idx_t POOL_SIZE = BLOCK_SIZE * BLOCK_COUNT;
	Allocator fallback;
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 1;
	config.options.enable_block_allocator = true;
	config.block_allocator = make_uniq<BlockAllocator>(fallback, BLOCK_SIZE, POOL_SIZE);
	DuckDB db(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db.instance);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	BlockAllocatorAsyncGate gate(scheduler);
	REQUIRE(gate.WaitUntilBlocked());

	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < BLOCK_COUNT; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		block[0] = 42;
		block[BLOCK_SIZE - 1] = 42;
		blocks.push_back(block);
	}
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
	blocks.clear();
	auto other_producer = scheduler.CreateProducer();
	BlockAllocatorTestHelper::Expire(allocator);
	allocator.ThreadExit();
	CHECK(scheduler.GetNumberOfTasks() == 0);
	for (idx_t i = 0; i < 4; i++) {
		allocator.ThreadIdle();
	}
	CHECK(scheduler.GetNumberOfTasks() == 1);
	scheduler.RelaunchThreads();
	allocator.ThreadIdle();
	CHECK(scheduler.GetNumberOfTasks() == 1);
	atomic<bool> execute {true};
	REQUIRE(scheduler.ExecuteTasks(&execute, 1) == 1);
	CHECK(scheduler.GetNumberOfTasks() == 1);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) > 0);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) < BLOCK_COUNT);

	SECTION("One task leaves warm blocks available for reuse") {
		idx_t reclaimed = 0;
		for (idx_t i = 0; i < BLOCK_COUNT; i++) {
			auto block = allocator.AllocateData(BLOCK_SIZE);
			if (block[0] == 0 && block[BLOCK_SIZE - 1] == 0) {
				reclaimed++;
			}
			blocks.push_back(block);
		}
#if defined(__linux__) || defined(_WIN32)
		CHECK(reclaimed > 0);
#endif
		CHECK(reclaimed < BLOCK_COUNT);
		// The continuation must not discard blocks that have since been allocated.
		for (auto block : blocks) {
			block[0] = 84;
		}
		CHECK(scheduler.ExecuteTasks(&execute, BLOCK_COUNT) == 1);
		for (auto block : blocks) {
			CHECK(block[0] == 84);
		}
	}
	SECTION("Continuations finish the backlog without another idle request") {
		CHECK(scheduler.ExecuteTasks(&execute, BLOCK_COUNT) > 0);
		CHECK(scheduler.GetNumberOfTasks() == 0);
		CHECK(BlockAllocatorTestHelper::Drained(allocator));
	}
	SECTION("A fresh burst limits the stale continuation budget") {
		for (idx_t i = 0; i < BLOCK_COUNT; i++) {
			auto block = allocator.AllocateData(BLOCK_SIZE);
			block[0] = 84;
			blocks.push_back(block);
		}
		for (auto block : blocks) {
			allocator.FreeData(block, BLOCK_SIZE);
		}
		blocks.clear();
		allocator.ThreadFlush(false, 0, 1);
		CHECK(scheduler.ExecuteTasks(&execute, BLOCK_COUNT) > 0);
		CHECK(scheduler.GetNumberOfTasks() == 0);
		const auto retained = BlockAllocatorTestHelper::RetainedBlocks(allocator);
		REQUIRE(retained > 0);
		CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) >= retained);
	}
	SECTION("Pending work stops the continuation chain") {
		class OtherTask : public Task {
		public:
			explicit OtherTask(bool &executed) : executed(executed) {
			}
			TaskExecutionResult Execute(TaskExecutionMode) override {
				executed = true;
				return TaskExecutionResult::TASK_FINISHED;
			}

		private:
			bool &executed;
		};
		bool executed = false;
		scheduler.ScheduleTask(*other_producer, make_shared_ptr<OtherTask>(executed), TaskSchedulerType::ASYNC);
		// Either queue order is valid; at most the current drain precedes other work.
		CHECK(scheduler.ExecuteTasks(&execute, 2) == 2);
		CHECK(executed);
		// After contention, an idle request can resume any deferred reclamation.
		allocator.ThreadIdle();
		scheduler.ExecuteTasks(&execute, BLOCK_COUNT);
		CHECK(scheduler.GetNumberOfTasks() == 0);
	}
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
}

TEST_CASE("BlockAllocator queued reclamation survives worker reconfiguration and shutdown", "[api][block_allocator]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 1;
	config.options.enable_block_allocator = true;
	config.block_allocator = make_uniq<BlockAllocator>(Allocator::DefaultAllocator(), DEFAULT_BLOCK_ALLOC_SIZE,
	                                                   128 * DEFAULT_BLOCK_ALLOC_SIZE);
	auto db = make_uniq<DuckDB>(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db->instance);
	auto &scheduler = TaskScheduler::GetScheduler(*db->instance);
	{
		BlockAllocatorAsyncGate gate(scheduler);
		REQUIRE(gate.WaitUntilBlocked());
		vector<data_ptr_t> blocks;
		for (idx_t i = 0; i < 128; i++) {
			blocks.push_back(allocator.AllocateData(DEFAULT_BLOCK_ALLOC_SIZE));
		}
		for (auto block : blocks) {
			allocator.FreeData(block, DEFAULT_BLOCK_ALLOC_SIZE);
		}
		BlockAllocatorTestHelper::Expire(allocator);
		allocator.ThreadIdle();
		CHECK(scheduler.GetNumberOfTasks() == 1);
		gate.Release();
		SECTION("Queued work remains executable after removing the worker") {
			scheduler.SetAsyncThreads(0);
			scheduler.RelaunchThreads();
			CHECK(scheduler.NumberOfAsyncThreads() == 0);
			atomic<bool> execute {true};
			scheduler.ExecuteTasks(&execute, 128);
			allocator.ThreadIdle();
			CHECK(BlockAllocatorTestHelper::Drained(allocator));
			scheduler.SetAsyncThreads(1);
			scheduler.RelaunchThreads();
			CHECK(scheduler.NumberOfAsyncThreads() == 1);
		}
		SECTION("Shutdown can race with the released reclamation work") {
		}
	}
	CHECK_NOTHROW(db.reset());
}

TEST_CASE("BlockAllocator idle workers reclaim eligible blocks", "[api][block_allocator]") {
	DBConfig config;
	config.options.maximum_threads = 2;
	config.options.async_threads = 0;
	config.options.enable_block_allocator = true;
	config.block_allocator = make_uniq<BlockAllocator>(Allocator::DefaultAllocator(), DEFAULT_BLOCK_ALLOC_SIZE,
	                                                   128 * DEFAULT_BLOCK_ALLOC_SIZE);
	SECTION("A regular worker performs synchronous idle maintenance") {
	}
	SECTION("An async worker performs bounded idle maintenance") {
		config.options.maximum_threads = 1;
		config.options.async_threads = 1;
	}
	DuckDB db(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db.instance);
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	BlockAllocatorAsyncGate gate(scheduler);
	REQUIRE(gate.WaitUntilBlocked());
	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < 128; i++) {
		auto block = allocator.AllocateData(DEFAULT_BLOCK_ALLOC_SIZE);
		block[0] = 42;
		blocks.push_back(block);
	}
	for (auto block : blocks) {
		allocator.FreeData(block, DEFAULT_BLOCK_ALLOC_SIZE);
	}
	BlockAllocatorTestHelper::Expire(allocator);
	CHECK(scheduler.GetNumberOfTasks() == 0);
	CHECK(BlockAllocatorTestHelper::FreeBlocks(allocator) == 128);
	gate.Release();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!BlockAllocatorTestHelper::Drained(allocator) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(BlockAllocatorTestHelper::Drained(allocator));
	allocator.ThreadIdle();
	CHECK(scheduler.GetNumberOfTasks() == 0);
}

#endif

#if defined(__linux__)
TEST_CASE("BlockAllocator preserves failed discards without spinning and can retry", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 16;
	constexpr idx_t POOL_SIZE = BLOCK_SIZE * BLOCK_COUNT;
	Allocator fallback;
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	config.options.enable_block_allocator = true;
#ifndef DUCKDB_NO_THREADS
	config.options.async_threads = 1;
#endif
	config.block_allocator = make_uniq<BlockAllocator>(fallback, BLOCK_SIZE, POOL_SIZE);
	DuckDB db(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db.instance);
#ifndef DUCKDB_NO_THREADS
	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	BlockAllocatorAsyncGate gate(scheduler);
	REQUIRE(gate.WaitUntilBlocked());
#endif
	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < BLOCK_COUNT; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		REQUIRE(BlockAllocatorTestHelper::IsInPool(allocator, block));
		memset(block, 42, BLOCK_SIZE);
		blocks.push_back(block);
	}
	std::sort(blocks.begin(), blocks.end());
	auto live_first = blocks[4];
	auto live_second = blocks[12];
	auto locked = blocks[8];
	// Linux rejects MADV_DONTNEED for locked pages, leaving the mapping accessible.
	// Sanitizers replace mlock/munlock with no-ops, so call the kernel directly.
	if (syscall(SYS_mlock, locked, 1) != 0) {
		WARN("Cannot lock a page to exercise discard failure");
		for (auto block : blocks) {
			allocator.FreeData(block, BLOCK_SIZE);
		}
		return;
	}
	for (auto block : blocks) {
		if (block != live_first && block != live_second) {
			allocator.FreeData(block, BLOCK_SIZE);
		}
	}
	STATIC_REQUIRE(noexcept(allocator.FlushAll()));
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 2) * BLOCK_SIZE);
	CHECK_NOTHROW(BlockAllocatorTestHelper::FlushBytes(allocator, POOL_SIZE));
	// The first contiguous range succeeded; the failed range and remaining blocks stay charged.
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 6) * BLOCK_SIZE);
	CHECK_NOTHROW(allocator.FlushAll());
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 6) * BLOCK_SIZE);
#ifndef DUCKDB_NO_THREADS
	BlockAllocatorTestHelper::Expire(allocator);
	allocator.ThreadIdle();
	atomic<bool> execute {true};
	CHECK(scheduler.ExecuteTasks(&execute, 32) == 1);
	CHECK(scheduler.GetNumberOfTasks() == 0);
	CHECK(allocator.GetCachedMemory() == (BLOCK_COUNT - 6) * BLOCK_SIZE);
	CHECK(locked[0] == 42);
#endif
	CHECK(syscall(SYS_munlock, locked, 1) == 0);
#ifndef DUCKDB_NO_THREADS
	allocator.ThreadIdle();
	CHECK(scheduler.ExecuteTasks(&execute, 32) == 1);
	CHECK(scheduler.GetNumberOfTasks() == 0);
#else
	allocator.FlushAll();
#endif
	CHECK(allocator.GetCachedMemory() == 0);
	std::unordered_set<data_ptr_t> allocated {live_first, live_second};
	blocks.clear();
	for (idx_t i = 0; i < BLOCK_COUNT - 2; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		CHECK(BlockAllocatorTestHelper::IsInPool(allocator, block));
		CHECK(allocated.insert(block).second);
		CHECK(block[0] == 0);
		CHECK(block[BLOCK_SIZE - 1] == 0);
		blocks.push_back(block);
	}
	CHECK(live_first[0] == 42);
	CHECK(live_second[0] == 42);
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
	allocator.FreeData(live_first, BLOCK_SIZE);
	allocator.FreeData(live_second, BLOCK_SIZE);
}

TEST_CASE("BlockAllocator database shutdown unmaps locked pool pages", "[api][block_allocator]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	config.options.enable_block_allocator = true;
	config.block_allocator = make_uniq<BlockAllocator>(Allocator::DefaultAllocator(), DEFAULT_BLOCK_ALLOC_SIZE,
	                                                   4 * DEFAULT_BLOCK_ALLOC_SIZE);
	auto db = make_uniq<DuckDB>(nullptr, &config);
	auto &allocator = BlockAllocator::Get(*db->instance);
	auto block = allocator.AllocateData(DEFAULT_BLOCK_ALLOC_SIZE);
	block[0] = 42;
	if (syscall(SYS_mlock, block, 1) != 0) {
		WARN("Cannot lock a page to exercise unmapping during shutdown");
		allocator.FreeData(block, DEFAULT_BLOCK_ALLOC_SIZE);
		return;
	}
	allocator.FreeData(block, DEFAULT_BLOCK_ALLOC_SIZE);
	// Unmapping the pool during destruction also releases the page lock.
	CHECK_NOTHROW(db.reset());
	unsigned char residency;
	CHECK(mincore(block, 1, &residency) == -1);
	CHECK(errno == ENOMEM);
}
#endif
#endif

#ifndef DUCKDB_NO_THREADS
TEST_CASE("BlockAllocator destruction is safe before thread exit and during allocator switching",
          "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 4096;
	constexpr idx_t POOL_SIZE = BLOCK_SIZE * 16;
	const bool switch_concurrently = GENERATE(false, true);
	Allocator fallback;
	BlockAllocator next(fallback, BLOCK_SIZE, POOL_SIZE);
	for (idx_t iteration = 0; iteration < 8; iteration++) {
		auto previous = make_uniq<BlockAllocator>(fallback, BLOCK_SIZE, POOL_SIZE);
		atomic<bool> ready {false};
		atomic<bool> proceed {false};
		std::thread worker([&] {
			auto pointer = previous->AllocateData(BLOCK_SIZE);
			previous->FreeData(pointer, BLOCK_SIZE);
			ready = true;
			while (!proceed.load()) {
				std::this_thread::yield();
			}
			if (switch_concurrently) {
				pointer = next.AllocateData(BLOCK_SIZE);
				next.FreeData(pointer, BLOCK_SIZE);
			}
		});
		while (!ready.load()) {
			std::this_thread::yield();
		}
		if (switch_concurrently) {
			proceed = true;
			previous.reset();
		} else {
			previous.reset();
			proceed = true;
		}
		worker.join();
	}
}
#endif

TEST_CASE("Active pool reuse expires old allocation classes and preserves fresh classes", "[api][allocation_cache]") {
	if (sizeof(void *) != 8) {
		return;
	}
	constexpr idx_t BLOCK_SIZE = 65536;
	constexpr idx_t BLOCK_COUNT = 32;
	constexpr idx_t OLD_SIZE = 512 * 1024;
	constexpr idx_t FRESH_SIZE = 1024 * 1024;
	atomic<idx_t> live_bytes {0};
	Allocator fallback(CacheTestAllocate, CacheTestFree, CacheTestReallocate,
	                   make_uniq<AllocationCacheTestData>(live_bytes));
	BlockAllocator allocator(fallback, BLOCK_SIZE, BLOCK_COUNT * BLOCK_SIZE);
	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < BLOCK_COUNT; i++) {
		auto block = allocator.AllocateData(BLOCK_SIZE);
		block[0] = 42;
		blocks.push_back(block);
	}
	for (auto block : blocks) {
		allocator.FreeData(block, BLOCK_SIZE);
	}
	allocator.ThreadFlush(false, 0, 1);
	blocks.clear();
	for (idx_t i = 0; i < 4; i++) {
		blocks.push_back(allocator.AllocateData(OLD_SIZE));
	}
	for (auto block : blocks) {
		allocator.FreeData(block, OLD_SIZE);
	}
	REQUIRE(live_bytes.load() == 4 * OLD_SIZE);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	auto fresh = allocator.AllocateData(FRESH_SIZE);
	allocator.FreeData(fresh, FRESH_SIZE);
	REQUIRE(live_bytes.load() == 4 * OLD_SIZE + FRESH_SIZE);
	auto reused = allocator.AllocateData(BLOCK_SIZE);
	CHECK(reused[0] == 42);
	CHECK(live_bytes.load() == FRESH_SIZE);
	allocator.FreeData(reused, BLOCK_SIZE);
	allocator.FlushAll();
	CHECK(live_bytes.load() == 0);
}
