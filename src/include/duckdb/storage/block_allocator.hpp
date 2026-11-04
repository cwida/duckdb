//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/block_allocator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class Allocator;
class AllocationCache;
class AttachedDatabase;
class DatabaseInstance;
class TaskScheduler;
struct ProducerToken;
struct BlockQueue;
struct BlockAllocatorLifetimeState;

class BlockAllocator {
	friend class BlockAllocatorThreadLocalState;
	friend class BlockAllocatorFlushTask;
	friend class BlockAllocatorTestHelper;
	friend class BufferAllocator;
	friend class TaskScheduler;

public:
	BlockAllocator(Allocator &allocator, idx_t block_size, idx_t pool_size);
	~BlockAllocator();

public:
	static BlockAllocator &Get(DatabaseInstance &db);
	static BlockAllocator &Get(AttachedDatabase &db);

	//! Control new pool allocations without changing ownership of existing blocks.
	void SetEnabled(bool enabled);
	bool IsEnabled() const;

	//! Allocation functions (same API as Allocator)
	data_ptr_t AllocateData(idx_t size) const;
	void FreeData(data_ptr_t pointer, idx_t size) const;
	data_ptr_t ReallocateData(data_ptr_t pointer, idx_t old_size, idx_t new_size) const;

	//! Free backing retained in global queues, thread-local caches, and ongoing reclamation.
	idx_t GetCachedMemory() const;

	//! Flush outstanding allocations
	bool SupportsFlush() const;
	optional_idx DecayDelay() const;
	void ThreadFlush(bool allocator_background_threads, idx_t threshold, idx_t thread_count) const;
	//! Apply idle decay through the bound scheduler; standalone allocators reclaim synchronously.
	void ThreadIdle() const DUCKDB_EXCLUDES(flush_lock);
	//! Return local blocks without scheduling or discarding pool backing.
	void ThreadExit() const;
	//! Best-effort reclamation of all cached backing and fallback allocations.
	void FlushAll() const noexcept DUCKDB_EXCLUDES(flush_lock);
	//! Leave pool backing for unmapping and release cached fallback allocations.
	void FlushOnShutdown() const noexcept DUCKDB_EXCLUDES(flush_lock);
	//! Retain cached backing that fits alongside the buffer manager's live reservations.
	void FlushForAllocation(idx_t extra_memory, idx_t memory_headroom) const noexcept DUCKDB_EXCLUDES(flush_lock);
	//! Trim cached backing, then purge accumulated fallback frees that do not fit in the headroom.
	bool TryFlushDeallocated(idx_t threshold, idx_t memory_headroom) const noexcept DUCKDB_EXCLUDES(flush_lock);

private:
	//! The buffer allocator's huge-page backend keeps matching allocation and release ownership.
	data_ptr_t AllocateHugeData(idx_t size) const;
	void FreeHugeData(data_ptr_t pointer, idx_t size) const;

	enum class FlushState : uint8_t { IDLE, SCHEDULED, RESCHEDULE_REQUESTED };
	enum class ReclaimMode : uint8_t { FORCE, DECAY, OPPORTUNISTIC };
	enum class PoolState : uint8_t { UNINITIALIZED, READY, UNAVAILABLE };

	bool TryUsePool() const DUCKDB_EXCLUDES(pool_init_lock);
	bool InitializePool() const DUCKDB_EXCLUDES(pool_init_lock);
	bool IsInitialized() const;
	bool IsInPool(data_ptr_t pointer) const;

	idx_t ModuloBlockSize(idx_t n) const;
	idx_t DivBlockSize(idx_t n) const;

	uint32_t GetBlockID(data_ptr_t pointer) const;
	data_ptr_t GetPointer(uint32_t block_id) const;

	void VerifyBlockID(uint32_t block_id) const;

	void InitializeRetention(idx_t now_ms) const;
	void AddRetention(idx_t count, idx_t now_ms) const;
	void ReuseRetention(idx_t count, idx_t now_ms) const;
	idx_t RetentionTarget(idx_t now_ms) const;
	void UpdateCachedBlocks(int64_t count, idx_t slot) const;
	idx_t GetCachedPoolMemory() const;
	void ReturnThreadLocalBlocks() const;
	void FlushFallbackAllocator(idx_t deallocated) const noexcept;
	//! Return bytes released to the fallback allocator, which may still need a purge.
	idx_t TrimCachedMemory(idx_t memory_headroom, optional_idx pool_block_limit = optional_idx()) const noexcept
	    DUCKDB_EXCLUDES(flush_lock);
	idx_t FlushAllocationCache(idx_t byte_limit, ReclaimMode mode = ReclaimMode::FORCE) const noexcept
	    DUCKDB_EXCLUDES(flush_lock);
	bool HasReclaimableMemory() const DUCKDB_REQUIRES(flush_lock);
	bool HasPendingCacheDecay() const;

