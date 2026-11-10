#include "duckdb/storage/block_allocator.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/thread.hpp"

#include <new>

#if defined(__linux__) && INTPTR_MAX == INT64_MAX
#include <cinttypes>
#include <cstdio>
#include <unistd.h>
#endif
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/concurrentqueue.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/chrono.hpp"

#if defined(_WIN32)
#include "duckdb/common/windows.hpp"
#else
#include <sys/mman.h>
#endif
#ifdef __MVS__
#include <zos-tls.h>
#endif

namespace duckdb {

template <class FUNC>
static data_ptr_t RetryAfterReclaiming(const BlockAllocator &block_allocator, FUNC &&allocate) {
	try {
		return allocate();
	} catch (const std::bad_alloc &) {
	} catch (const OutOfMemoryException &) {
	}
	// Cached backing must not cause an otherwise avoidable allocation failure.
	block_allocator.FlushAll();
	return allocate();
}

struct BlockAllocatorConfig {
	//! Blocks transferred between thread-local caches and global queues.
	static constexpr idx_t BATCH_SIZE = 16;
	//! Cached freed blocks that trigger a transfer to the global queue.
	static constexpr idx_t FREE_THRESHOLD = BATCH_SIZE * 2;
	//! Idle decay delay in seconds when the fallback allocator supplies none.
	static constexpr idx_t DEFAULT_DECAY_DELAY = 1;
	//! Maximum blocks sorted and coalesced in one reclamation batch.
	static constexpr idx_t RECLAIM_BATCH_SIZE = 1024;
	//! Maximum blocks reclaimed by one background task.
	static constexpr idx_t MAX_FLUSH_BLOCKS = 64;
	//! Width of each retention bucket, rounding return times up to the bucket end.
	static constexpr idx_t RETENTION_INTERVAL_MS = 500;
	//! Minimum interval between bounded decay checks on each allocating thread.
	static constexpr idx_t ACTIVE_DECAY_INTERVAL_MS = RETENTION_INTERVAL_MS;
	//! Linear decay on the same one-second timescale as bundled jemalloc, without extra grace.
	static constexpr idx_t RETENTION_DECAY_MS = DEFAULT_DECAY_DELAY * 1000;
	//! Current bucket plus the complete decay history.
	static constexpr idx_t RETENTION_BUCKETS = RETENTION_DECAY_MS / RETENTION_INTERVAL_MS + 1;
};

static idx_t RetentionTimeMillis() {
	return NumericCast<idx_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

static uint64_t PackRetentionBucket(const uint32_t epoch, const uint32_t count) {
	return (uint64_t(epoch) << 32) | count;
}

static uint32_t RetentionBucketEpoch(const uint64_t bucket) {
	return static_cast<uint32_t>(bucket >> 32);
}

static uint32_t RetentionBucketCount(const uint64_t bucket) {
	return static_cast<uint32_t>(bucket);
}

void BlockAllocator::InitializeRetention(const idx_t now_ms) const {
	const auto epoch = now_ms / BlockAllocatorConfig::RETENTION_INTERVAL_MS;
	for (idx_t age = 0; age < retention_buckets.size(); age++) {
		auto &bucket = retention_buckets[(epoch % retention_buckets.size() + retention_buckets.size() - age) %
		                                 retention_buckets.size()];
		bucket.store(PackRetentionBucket(static_cast<uint32_t>(epoch - age), 0), std::memory_order_relaxed);
	}
}

void BlockAllocator::AddRetention(const idx_t count, const idx_t now_ms) const {
	const auto epoch = now_ms / BlockAllocatorConfig::RETENTION_INTERVAL_MS;
	const auto capacity = DivBlockSize(pool_size);
	auto &bucket = retention_buckets[epoch % retention_buckets.size()];
	auto current = bucket.load(std::memory_order_relaxed);
	do {
		const auto elapsed = static_cast<uint32_t>(epoch - RetentionBucketEpoch(current));
		// Modular ordering assumes generations are less than half the epoch range apart.
		if (elapsed >= (uint32_t(1) << 31)) {
			return;
		}
		const idx_t retained = elapsed == 0 ? RetentionBucketCount(current) : 0;
		const auto updated = PackRetentionBucket(
		    static_cast<uint32_t>(epoch),
		    NumericCast<uint32_t>(retained + MinValue(count, capacity - MinValue(capacity, retained))));
		if (updated == current || bucket.compare_exchange_weak(current, updated, std::memory_order_relaxed)) {
			return;
		}
	} while (true);
}

void BlockAllocator::ReuseRetention(idx_t count, const idx_t now_ms) const {
	const auto epoch = now_ms / BlockAllocatorConfig::RETENTION_INTERVAL_MS;
	for (idx_t age = 0; age < retention_buckets.size() && count > 0; age++) {
		auto &bucket = retention_buckets[(epoch % retention_buckets.size() + retention_buckets.size() - age) %
		                                 retention_buckets.size()];
		auto current = bucket.load(std::memory_order_relaxed);
		while (RetentionBucketEpoch(current) == static_cast<uint32_t>(epoch - age) &&
		       RetentionBucketCount(current) > 0) {
			const auto reused = MinValue<idx_t>(count, RetentionBucketCount(current));
			const auto updated = PackRetentionBucket(RetentionBucketEpoch(current),
			                                         RetentionBucketCount(current) - NumericCast<uint32_t>(reused));
			if (bucket.compare_exchange_weak(current, updated, std::memory_order_relaxed)) {
				count -= reused;
				break;
			}
		}
	}
}

idx_t BlockAllocator::RetentionTarget(const idx_t now_ms) const {
	const auto epoch = now_ms / BlockAllocatorConfig::RETENTION_INTERVAL_MS;
	idx_t weighted = 0;
	const auto offset = now_ms % BlockAllocatorConfig::RETENTION_INTERVAL_MS;
	for (auto &bucket : retention_buckets) {
		const auto current = bucket.load(std::memory_order_relaxed);
		const auto age = static_cast<uint32_t>(epoch - RetentionBucketEpoch(current));
		if (age >= retention_buckets.size()) {
			continue;
		}
		const auto age_ms = age == 0 ? 0 : (age - 1) * BlockAllocatorConfig::RETENTION_INTERVAL_MS + offset;
		const auto remaining_ms = BlockAllocatorConfig::RETENTION_DECAY_MS - age_ms;
		weighted += RetentionBucketCount(current) * remaining_ms;
	}
	return weighted / BlockAllocatorConfig::RETENTION_DECAY_MS;
}

//===--------------------------------------------------------------------===//
// Memory Helpers
//===--------------------------------------------------------------------===//
static data_ptr_t AllocateVirtualMemory(const idx_t size) {
#if INTPTR_MAX == INT32_MAX
	// Disable on 32-bit
	return nullptr;
#endif

#if defined(_WIN32)
	// This returns nullptr on failure
	return data_ptr_t(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
#else
	auto flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__linux__)
	// Reserve address space without charging the entire pool against the commit limit.
	flags |= MAP_NORESERVE;
#endif
	const auto ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	return ptr == MAP_FAILED ? nullptr : data_ptr_cast(ptr);
#endif
}

static void FreeVirtualMemory(const data_ptr_t pointer, const idx_t size) {
	bool success;
#if defined(_WIN32)
	success = VirtualFree(pointer, 0, MEM_RELEASE);
#else
	success = munmap(pointer, size) == 0;
#endif
	if (!success) {
		throw InternalException("FreeVirtualMemory failed");
	}
}

static void OnFirstAllocation(const data_ptr_t pointer, const idx_t size) {
	bool success = true;
#if defined(_WIN32)
	success = VirtualAlloc(pointer, size, MEM_COMMIT, PAGE_READWRITE);
#elif defined(__APPLE__)
	// Reclaimed pages remain reusable until explicitly claimed again.
	success = madvise(pointer, size, MADV_FREE_REUSE) == 0;
#endif
	if (!success) {
		throw InternalException("OnFirstAllocation failed");
	}
}

static bool OnDeallocation(const data_ptr_t pointer, const idx_t size) {
#if defined(_WIN32)
	return VirtualFree(pointer, size, MEM_DECOMMIT);
#elif defined(__APPLE__)
	return madvise(pointer, size, MADV_FREE_REUSABLE) == 0;
#elif defined(__MVS__)
	// the madvice functionality is not available on z/OS in any form
	return false;
#else
	return madvise(pointer, size, MADV_DONTNEED) == 0;
#endif
}

//===--------------------------------------------------------------------===//
// BlockAllocatorThreadLocalState
//===--------------------------------------------------------------------===//
struct BlockQueue {
	duckdb_moodycamel::ConcurrentQueue<uint32_t> q;
};

struct BlockAllocatorLifetimeState {
	annotated_mutex lock;
	bool alive DUCKDB_GUARDED_BY(lock) = true;
};

class BlockAllocatorThreadLocalState {
public:
	explicit BlockAllocatorThreadLocalState(const BlockAllocator &block_allocator_p) {
		Initialize(block_allocator_p);
	}
	~BlockAllocatorThreadLocalState() {
		Clear();
	}

public:
	void TryInitialize(const BlockAllocator &block_allocator_p) {
		// Local state can be invalidated if DB closes but thread stays alive
		if (cached_uuid != block_allocator_p.uuid) {
			Initialize(block_allocator_p);
		}
	}

	data_ptr_t Allocate() {
		auto pointer = TryAllocateFromLocal();
		if (pointer) {
			return pointer;
		}

		// We have run out of local blocks
		if (TryGetBatch(touched, *block_allocator->touched) || TryGetBatch(untouched, *block_allocator->untouched)) {
			// We have refilled local blocks
			pointer = TryAllocateFromLocal();
			D_ASSERT(pointer);
			return pointer;
		}

		// We have also run out of global blocks, use fallback allocator
		return RetryAfterReclaiming(
		    *block_allocator, [&] { return block_allocator->allocator.AllocateData(block_allocator->block_size); });
	}

	void Free(const data_ptr_t pointer) {
		touched.push_back(block_allocator->GetBlockID(pointer));
		block_allocator->UpdateCachedBlocks(1, cache_slot);
		if (touched.size() < BlockAllocatorConfig::FREE_THRESHOLD) {
			return;
		}

		// Upon reaching the threshold, we return a local batch to global
		std::sort(touched.begin(), touched.end());
		ReturnTouched(BlockAllocatorConfig::BATCH_SIZE);
	}

	void Clear() DUCKDB_EXCLUDES(lifetime_state->lock) {
		if (lifetime_state) {
			annotated_lock_guard<annotated_mutex> guard(lifetime_state->lock);
			if (lifetime_state->alive) {
				if (!touched.empty()) {
					ReturnTouched(touched.size());
				}
				if (!untouched.empty()) {
					block_allocator->untouched->q.enqueue_bulk(untouched.begin(), untouched.size());
				}
			}
		}
		touched.clear();
		untouched.clear();
	}

private:
	void ReturnTouched(const idx_t count) {
		block_allocator->AddRetention(count, RetentionTimeMillis());
		block_allocator->touched->q.enqueue_bulk(touched.end() - NumericCast<vector<uint32_t>::difference_type>(count),
		                                         count);
		touched.resize(touched.size() - count);
	}

	void Initialize(const BlockAllocator &block_allocator_p) {
		Clear();
		cached_uuid = block_allocator_p.uuid;
		block_allocator = block_allocator_p;
		lifetime_state = block_allocator_p.lifetime_state;
		cache_slot = block_allocator_p.next_cache_slot.fetch_add(1, std::memory_order_relaxed) %
		             block_allocator_p.cached_blocks.size();
		next_decay_check = 0;
		untouched.reserve(BlockAllocatorConfig::BATCH_SIZE);
		touched.reserve(BlockAllocatorConfig::FREE_THRESHOLD);
	}

	data_ptr_t TryAllocateFromLocal() {
		if (!touched.empty()) {
			const auto pointer = block_allocator->GetPointer(touched.back());
			touched.pop_back();
			block_allocator->UpdateCachedBlocks(-1, cache_slot);
			return pointer;
		}
		if (!untouched.empty()) {
			const auto pointer = block_allocator->GetPointer(untouched.back());
			OnFirstAllocation(pointer, block_allocator->block_size);
			untouched.pop_back();
			return pointer;
		}
		return nullptr;
	}

	bool TryGetBatch(vector<uint32_t> &local, BlockQueue &global) {
		D_ASSERT(local.empty());
		local.resize(BlockAllocatorConfig::BATCH_SIZE);
		const auto size = global.q.try_dequeue_bulk(local.begin(), BlockAllocatorConfig::BATCH_SIZE);
		local.resize(size);
		if (size > 0 && RefersToSameObject(global, *block_allocator->touched)) {
			const auto now = RetentionTimeMillis();
			block_allocator->ReuseRetention(size, now);
			if (now >= next_decay_check) {
				next_decay_check = now + BlockAllocatorConfig::ACTIVE_DECAY_INTERVAL_MS;
				try {
					block_allocator->FlushAllocationCache(BlockAllocatorConfig::MAX_FLUSH_BLOCKS *
					                                          block_allocator->block_size,
					                                      BlockAllocator::ReclaimMode::OPPORTUNISTIC);
					block_allocator->FreeInternal(optional_idx(), BlockAllocatorConfig::MAX_FLUSH_BLOCKS,
					                              BlockAllocator::ReclaimMode::OPPORTUNISTIC);
				} catch (...) {
					// Failed reclamation leaves blocks available for reuse.
				}
			}
		}
		std::sort(local.begin(), local.end());
		return !local.empty();
	}

private:
	hugeint_t cached_uuid;
	optional_ptr<const BlockAllocator> block_allocator;
	shared_ptr<BlockAllocatorLifetimeState> lifetime_state;
	idx_t next_decay_check = 0;
	idx_t cache_slot = 0;

	vector<uint32_t> untouched;
	vector<uint32_t> touched;
};

#ifndef __MVS__
//! Whether this thread created its state. A bool needs no thread_local destructor, so reading it allocates nothing
static thread_local bool thread_local_state_created = false; // NOLINT
#endif

BlockAllocatorThreadLocalState &GetBlockAllocatorThreadLocalState(const BlockAllocator &block_allocator) {
#ifdef __MVS__
	auto allocator_state = BlockAllocatorThreadLocalState(block_allocator);
	static __tlssim<BlockAllocatorThreadLocalState> local_state_impl(allocator_state);
	auto *local_state = local_state_impl.access();
	(*local_state).TryInitialize(block_allocator);
	return *local_state;
#else
	thread_local BlockAllocatorThreadLocalState local_state(block_allocator);
	thread_local_state_created = true;
	local_state.TryInitialize(block_allocator);
	return local_state;
#endif
}

//! A thread that never created its state has no blocks to give back. Creating it registers a thread_local
//! destructor, which glibc cannot fail to allocate without ending the process
static bool ThreadHasLocalState() {
#ifdef __MVS__
	return true;
#else
	return thread_local_state_created;
#endif
}

//===--------------------------------------------------------------------===//
// BlockAllocator
//===--------------------------------------------------------------------===//
class AllocationCache {
	using Queue = duckdb_moodycamel::ConcurrentQueue<data_ptr_t>;
	static constexpr idx_t MIN_SHIFT = 19;
	static constexpr idx_t MAX_SHIFT = 32;
	static constexpr idx_t MAX_SHARDS = 256;
	static constexpr idx_t CPUS_PER_SHARD = 4;
	static constexpr idx_t MAX_LOCAL_SPARES = 16;
	// Retain at most 1 GiB per size class, or one larger allocation.
	static constexpr idx_t MAX_CACHED_BYTES_PER_BUCKET = 1024ULL * 1024 * 1024;

	struct alignas(64) Bucket {
		vector<Queue> queues;
		alignas(64) atomic<idx_t> count {0};
		atomic<idx_t> last_return_ms {0};
	};

public:
	explicit AllocationCache(Allocator &allocator) : allocator(allocator), shard_count(GetShardCount()) {
		for (auto &bucket : buckets) {
			bucket.queues.resize(shard_count);
		}
	}
	~AllocationCache() {
		Flush(NumericLimits<idx_t>::Maximum());
	}

	bool SupportsSize(idx_t size) const {
		return size >= (static_cast<idx_t>(1) << MIN_SHIFT) && size <= (static_cast<idx_t>(1) << MAX_SHIFT) &&
		       IsPowerOfTwo(size);
	}
	data_ptr_t TryAllocate(idx_t size) {
		D_ASSERT(SupportsSize(size));
		const auto shard = LocalShard();
		auto &bucket = buckets[CountZeros<idx_t>::Trailing(size) - MIN_SHIFT];
		data_ptr_t pointer;
		if (bucket.queues[shard].try_dequeue(pointer)) {
			bucket.count.fetch_sub(1, std::memory_order_relaxed);
			return pointer;
		}
		// Share surplus buffers, leaving a few local spares without reserving the entire budget.
		const auto reserve = MinValue<idx_t>(MAX_LOCAL_SPARES, MAX_CACHED_BYTES_PER_BUCKET / size / (2 * shard_count));
		for (idx_t offset = 1; offset < shard_count; offset++) {
			auto &queue = bucket.queues[(shard + offset) % shard_count];
			if ((reserve == 0 || queue.size_approx() > reserve) && queue.try_dequeue(pointer)) {
				bucket.count.fetch_sub(1, std::memory_order_relaxed);
				return pointer;
			}
		}
		return nullptr;
	}
	bool TryFree(data_ptr_t pointer, idx_t size) {
		D_ASSERT(SupportsSize(size));
		const auto shard = LocalShard();
		if (!pointer) {
			return false;
		}
		auto &bucket = buckets[CountZeros<idx_t>::Trailing(size) - MIN_SHIFT];
		if (bucket.count.fetch_add(1, std::memory_order_relaxed) <
		    MaxValue<idx_t>(MAX_CACHED_BYTES_PER_BUCKET / size, 1)) {
			try {
				const auto now = RetentionTimeMillis();
				auto last_return = bucket.last_return_ms.load(std::memory_order_relaxed);
				while (last_return < now &&
				       !bucket.last_return_ms.compare_exchange_weak(last_return, now, std::memory_order_relaxed)) {
				}
				if (bucket.queues[shard].enqueue(pointer)) {
					return true;
				}
			} catch (...) {
				// Failed admission leaves ownership with the caller.
			}
		}
		bucket.count.fetch_sub(1, std::memory_order_relaxed);
		return false;
	}

	idx_t GetCachedMemory(optional_idx idle_before = optional_idx()) const {
		idx_t bytes = 0;
		for (idx_t i = 0; i < buckets.size(); i++) {
			const auto &bucket = buckets[i];
			if (!idle_before.IsValid() ||
			    bucket.last_return_ms.load(std::memory_order_relaxed) <= idle_before.GetIndex()) {
				bytes += bucket.count.load(std::memory_order_relaxed) << (MIN_SHIFT + i);
			}
		}
		return bytes;
	}

	// Maintenance is serialized by BlockAllocator; allocation and return never take that lock.
	idx_t Flush(idx_t byte_limit, optional_idx idle_before = optional_idx()) {
		idx_t freed = 0;
		for (idx_t i = buckets.size(); i > 0 && freed < byte_limit; i--) {
			auto &bucket = buckets[i - 1];
			const idx_t size = static_cast<idx_t>(1) << (MIN_SHIFT + i - 1);
			// A snapshot bounds the work even if producers continue returning buffers.
			auto remaining = bucket.count.load(std::memory_order_relaxed);
			for (auto &queue : bucket.queues) {
				data_ptr_t pointer;
				while (remaining > 0 && freed < byte_limit) {
					if (idle_before.IsValid() &&
					    bucket.last_return_ms.load(std::memory_order_relaxed) > idle_before.GetIndex()) {
						break;
					}
					if (!queue.try_dequeue(pointer)) {
						break;
					}
					allocator.FreeData(pointer, size);
					bucket.count.fetch_sub(1, std::memory_order_relaxed);
					freed += size;
					remaining--;
				}
			}
		}
		return freed;
	}

private:
	static idx_t GetShardCount() {
#ifndef DUCKDB_NO_THREADS
		const idx_t cpus = std::thread::hardware_concurrency();
		return MaxValue<idx_t>(1, MinValue<idx_t>(MAX_SHARDS, (cpus + CPUS_PER_SHARD - 1) / CPUS_PER_SHARD));
#else
		return 1;
#endif
	}

	idx_t LocalShard() const {
		const auto cpu = TaskScheduler::GetEstimatedCPUId();
		return (cpu / CPUS_PER_SHARD) % shard_count;
	}

	Allocator &allocator;
	const idx_t shard_count;
	array<Bucket, MAX_SHIFT - MIN_SHIFT + 1> buckets;
};

BlockAllocator::BlockAllocator(Allocator &allocator_p, const idx_t block_size_p, const idx_t pool_size_p)
    : uuid(UUID::GenerateRandomUUID()), allocator(allocator_p), allocation_cache(make_uniq<AllocationCache>(allocator)),
      block_size(block_size_p), block_size_div_shift(CountZeros<idx_t>::Trailing(block_size)),
      pool_size(sizeof(void *) == 8 ? pool_size_p - pool_size_p % block_size : 0),
      pool_state(pool_size != 0 && DivBlockSize(pool_size) <= NumericLimits<uint32_t>::Maximum()
                     ? PoolState::UNINITIALIZED
                     : PoolState::UNAVAILABLE),
      touched(make_unsafe_uniq<BlockQueue>()), lifetime_state(make_shared_ptr<BlockAllocatorLifetimeState>()) {
	static_assert(std::tuple_size<decltype(retention_buckets)>::value == BlockAllocatorConfig::RETENTION_BUCKETS,
	              "Retention bucket storage must match the decay configuration");
	D_ASSERT(IsPowerOfTwo(block_size));
	InitializeRetention(RetentionTimeMillis());
}

BlockAllocator::~BlockAllocator() {
	{
		annotated_lock_guard<annotated_mutex> guard(lifetime_state->lock);
		lifetime_state->alive = false;
	}
	GetBlockAllocatorThreadLocalState(*this).Clear();
	if (IsInitialized()) {
		try {
			FreeVirtualMemory(pool_base, pool_size);
		} catch (...) {
			// Not allowed to throw in destructor!
		}
	}
}

BlockAllocator &BlockAllocator::Get(DatabaseInstance &db) {
	return *db.config.block_allocator;
}

BlockAllocator &BlockAllocator::Get(AttachedDatabase &db) {
	return Get(db.GetDatabase());
}

void BlockAllocator::SetEnabled(bool enabled_p) {
	enabled.store(enabled_p, std::memory_order_relaxed);
}

bool BlockAllocator::IsEnabled() const {
	return enabled.load(std::memory_order_relaxed);
}

bool BlockAllocator::TryUsePool() const {
	if (!IsEnabled()) {
		return false;
	}
	const auto state = pool_state.load(std::memory_order_acquire);
	if (state != PoolState::UNINITIALIZED) {
		return state == PoolState::READY;
	}
	return InitializePool();
}

bool BlockAllocator::InitializePool() const {
	annotated_lock_guard<annotated_mutex> guard(pool_init_lock);
	const auto state = pool_state.load(std::memory_order_relaxed);
	if (state != PoolState::UNINITIALIZED) {
		return state == PoolState::READY;
	}

	unsafe_unique_ptr<BlockQueue> queue;
	try {
		queue = make_unsafe_uniq<BlockQueue>();
		uint32_t block_ids[STANDARD_VECTOR_SIZE];
		const auto count = DivBlockSize(pool_size);
		for (idx_t block_id = 0; block_id < count; block_id += STANDARD_VECTOR_SIZE) {
			const auto next = MinValue<idx_t>(count - block_id, STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < next; i++) {
				block_ids[i] = NumericCast<uint32_t>(block_id + i);
			}
			if (!queue->q.enqueue_bulk(block_ids, next)) {
				pool_state.store(PoolState::UNAVAILABLE, std::memory_order_release);
				return false;
			}
		}
	} catch (const std::bad_alloc &) {
		pool_state.store(PoolState::UNAVAILABLE, std::memory_order_release);
		return false;
	}

	const auto pointer = AllocateVirtualMemory(pool_size);
	if (!pointer) {
		pool_state.store(PoolState::UNAVAILABLE, std::memory_order_release);
		return false;
	}
	untouched = std::move(queue);
	pool_base = pointer;
	pool_state.store(PoolState::READY, std::memory_order_release);
	return true;
}

bool BlockAllocator::IsInitialized() const {
	return pool_state.load(std::memory_order_acquire) == PoolState::READY;
}

bool BlockAllocator::IsInPool(const data_ptr_t pointer) const {
	if (!IsInitialized()) {
		return false;
	}
	const auto address = reinterpret_cast<uintptr_t>(pointer);
	const auto base = reinterpret_cast<uintptr_t>(pool_base);
	return address >= base && address - base < pool_size;
}

idx_t BlockAllocator::ModuloBlockSize(const idx_t n) const {
	return n & (block_size - 1);
}

idx_t BlockAllocator::DivBlockSize(const idx_t n) const {
	return n >> block_size_div_shift;
}

uint32_t BlockAllocator::GetBlockID(const data_ptr_t pointer) const {
	D_ASSERT(IsInPool(pointer));
	const auto offset = NumericCast<idx_t>(pointer - pool_base);
	D_ASSERT(ModuloBlockSize(offset) == 0);
	const auto block_id = NumericCast<uint32_t>(DivBlockSize(offset));
	VerifyBlockID(block_id);
	return block_id;
}

void BlockAllocator::VerifyBlockID(const uint32_t block_id) const {
	D_ASSERT(block_id < DivBlockSize(pool_size));
}

data_ptr_t BlockAllocator::GetPointer(const uint32_t block_id) const {
	VerifyBlockID(block_id);
	return pool_base + NumericCast<idx_t>(block_id) * block_size;
}

data_ptr_t BlockAllocator::AllocateData(const idx_t size) const {
	if (size != block_size || !TryUsePool()) {
		if (allocation_cache->SupportsSize(size)) {
			auto pointer = allocation_cache->TryAllocate(size);
			if (pointer) {
				return pointer;
			}
		}
		return RetryAfterReclaiming(*this, [&] { return allocator.AllocateData(size); });
	}
	return GetBlockAllocatorThreadLocalState(*this).Allocate();
}

idx_t BlockAllocator::GetCachedMemory() const {
	return GetCachedPoolMemory() + allocation_cache->GetCachedMemory();
}

idx_t BlockAllocator::GetCachedPoolMemory() const {
	int64_t count = 0;
	const auto slots = MinValue<idx_t>(next_cache_slot.load(std::memory_order_relaxed), cached_blocks.size());
	for (idx_t i = 0; i < slots; i++) {
		count += cached_blocks[i].count.load(std::memory_order_relaxed);
	}
	return count > 0 ? idx_t(count) * block_size : 0;
}

void BlockAllocator::UpdateCachedBlocks(const int64_t count, const idx_t slot) const {
	cached_blocks[slot].count.fetch_add(count, std::memory_order_relaxed);
}

void BlockAllocator::FreeData(const data_ptr_t pointer, const idx_t size) const {
	if (!pointer) {
		return;
	}
	if (!IsInPool(pointer)) {
		if (allocation_cache->SupportsSize(size) && allocation_cache->TryFree(pointer, size)) {
			return;
		}
		allocator.FreeData(pointer, size);
		estimated_unflushed_memory.fetch_add(size, std::memory_order_relaxed);
		return;
	}
	D_ASSERT(size == block_size);
	GetBlockAllocatorThreadLocalState(*this).Free(pointer);
}

data_ptr_t BlockAllocator::ReallocateData(const data_ptr_t pointer, const idx_t old_size, const idx_t new_size) const {
	if (old_size == new_size) {
		return pointer;
	}

	const bool old_in_pool = IsInPool(pointer);
	const bool new_can_use_pool = new_size == block_size && TryUsePool();
	if (!old_in_pool && !new_can_use_pool) {
		return RetryAfterReclaiming(*this, [&] { return allocator.ReallocateData(pointer, old_size, new_size); });
	}

	// Either old or new can be in the pool: allocate, copy, and free
	const auto new_pointer = AllocateData(new_size);
	memcpy(new_pointer, pointer, MinValue(old_size, new_size));
	FreeData(pointer, old_size);
	return new_pointer;
}

#if defined(__linux__) && INTPTR_MAX == INT64_MAX && defined(MADV_HUGEPAGE)
static idx_t GetHugePageSize() {
	auto file = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
	if (!file) {
		return 0;
	}
	int64_t huge_page_size = 0;
	char trailing;
	auto fields = fscanf(file, "%" SCNd64 " %c", &huge_page_size, &trailing);
	fclose(file);
	auto page_size = sysconf(_SC_PAGESIZE);
	if (fields != 1 || page_size <= 0 || huge_page_size < page_size || (huge_page_size & (huge_page_size - 1)) != 0 ||
	    huge_page_size % page_size != 0) {
		return 0;
	}
	return NumericCast<idx_t>(huge_page_size);
}
#endif

static void AdviseHugePages(data_ptr_t pointer, idx_t size) {
#if defined(__linux__) && INTPTR_MAX == INT64_MAX && defined(MADV_HUGEPAGE)
	static constexpr idx_t MIN_ADVISE_SIZE = 8ULL * 1024 * 1024;
	if (size >= MIN_ADVISE_SIZE) {
		static const idx_t huge_page_size = GetHugePageSize();
		if (huge_page_size != 0 && size / huge_page_size >= 4) {
			const auto base = reinterpret_cast<uintptr_t>(pointer);
			const auto offset = (huge_page_size - base % huge_page_size) % huge_page_size;
			const auto length = ((size - offset) / huge_page_size) * huge_page_size;
			// Only advise complete huge pages contained in this allocation.
			madvise(pointer + offset, length, MADV_HUGEPAGE);
		}
	}
#endif
}

data_ptr_t BlockAllocator::AllocateHugeData(const idx_t size) const {
	auto pointer = AllocateData(size);
	AdviseHugePages(pointer, size);
	return pointer;
}

void BlockAllocator::FreeHugeData(data_ptr_t pointer, const idx_t size) const {
	FreeData(pointer, size);
}

bool BlockAllocator::SupportsFlush() const {
	return true;
}

optional_idx BlockAllocator::DecayDelay() const {
	auto delay = Allocator::DecayDelay();
	if (!delay.IsValid()) {
		return optional_idx(BlockAllocatorConfig::DEFAULT_DECAY_DELAY);
	}
	return delay;
}

void BlockAllocator::ReturnThreadLocalBlocks() const {
	if (IsInitialized() && ThreadHasLocalState()) {
		GetBlockAllocatorThreadLocalState(*this).Clear();
	}
}

void BlockAllocator::ThreadFlush(bool allocator_background_threads, idx_t threshold, idx_t thread_count) const {
	ReturnThreadLocalBlocks();
	if (Allocator::SupportsFlush()) {
		Allocator::ThreadFlush(allocator_background_threads, threshold, thread_count);
	}
}

class BlockAllocatorFlushTask : public Task {
public:
	BlockAllocatorFlushTask(TaskScheduler &scheduler, const BlockAllocator &allocator,
	                        optional_idx remaining_blocks = optional_idx())
	    : scheduler(scheduler), allocator(allocator), remaining_blocks(remaining_blocks) {
	}

	TaskExecutionResult Execute(TaskExecutionMode mode) override {
		using FlushState = BlockAllocator::FlushState;
		{
			annotated_lock_guard<annotated_mutex> guard(allocator.flush_lock);
			D_ASSERT(allocator.flush_state == FlushState::SCHEDULED ||
			         allocator.flush_state == FlushState::RESCHEDULE_REQUESTED);
			if (!remaining_blocks.IsValid()) {
				allocator.flush_state = FlushState::SCHEDULED;
			}
		}
		const auto remaining = allocator.FlushPool(remaining_blocks, BlockAllocatorConfig::MAX_FLUSH_BLOCKS,
		                                           BlockAllocator::ReclaimMode::DECAY);
		const auto cache_freed = allocator.FlushAllocationCache(
		    BlockAllocatorConfig::MAX_FLUSH_BLOCKS * allocator.block_size, BlockAllocator::ReclaimMode::DECAY);
		if (cache_freed > 0) {
			allocator.FlushFallbackAllocator(
			    allocator.estimated_unflushed_memory.exchange(0, std::memory_order_relaxed));
		}
		annotated_lock_guard<annotated_mutex> guard(allocator.flush_lock);
		const bool reschedule = allocator.flush_state == FlushState::RESCHEDULE_REQUESTED;
		allocator.flush_state = FlushState::IDLE;
		// Failed or unavailable work waits for another idle request; successful work can continue in bounded tasks.
		if ((remaining > 0 || cache_freed > 0 || reschedule) && scheduler.GetNumberOfTasks() == 0 &&
		    scheduler.NumberOfAsyncThreads() > 0 && allocator.HasReclaimableMemory()) {
			try {
				scheduler.ScheduleTask(
				    *allocator.flush_producer,
				    make_shared_ptr<BlockAllocatorFlushTask>(scheduler, allocator,
				                                             remaining > 0 ? optional_idx(remaining) : optional_idx()),
				    TaskSchedulerType::ASYNC);
				allocator.flush_state =
				    remaining > 0 && reschedule ? FlushState::RESCHEDULE_REQUESTED : FlushState::SCHEDULED;
			} catch (...) {
				// A later idle request can retry failed submission.
			}
		}
		return TaskExecutionResult::TASK_FINISHED;
	}

private:
	TaskScheduler &scheduler;
	const BlockAllocator &allocator;
	const optional_idx remaining_blocks;
};

bool BlockAllocator::TryScheduleFlush(TaskScheduler &scheduler) const {
	annotated_lock_guard<annotated_mutex> guard(flush_lock);
	if (scheduler.NumberOfAsyncThreads() == 0) {
		return false;
	}
	if (flush_state != FlushState::IDLE) {
		flush_state = FlushState::RESCHEDULE_REQUESTED;
		return true;
	}
	if (!HasReclaimableMemory()) {
		return true;
	}
	try {
		if (!flush_producer) {
			flush_producer = scheduler.CreateProducer();
		}
		scheduler.ScheduleTask(*flush_producer, make_shared_ptr<BlockAllocatorFlushTask>(scheduler, *this),
		                       TaskSchedulerType::ASYNC);
		flush_state = FlushState::SCHEDULED;
		return true;
	} catch (...) {
		return false;
	}
}

void BlockAllocator::SetScheduler(TaskScheduler &scheduler_p) {
	D_ASSERT(!scheduler);
	scheduler = scheduler_p;
}

void BlockAllocator::ClearScheduler() {
	scheduler = nullptr;
}

void BlockAllocator::ThreadIdle() const {
	auto scheduler_ref = scheduler;
	try {
		ReturnThreadLocalBlocks();
		if (!scheduler_ref || !TryScheduleFlush(*scheduler_ref)) {
			const auto mode = scheduler_ref ? ReclaimMode::DECAY : ReclaimMode::FORCE;
			FlushPool(optional_idx(), optional_idx(), mode);
			if (FlushAllocationCache(NumericLimits<idx_t>::Maximum(), mode) > 0) {
				FlushFallbackAllocator(estimated_unflushed_memory.exchange(0, std::memory_order_relaxed));
			}
		}
	} catch (...) {
		// Reclamation is best effort on scheduler threads.
	}
	Allocator::ThreadIdle();
}

void BlockAllocator::ThreadExit() const {
	try {
		ReturnThreadLocalBlocks();
	} catch (...) {
		// Returning cached blocks is best effort on exiting threads.
	}
	Allocator::ThreadIdle();
}

idx_t BlockAllocator::GetFreeBlockCount() const {
	return touched->q.size_approx();
}

idx_t BlockAllocator::GetReclaimableBlockCount(const ReclaimMode mode, const optional_idx cache_limit) const {
	auto available = GetFreeBlockCount();
	if (cache_limit.IsValid()) {
		const auto cached = DivBlockSize(GetCachedPoolMemory());
		const auto remaining = cached - MinValue(cached, reclaiming_blocks);
		available = MinValue(available, remaining - MinValue(remaining, cache_limit.GetIndex()));
	}
	if (mode == ReclaimMode::FORCE) {
		return available;
	}
	return available - MinValue(available, RetentionTarget(RetentionTimeMillis()));
}

idx_t BlockAllocator::FlushPool(const optional_idx block_limit, const optional_idx task_limit, const ReclaimMode mode,
                                const optional_idx cache_limit) const noexcept {
	try {
		ReturnThreadLocalBlocks();
		return FreeInternal(block_limit, task_limit, mode, cache_limit);
	} catch (...) {
		// Failed reclamation leaves blocks available for reuse.
		return 0;
	}
}

static idx_t AllocationCacheIdleCutoff() {
	const auto now = RetentionTimeMillis();
	return now - MinValue(now, BlockAllocatorConfig::RETENTION_DECAY_MS);
}

bool BlockAllocator::HasReclaimableMemory() const {
	return GetReclaimableBlockCount(ReclaimMode::DECAY) > 0 ||
	       allocation_cache->GetCachedMemory(AllocationCacheIdleCutoff()) > 0;
}

bool BlockAllocator::HasPendingCacheDecay() const {
	return allocation_cache->GetCachedMemory() != 0;
}

idx_t BlockAllocator::FlushAllocationCache(idx_t byte_limit, ReclaimMode mode) const noexcept {
	annotated_unique_lock<annotated_mutex> guard(flush_lock, std::defer_lock);
	if (mode == ReclaimMode::OPPORTUNISTIC) {
		if (!guard.try_lock()) {
			return 0;
		}
	} else {
		guard.lock();
	}
	const auto freed = allocation_cache->Flush(
	    byte_limit, mode != ReclaimMode::FORCE ? optional_idx(AllocationCacheIdleCutoff()) : optional_idx());
	estimated_unflushed_memory.fetch_add(freed, std::memory_order_relaxed);
	return freed;
}

idx_t BlockAllocator::TrimCachedMemory(idx_t memory_headroom, optional_idx pool_block_limit) const noexcept {
	const auto cached = allocation_cache->GetCachedMemory();
	// Avoid reading the fixed pool's per-thread counters when even its capacity fits.
	if (cached <= memory_headroom &&
	    (pool_size <= memory_headroom - cached || GetCachedPoolMemory() <= memory_headroom - cached)) {
		return 0;
	}
	idx_t freed = 0;
	idx_t pool_headroom;
	{
		annotated_lock_guard<annotated_mutex> guard(flush_lock);
		const auto total = GetCachedMemory();
		if (total <= memory_headroom) {
			return 0;
		}
		freed = allocation_cache->Flush(total - memory_headroom);
		estimated_unflushed_memory.fetch_add(freed, std::memory_order_relaxed);
		const auto remaining = allocation_cache->GetCachedMemory();
		pool_headroom = memory_headroom - MinValue(memory_headroom, remaining);
	}
	FlushPool(pool_block_limit, optional_idx(), ReclaimMode::FORCE, DivBlockSize(pool_headroom));
	return freed;
}

void BlockAllocator::FlushAll() const noexcept {
	FlushPool();
	FlushAllocationCache(NumericLimits<idx_t>::Maximum());
	FlushFallbackAllocator(estimated_unflushed_memory.exchange(0, std::memory_order_relaxed));
}

void BlockAllocator::FlushOnShutdown() const noexcept {
	FlushAllocationCache(NumericLimits<idx_t>::Maximum());
	FlushFallbackAllocator(estimated_unflushed_memory.exchange(0, std::memory_order_relaxed));
}

void BlockAllocator::FlushForAllocation(const idx_t extra_memory, const idx_t memory_headroom) const noexcept {
	TrimCachedMemory(memory_headroom, DivBlockSize(extra_memory));
	FlushFallbackAllocator(estimated_unflushed_memory.exchange(0, std::memory_order_relaxed));
}

bool BlockAllocator::TryFlushDeallocated(const idx_t threshold, const idx_t memory_headroom) const noexcept {
	if (TrimCachedMemory(memory_headroom) > 0) {
		FlushFallbackAllocator(estimated_unflushed_memory.exchange(0, std::memory_order_relaxed));
		return true;
	}
	auto deallocated = estimated_unflushed_memory.load(std::memory_order_relaxed);
	if (deallocated == 0 || deallocated < threshold || !Allocator::SupportsFlush()) {
		return false;
	}
	if (deallocated <= memory_headroom && GetCachedMemory() <= memory_headroom - deallocated) {
		return false;
	}
	if (!estimated_unflushed_memory.compare_exchange_strong(deallocated, 0, std::memory_order_relaxed)) {
		return false;
	}
	const auto mapped_memory = allocator.GetMappedMemory();
	if (mapped_memory.IsValid()) {
		// Repeated frees of reused memory cannot retain more than the fallback allocator's mapped footprint.
		const auto unflushed = MinValue(deallocated, mapped_memory.GetIndex());
		if (unflushed <= memory_headroom && GetCachedMemory() <= memory_headroom - unflushed) {
			// Preserve the bound and any frees made while checking allocator statistics.
			estimated_unflushed_memory.fetch_add(unflushed, std::memory_order_relaxed);
			return false;
		}
	}
	FlushFallbackAllocator(deallocated);
	return true;
}

void BlockAllocator::FlushFallbackAllocator(const idx_t deallocated) const noexcept {
	try {
		if (Allocator::SupportsFlush()) {
			Allocator::FlushAll();
		}
	} catch (...) {
		// Keep failed purges eligible for retry without losing concurrent frees.
		estimated_unflushed_memory.fetch_add(deallocated, std::memory_order_relaxed);
	}
}

idx_t BlockAllocator::FreeInternal(const optional_idx block_limit, const optional_idx task_limit,
                                   const ReclaimMode mode, const optional_idx cache_limit) const {
	if (!IsInitialized()) {
		return 0;
	}
	// Bound this flush even when other threads keep freeing blocks.
	idx_t remaining;
	{
		annotated_unique_lock<annotated_mutex> guard(flush_lock, std::defer_lock);
		if (mode == ReclaimMode::OPPORTUNISTIC) {
			if (!guard.try_lock()) {
				return 0;
			}
		} else {
			guard.lock();
		}
		remaining = GetReclaimableBlockCount(mode, cache_limit);
	}
	if (block_limit.IsValid()) {
		remaining = MinValue(remaining, block_limit.GetIndex());
	}
	auto task_remaining = task_limit.IsValid() ? MinValue(remaining, task_limit.GetIndex()) : remaining;
	const auto batch_size =
	    mode == ReclaimMode::FORCE ? BlockAllocatorConfig::RECLAIM_BATCH_SIZE : BlockAllocatorConfig::MAX_FLUSH_BLOCKS;
	unsafe_vector<uint32_t> to_free_buffer;
	to_free_buffer.resize(MinValue(task_remaining, batch_size));
	while (task_remaining > 0) {
		idx_t count;
		{
			annotated_unique_lock<annotated_mutex> guard(flush_lock, std::defer_lock);
			if (mode == ReclaimMode::OPPORTUNISTIC) {
				if (!guard.try_lock()) {
					return 0;
				}
			} else {
				guard.lock();
			}
			const auto eligible = MinValue(task_remaining, GetReclaimableBlockCount(mode, cache_limit));
			if (eligible == 0) {
				return 0;
			}
			count = touched->q.try_dequeue_bulk(to_free_buffer.begin(), MinValue(eligible, batch_size));
			reclaiming_blocks += count;
		}
		if (count == 0) {
			return 0;
		}
		remaining -= count;
		task_remaining -= count;
		std::sort(to_free_buffer.begin(),
		          to_free_buffer.begin() + NumericCast<unsafe_vector<uint32_t>::difference_type>(count));

		idx_t start = 0;
		while (start < count) {
			auto end = start + 1;
			while (end < count && to_free_buffer[end] == to_free_buffer[end - 1] + 1) {
				end++;
			}
			if (!FreeContiguousBlocks(to_free_buffer[start], to_free_buffer[end - 1])) {
				annotated_lock_guard<annotated_mutex> guard(flush_lock);
				reclaiming_blocks -= count - start;
				touched->q.enqueue_bulk(to_free_buffer.begin() +
				                            NumericCast<unsafe_vector<uint32_t>::difference_type>(start),
				                        count - start);
				return 0;
			}
			{
				annotated_lock_guard<annotated_mutex> guard(flush_lock);
				UpdateCachedBlocks(-NumericCast<int64_t>(end - start), 0);
				reclaiming_blocks -= end - start;
			}
			untouched->q.enqueue_bulk(
			    to_free_buffer.begin() + NumericCast<unsafe_vector<uint32_t>::difference_type>(start), end - start);
			start = end;
		}
	}
	return remaining;
}

bool BlockAllocator::FreeContiguousBlocks(const uint32_t block_id_start, const uint32_t block_id_end_including) const {
	const auto pointer = GetPointer(block_id_start);
	const auto num_blocks = block_id_end_including - block_id_start + 1;
	const auto size = num_blocks * block_size;
	return OnDeallocation(pointer, size);
}

} // namespace duckdb