	void SetScheduler(TaskScheduler &scheduler);
	void ClearScheduler();

	bool TryScheduleFlush(TaskScheduler &scheduler) const DUCKDB_EXCLUDES(flush_lock);
	idx_t GetFreeBlockCount() const DUCKDB_REQUIRES(flush_lock);
	idx_t GetReclaimableBlockCount(ReclaimMode mode, optional_idx cache_limit = optional_idx()) const
	    DUCKDB_REQUIRES(flush_lock);
	//! Return the unprocessed portion of the initial free-block budget.
	idx_t FlushPool(optional_idx block_limit = optional_idx(), optional_idx task_limit = optional_idx(),
	                ReclaimMode mode = ReclaimMode::FORCE, optional_idx cache_limit = optional_idx()) const noexcept
	    DUCKDB_EXCLUDES(flush_lock);
	idx_t FreeInternal(optional_idx block_limit, optional_idx task_limit, ReclaimMode mode,
	                   optional_idx cache_limit = optional_idx()) const DUCKDB_EXCLUDES(flush_lock);
	bool FreeContiguousBlocks(uint32_t block_id_start, uint32_t block_id_end_including) const;

private:
	//! Identifier
	const hugeint_t uuid;
	//! Fallback allocator
	Allocator &allocator;
	//! Cache of whole power-of-two fallback allocations, with preferred CPU-local reuse.
	unique_ptr<AllocationCache> allocation_cache;

	//! Block size (power of two)
	const idx_t block_size;
	//! Shift for dividing by block size
	const idx_t block_size_div_shift;

	//! Fixed capacity, rounded down to whole blocks; zero on 32-bit.
	const idx_t pool_size;
	//! Allocation permission is independent of the lifetime of the mapping.
	atomic<bool> enabled {true};
	//! READY publishes the mapping and queues; UNAVAILABLE prevents retrying a failed reservation.
	mutable atomic<PoolState> pool_state;
	mutable annotated_mutex pool_init_lock;
	//! Immutable after publication through pool_state.
	mutable data_ptr_t pool_base = nullptr;

	//! Untouched block IDs
	mutable unsafe_unique_ptr<BlockQueue> untouched;
	//! Touched by block IDs
	unsafe_unique_ptr<BlockQueue> touched;

	struct alignas(64) CachedBlockCount {
		//! Cross-thread reuse can make individual balances negative.
		atomic<int64_t> count {0};
	};
	//! Separate writers so each block reuse does not contend on a single counter.
	mutable array<CachedBlockCount, 64> cached_blocks;
	mutable atomic<idx_t> next_cache_slot {0};
	//! Still charged to cached_blocks until discard succeeds.
	mutable idx_t reclaiming_blocks DUCKDB_GUARDED_BY(flush_lock) = 0;

	//! Synchronizes returning cached blocks with allocator destruction.
	shared_ptr<BlockAllocatorLifetimeState> lifetime_state;

	//! Approximate retention volume, with a generation and count packed into each bucket.
	mutable array<atomic<uint64_t>, 3> retention_buckets;
	//! Protect maintenance decisions and background scheduling.
	mutable annotated_mutex flush_lock;
	//! Scheduler queue destruction invalidates this token before allocator destruction.
	mutable unique_ptr<ProducerToken> flush_producer DUCKDB_GUARDED_BY(flush_lock);
	mutable FlushState flush_state DUCKDB_GUARDED_BY(flush_lock) = FlushState::IDLE;
	//! Bound before workers start and cleared after they join; never owns the scheduler.
	optional_ptr<TaskScheduler> scheduler;

	//! Estimated memory freed but still held by the fallback allocator, separate from pooled allocations.
	alignas(64) mutable atomic<idx_t> estimated_unflushed_memory {0};
};

} // namespace duckdb
