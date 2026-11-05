#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/concurrentqueue.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

static idx_t FileBufferTypeToEvictionQueueTypeIdx(const FileBufferType &type) {
	switch (type) {
	case FileBufferType::BLOCK:
	case FileBufferType::EXTERNAL_FILE:
		return 0; // Evict these first (cheap, just free)
	case FileBufferType::MANAGED_BUFFER:
		return 1; // Then these (have to write to storage)
	case FileBufferType::TINY_BUFFER:
		return 2; // Evict tiny buffers last (last resort)
	default:
		throw InternalException("Unknown FileBufferType in FileBufferTypeToEvictionQueueTypeIdx");
	}
}

static vector<FileBufferType> EvictionQueueTypeIdxToFileBufferTypes(const idx_t &queue_type_idx) {
	switch (queue_type_idx) {
	case 0:
		return {FileBufferType::BLOCK, FileBufferType::EXTERNAL_FILE};
	case 1:
		return {FileBufferType::MANAGED_BUFFER};
	case 2:
		return {FileBufferType::TINY_BUFFER};
	default:
		throw InternalException("Unknown queue type index in EvictionQueueTypeIdxToFileBufferTypes");
	}
}

BufferEvictionNode::BufferEvictionNode(weak_ptr<BlockMemory> block_memory_p, idx_t eviction_seq_num)
    : memory_p(std::move(block_memory_p)), handle_sequence_number(eviction_seq_num) {
	D_ASSERT(!memory_p.expired());
}

bool BufferEvictionNode::IsDeadNode(optional_idx debug_sleep_micros) {
	auto shared_memory_p = memory_p.lock();
	if (debug_sleep_micros.IsValid()) {
		ThreadUtil::SleepMicroSeconds(debug_sleep_micros.GetIndex());
	}
	if (!shared_memory_p) {
		return true;
	}
	if (handle_sequence_number != shared_memory_p->GetEvictionSequenceNumber()) {
		return true;
	}
	return false;
}

typedef duckdb_moodycamel::ConcurrentQueue<BufferEvictionNode> eviction_queue_t;

struct EvictionQueue {
public:
	explicit EvictionQueue(const vector<FileBufferType> &file_buffer_types_p)
	    : file_buffer_types(file_buffer_types_p), debug_eviction_queue_sleep(0), evict_queue_insertions(0),
	      total_dead_nodes(0), purge_consumer_token(q), purge_producer_token(q) {
	}

public:
	//! Add a buffer handle to the eviction queue. Returns true, if the queue is
	//! ready to be purged, and false otherwise.
	bool AddToEvictionQueue(BlockLock &lock, BlockMemory &memory, BufferEvictionNode &&node);
	//! Publish an entry while holding its block lock, updating the live-entry flag on success.
	bool Enqueue(BlockLock &lock, BlockMemory &memory, BufferEvictionNode &&node);
	//! Tries to dequeue an element from the eviction queue, but only after acquiring the purge queue lock.
	bool TryDequeueWithLock(BufferEvictionNode &node);
	//! Garbage collect dead nodes in the eviction queue.
	void Purge();
	template <typename FN>
	void IterateUnloadableBlocks(FN fn);

	//! Increment the dead node counter in the purge queue.
	inline void IncrementDeadNodes() {
		total_dead_nodes++;
	}
	//! Decrement the dead node counter in the purge queue.
	inline void DecrementDeadNodes() {
		total_dead_nodes--;
	}
	bool HasFileBufferType(const FileBufferType &type) const {
		return std::find(file_buffer_types.begin(), file_buffer_types.end(), type) != file_buffer_types.end();
	}
	idx_t GetApproximateSize() const {
		return q.size_approx();
	}
	idx_t GetDeadNodes() const {
		return total_dead_nodes.load(std::memory_order_relaxed);
	}
	idx_t GetTotalInsertions() const {
		return evict_queue_insertions.load(std::memory_order_relaxed);
	}

private:
	//! Bulk purge dead nodes from the eviction queue. Then, re-enqueue those that are still alive.
	void PurgeIteration(const idx_t purge_size);
	//! Forget a dequeued entry that could not be published again.
	void DiscardNode(BufferEvictionNode &node);

public:
	//! The type of the buffers in this queue and helper function (both for verification only)
	const vector<FileBufferType> file_buffer_types;
	//! The concurrent queue
	eviction_queue_t q;
	//! Debug-only: atomic holding the eviction queue sleep setting.
	atomic<idx_t> debug_eviction_queue_sleep;

private:
	//! We trigger a purge of the eviction queue every INSERT_INTERVAL insertions
	constexpr static idx_t INSERT_INTERVAL = 4096;
	//! We multiply the base purge size by this value.
	constexpr static idx_t PURGE_SIZE_MULTIPLIER = 2;
	//! We multiply the purge size by this value to determine early-outs. This is the minimum queue size.
	//! We never purge below this point.
	constexpr static idx_t EARLY_OUT_MULTIPLIER = 4;
	//! We multiply the approximate alive nodes by this value to test whether our total dead nodes
	//! exceed their allowed ratio. Must be greater than 1.
	constexpr static idx_t ALIVE_NODE_MULTIPLIER = 4;

private:
	//! Total number of insertions into the eviction queue. This guides the schedule for calling PurgeQueue.
	atomic<idx_t> evict_queue_insertions;
	//! Total dead nodes in the eviction queue. There are two scenarios in which a node dies: (1) we destroy its block
	//! handle, or (2) we insert a newer version into the eviction queue.
	atomic<idx_t> total_dead_nodes;

	//! Locked, if a queue purge is currently active or we're trying to forcefully evict a node.
	//! Only lets a single thread enter the purge phase.
	mutex purge_lock;
	//! A pre-allocated vector of eviction nodes. We reuse this to keep the allocation overhead of purges small.
	vector<BufferEvictionNode> purge_nodes;
	//! Consumer token for purge dequeuing — progresses through sub-queues sequentially.
	duckdb_moodycamel::ConsumerToken purge_consumer_token;
	//! Producer token for re-enqueuing alive nodes into a dedicated sub-queue.
	duckdb_moodycamel::ProducerToken purge_producer_token;
};

bool EvictionQueue::Enqueue(BlockLock &lock, BlockMemory &memory, BufferEvictionNode &&node) {
	memory.SetHasLiveQueueEntry(lock, false);
	const auto success = q.enqueue(std::move(node));
	memory.SetHasLiveQueueEntry(lock, success);
	return success;
}

bool EvictionQueue::AddToEvictionQueue(BlockLock &lock, BlockMemory &memory, BufferEvictionNode &&node) {
	if (!Enqueue(lock, memory, std::move(node))) {
		return false;
	}
	return ++evict_queue_insertions % INSERT_INTERVAL == 0;
}

void EvictionQueue::DiscardNode(BufferEvictionNode &node) {
	auto memory = node.memory_p.lock();
	if (!memory) {
		DecrementDeadNodes();
		return;
	}
	auto lock = memory->GetLock();
	if (node.handle_sequence_number != memory->GetEvictionSequenceNumber()) {
		DecrementDeadNodes();
	} else {
		memory->SetHasLiveQueueEntry(lock, false);
	}
}

bool EvictionQueue::TryDequeueWithLock(BufferEvictionNode &node) {
	lock_guard<mutex> lock(purge_lock);
	return q.try_dequeue(node);
}

void EvictionQueue::Purge() {
	// only one thread purges the queue, all other threads early-out
	unique_lock<mutex> guard(purge_lock, std::try_to_lock);
	if (!guard.owns_lock()) {
		return;
	}

	// we purge INSERT_INTERVAL * PURGE_SIZE_MULTIPLIER nodes
	idx_t purge_size = INSERT_INTERVAL * PURGE_SIZE_MULTIPLIER;

	// get an estimate of the queue size as-of now
	idx_t approx_q_size = q.size_approx();

	// early-out, if the queue is not big enough to justify purging
	// - we want to keep the LRU characteristic alive
	if (approx_q_size < purge_size * EARLY_OUT_MULTIPLIER) {
		return;
	}

	// There are two types of situations.

	// For most scenarios, purging INSERT_INTERVAL * PURGE_SIZE_MULTIPLIER nodes is enough.
	// Purging more nodes than we insert also counters oscillation for scenarios where most nodes are dead.
	// If we always purge slightly more, we trigger a purge less often, as we purge below the trigger.

	// However, if the pressure on the queue becomes too contested, we need to purge more aggressively,
	// i.e., we actively seek a specific number of dead nodes to purge. We use the total number of existing dead nodes.
	// We detect this situation by observing the queue's ratio between alive vs. dead nodes. If the ratio of alive vs.
	// dead nodes grows faster than we can purge, we keep purging until we hit one of the following conditions.

	// 2.1. We're back at an approximate queue size less than purge_size * EARLY_OUT_MULTIPLIER.
	// 2.2. We're back at a ratio of 1*alive_node:ALIVE_NODE_MULTIPLIER*dead_nodes.
	// 2.3. We've purged the entire queue: max_purges is zero. This is a worst-case scenario,
	// guaranteeing that we always exit the loop.

	idx_t max_purges = approx_q_size / purge_size;

	while (max_purges != 0) {
		PurgeIteration(purge_size);

		// update relevant sizes and potentially early-out
		approx_q_size = q.size_approx();

		// early-out according to (2.1)
		if (approx_q_size < purge_size * EARLY_OUT_MULTIPLIER) {
			break;
		}

		idx_t approx_dead_nodes = total_dead_nodes;
		approx_dead_nodes = approx_dead_nodes > approx_q_size ? approx_q_size : approx_dead_nodes;
		idx_t approx_alive_nodes = approx_q_size - approx_dead_nodes;

		// early-out according to (2.2)
		if (approx_alive_nodes * (ALIVE_NODE_MULTIPLIER - 1) > approx_dead_nodes) {
			break;
		}

		max_purges--;
	}
}

void EvictionQueue::PurgeIteration(const idx_t purge_size) {
	// if this purge is significantly smaller or bigger than the previous purge, then
	// we need to resize the purge_nodes vector. Note that this barely happens, as we
	// purge queue_insertions * PURGE_SIZE_MULTIPLIER nodes
	idx_t previous_purge_size = purge_nodes.size();
	if (purge_size < previous_purge_size / 2 || purge_size > previous_purge_size) {
		purge_nodes.resize(purge_size);
	}

	// Dequeue using consumer token — progresses through sub-queues sequentially
	const idx_t actually_dequeued = q.try_dequeue_bulk(purge_consumer_token, purge_nodes.begin(), purge_size);
	if (actually_dequeued == 0) {
		return;
	}

	idx_t dead_count = 0;
	idx_t alive_count = 0;
	auto raw_sleep_micros = debug_eviction_queue_sleep.load(std::memory_order_relaxed);
	optional_idx debug_sleep_micros = raw_sleep_micros > 0 ? optional_idx(raw_sleep_micros) : optional_idx();
	for (idx_t i = 0; i < actually_dequeued; i++) {
		auto &node = purge_nodes[i];
		if (node.IsDeadNode(debug_sleep_micros)) {
			dead_count++;
		} else {
			// Move alive nodes to the front for bulk re-enqueue
			purge_nodes[alive_count++] = std::move(node);
		}
	}

	total_dead_nodes -= dead_count;

	// Re-enqueue alive nodes via producer token — goes into a dedicated sub-queue
	// that the consumer token has already passed
	if (alive_count > 0) {
#ifdef DUCKDB_NO_THREADS
		// The fallback queue can throw after publishing a prefix of a bulk insertion.
		for (idx_t i = 0; i < alive_count; i++) {
			try {
				q.enqueue(purge_nodes[i]);
			} catch (...) {
				for (idx_t j = i; j < alive_count; j++) {
					DiscardNode(purge_nodes[j]);
				}
				throw;
			}
		}
#else
		if (!purge_producer_token.valid() || !q.enqueue_bulk(purge_producer_token, purge_nodes.begin(), alive_count)) {
			for (idx_t i = 0; i < alive_count; i++) {
				DiscardNode(purge_nodes[i]);
			}
		}
#endif
	}
}

BufferPool::BufferPool(BlockAllocator &block_allocator, idx_t maximum_memory, bool track_eviction_timestamps,
                       idx_t allocator_bulk_deallocation_flush_threshold)
    : eviction_queue_sizes({BLOCK_AND_EXTERNAL_FILE_QUEUE_SIZE, MANAGED_BUFFER_QUEUE_SIZE, TINY_BUFFER_QUEUE_SIZE}),
      maximum_memory(maximum_memory),
      allocator_bulk_deallocation_flush_threshold(allocator_bulk_deallocation_flush_threshold),
      track_eviction_timestamps(track_eviction_timestamps),
      temporary_memory_manager(make_uniq<TemporaryMemoryManager>()), block_allocator(block_allocator) {
	for (idx_t queue_type_idx = 0; queue_type_idx < EVICTION_QUEUE_TYPES; queue_type_idx++) {
		const auto types = EvictionQueueTypeIdxToFileBufferTypes(queue_type_idx);
		const auto &type_queue_size = eviction_queue_sizes[queue_type_idx];
		for (idx_t queue_idx = 0; queue_idx < type_queue_size; queue_idx++) {
			queues.push_back(make_uniq<EvictionQueue>(types));
		}
	}
}
BufferPool::~BufferPool() {
}

bool BufferPool::AddToEvictionQueue(BlockLock &lock, shared_ptr<BlockHandle> &handle) {
	auto &memory = handle->GetMemory();
	// Verify the caller passed this block's lock before we mutate any of its state.
	// The block lock is held throughout: Unpin holds it; ConvertToPersistent acquires the
	// (uncontended) lock of the freshly created block before calling.
	memory.VerifyMutex(lock);
	D_ASSERT(memory.GetReaders() == 0);
	// Repeated hash-table appends can reuse an entry while leaving the block evictable.
	if (!track_eviction_timestamps && memory.GetBufferType() == FileBufferType::MANAGED_BUFFER &&
	    memory.GetMemoryTag() == MemoryTag::HASH_TABLE && memory.HasLiveQueueEntry(lock)) {
		return false;
	}
	auto &queue = GetEvictionQueueForBlockMemory(memory);

	if (memory.HasLiveQueueEntry(lock)) {
		// Count the previous live entry before bumping the sequence number. PurgeIteration
		// reads sequence numbers without the block lock; bumping first could let it see the
		// previous entry as stale and decrement dead_nodes before this matching increment.
		queue.IncrementDeadNodes();
	}

	auto ts = memory.NextEvictionSequenceNumber(lock);
	if (track_eviction_timestamps) {
		memory.SetLRUTimestamp(std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now())
		                           .time_since_epoch()
		                           .count());
	}
	// Get the eviction queue for the block and add it
	BufferEvictionNode node(handle->GetMemoryWeak(), ts);
	return queue.AddToEvictionQueue(lock, memory, std::move(node));
}

EvictionQueue &BufferPool::GetEvictionQueueForBlockMemory(const BlockMemory &memory) {
	const auto &handle_buffer_type = memory.GetBufferType();

	// Get offset into eviction queues for this FileBufferType
	idx_t queue_index = 0;
	const auto handle_queue_type_idx = FileBufferTypeToEvictionQueueTypeIdx(handle_buffer_type);
	for (idx_t type_idx = 0; type_idx < handle_queue_type_idx; type_idx++) {
		queue_index += eviction_queue_sizes[type_idx];
	}

	const auto &queue_size = eviction_queue_sizes[handle_queue_type_idx];
	// Adjust if eviction_queue_idx is set (idx == 0 -> add at back, idx >= queue_size -> add at front)
	auto eviction_queue_idx = memory.GetEvictionQueueIndex();
	if (eviction_queue_idx < queue_size) {
		queue_index += queue_size - eviction_queue_idx - 1;
	}

	D_ASSERT(queues[queue_index]->HasFileBufferType(handle_buffer_type));
	return *queues[queue_index];
}

void BufferPool::IncrementDeadNodes(const BlockMemory &memory) {
	GetEvictionQueueForBlockMemory(memory).IncrementDeadNodes();
}

void BufferPool::UpdateUsedMemory(MemoryTag tag, int64_t size) {
	memory_usage.UpdateUsedMemory(tag, size);
}

idx_t BufferPool::GetUsedMemory(bool flush) const {
	return memory_usage.GetUsedMemory(flush ? MemoryUsageCaches::FLUSH : MemoryUsageCaches::NO_FLUSH);
}

idx_t BufferPool::GetMaxMemory() const {
	return maximum_memory;
}

idx_t BufferPool::GetOperatorMemoryLimit() const {
	return GetMaxMemory();
}

TemporaryMemoryManager &BufferPool::GetTemporaryMemoryManager() {
	return *temporary_memory_manager;
}

BufferPool::EvictionResult BufferPool::EvictObjectCacheEntries(MemoryTag tag, idx_t extra_memory, idx_t memory_limit) {
	TempBufferPoolReservation r(tag, *this, extra_memory);

	if (!object_cache->EvictToMemoryLimit(memory_limit)) {
		r.Resize(0);
		return {false, std::move(r)};
	}
	FlushForAllocation(extra_memory, memory_limit);
	return {true, std::move(r)};
}

void BufferPool::FlushForAllocation(const idx_t extra_memory, const idx_t memory_limit) {
	static constexpr idx_t BULK_DEALLOCATION_FLUSH_DIVISOR = 16;
	const auto used_memory = GetUsedMemory(false);
	const auto memory_headroom = memory_limit - MinValue(memory_limit, used_memory);
	if (extra_memory > allocator_bulk_deallocation_flush_threshold) {
		block_allocator.FlushForAllocation(extra_memory, memory_headroom);
	} else {
		block_allocator.TryFlushDeallocated(memory_limit / BULK_DEALLOCATION_FLUSH_DIVISOR, memory_headroom);
	}
}

BufferPool::EvictionResult BufferPool::EvictBlocks(QueryContext context, MemoryTag tag, idx_t extra_memory,
                                                   idx_t memory_limit, unique_ptr<FileBuffer> *buffer) {
	for (auto &queue : queues) {
		auto block_result = EvictBlocksInternal(context, *queue, tag, extra_memory, memory_limit, buffer);
		if (block_result.success) {
			return block_result;
		}
	}

	// Evict object cache, which is usually used to cache metadata and configs, when flushing buffer blocks alone is not
	// enough to limit overall memory consumption.
	return EvictObjectCacheEntries(tag, extra_memory, memory_limit);
}

BufferPool::EvictionResult BufferPool::EvictBlocksInternal(QueryContext context, EvictionQueue &queue, MemoryTag tag,
                                                           idx_t extra_memory, idx_t memory_limit,
                                                           unique_ptr<FileBuffer> *buffer) {
	TempBufferPoolReservation r(tag, *this, extra_memory);
	bool found = false;

	if (memory_usage.GetUsedMemory(MemoryUsageCaches::NO_FLUSH) <= memory_limit) {
		FlushForAllocation(extra_memory, memory_limit);
		return {true, std::move(r)};
	}

	queue.IterateUnloadableBlocks([&](BufferEvictionNode &, const shared_ptr<BlockMemory> &handle, BlockLock &lock) {
		// hooray, we can unload the block
		if (buffer && handle->GetBuffer(lock)->AllocSize() == extra_memory) {
			// we can re-use the memory directly
			*buffer = handle->UnloadAndTakeBlock(lock, context);
			found = true;
			return false;
		}

		// release the memory and mark the block as unloaded
		handle->Unload(lock, context);

		if (memory_usage.GetUsedMemory(MemoryUsageCaches::NO_FLUSH) <= memory_limit) {
			found = true;
			return false;
		}

		// Continue iteration
		return true;
	});

	if (!found) {
		r.Resize(0);
	} else {
		FlushForAllocation(extra_memory, memory_limit);
	}

	return {found, std::move(r)};
}

//! Do not remove this method.
//! There are extensions that rely on time-based purging of blocks, that uses the method.
idx_t BufferPool::PurgeAgedBlocks(uint32_t max_age_sec) {
	int64_t now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now())
	                  .time_since_epoch()
	                  .count();
	int64_t limit = now - (static_cast<int64_t>(max_age_sec) * 1000);
	idx_t purged_bytes = 0;
	for (auto &queue : queues) {
		purged_bytes += PurgeAgedBlocksInternal(*queue, max_age_sec, now, limit);
	}
	return purged_bytes;
}

idx_t BufferPool::PurgeAgedBlocksInternal(EvictionQueue &queue, uint32_t max_age_sec, int64_t now, int64_t limit) {
	idx_t purged_bytes = 0;
	queue.IterateUnloadableBlocks(
	    [&](BufferEvictionNode &node, const shared_ptr<BlockMemory> &handle, BlockLock &lock) {
		    // We will unload this block regardless. But stop the iteration immediately afterward if this
		    // block is younger than the age threshold.
		    auto lru_timestamp_msec = handle->GetLRUTimestamp();
		    bool is_fresh = lru_timestamp_msec >= limit && lru_timestamp_msec <= now;
		    purged_bytes += handle->GetMemoryUsage();
		    handle->Unload(lock);
		    // Return false to stop iterating if the current block is_fresh
		    return !is_fresh;
	    });
	return purged_bytes;
}

template <typename FN>
void EvictionQueue::IterateUnloadableBlocks(FN fn) {
	auto debug_sleep_micros = debug_eviction_queue_sleep.load(std::memory_order_relaxed);
	for (;;) {
		// get a block to unpin from the queue
		BufferEvictionNode node;
		// Purge parks older live entries in its own producer. Tokenless dequeue can
		// keep selecting newer producers while that queue is non-empty.
		const bool from_purged =
		    purge_producer_token.valid() && q.try_dequeue_from_producer(purge_producer_token, node);
		if (!from_purged && !q.try_dequeue(node)) {
			// we could not dequeue any eviction node, so we try one more time,
			// but more aggressively
			if (!TryDequeueWithLock(node)) {
				return;
			}
		}

		// get a reference to the underlying block pointer
		auto handle = node.memory_p.lock();
		if (debug_sleep_micros > 0) {
			// Debug race conditions regarding the ownership of the BlockMemory.
			// Note that for this to trigger we need at least one purge iteration with the setting active.
			ThreadUtil::SleepMicroSeconds(debug_sleep_micros);
		}
		if (!handle) {
			DecrementDeadNodes();
			continue;
		}

		// we might be able to free this block: grab the mutex and check if we can free it
		auto lock = handle->GetLock();
		if (node.handle_sequence_number != handle->GetEvictionSequenceNumber()) {
			// A newer entry superseded this node: it was counted as dead when that entry was added.
			DecrementDeadNodes();
			continue;
		}
		// This node is the block's live queue entry, and we just dequeued it: the block no longer
		// has an entry in the queue. Live entries are never counted as dead, so no decrement.
		handle->SetHasLiveQueueEntry(lock, false);
		switch (handle->CanUnload()) {
		case CanUnloadResult::CAN_UNLOAD:
			break;
		case CanUnloadResult::NO_TEMP_DIRECTORY:
			// Unpinned temporary block that cannot be offloaded yet: re-enqueue until temp directory is set.
			Enqueue(lock, *handle, std::move(node));
			return;
		case CanUnloadResult::PINNED:
		case CanUnloadResult::ALREADY_UNLOADED:
			// The block cannot be unloaded right now (e.g. it is pinned). It gets a new queue
			// entry when it is unpinned again.
			continue;
		}

		bool continue_iteration;
		try {
			continue_iteration = fn(node, handle, lock);
		} catch (...) {
			// The unload failed (e.g. the temporary directory is full) and the block is still loaded.
			// Give it its queue entry back, or it stays un-evictable until the next unpin.
			Enqueue(lock, *handle, std::move(node));
			throw;
		}
		if (!continue_iteration) {
			break;
		}
	}
}

void BufferPool::PurgeQueue(const BlockHandle &block) {
	const auto &memory = block.GetMemory();
	const auto &buffer_manager = memory.GetBufferManager();
	auto &eviction_queue = GetEvictionQueueForBlockMemory(memory);
	const auto queue_sleep_micros =
	    Settings::Get<DebugEvictionQueueSleepMicroSecondsSetting>(buffer_manager.GetDatabase());
	eviction_queue.debug_eviction_queue_sleep = queue_sleep_micros;
	eviction_queue.Purge();
}

void BufferPool::SetLimit(idx_t limit, const char *exception_postscript) {
	lock_guard<mutex> l_lock(limit_lock);
	// try to evict until the limit is reached
	if (!EvictBlocks(QueryContext(), MemoryTag::EXTENSION, 0, limit).success) {
		throw OutOfMemoryException(
		    "Failed to change memory limit to %lld: could not free up enough memory for the new limit%s", limit,
		    exception_postscript);
	}
	idx_t old_limit = maximum_memory;
	// set the global maximum memory to the new limit if successful
	maximum_memory = limit;
	// evict again
	if (!EvictBlocks(QueryContext(), MemoryTag::EXTENSION, 0, limit).success) {
		// failed: go back to old limit
		maximum_memory = old_limit;
		throw OutOfMemoryException(
		    "Failed to change memory limit to %lld: could not free up enough memory for the new limit%s", limit,
		    exception_postscript);
	}
	block_allocator.FlushAll();
}

void BufferPool::SetAllocatorBulkDeallocationFlushThreshold(idx_t threshold) {
	allocator_bulk_deallocation_flush_threshold = threshold;
}

idx_t BufferPool::GetAllocatorBulkDeallocationFlushThreshold() {
	return allocator_bulk_deallocation_flush_threshold;
}

vector<EvictionQueueInformation> BufferPool::GetEvictionQueueInfo() const {
	static const char *QUEUE_TYPE_NAMES[] = {"BLOCK_AND_EXTERNAL_FILE", "MANAGED_BUFFER", "TINY_BUFFER"};
	vector<EvictionQueueInformation> result;
	idx_t global_queue_index = 0;
	for (idx_t type_idx = 0; type_idx < EVICTION_QUEUE_TYPES; type_idx++) {
		const auto &type_queue_size = eviction_queue_sizes[type_idx];
		for (idx_t local_queue_idx = 0; local_queue_idx < type_queue_size; local_queue_idx++) {
			auto &queue = *queues[global_queue_index];
			EvictionQueueInformation info;
			info.queue_index = global_queue_index;
			info.queue_type = QUEUE_TYPE_NAMES[type_idx];
			info.approximate_size = queue.GetApproximateSize();
			info.dead_nodes = queue.GetDeadNodes();
			info.total_insertions = queue.GetTotalInsertions();
			result.push_back(std::move(info));
			global_queue_index++;
		}
	}
	return result;
}

BufferPool::MemoryUsage::MemoryUsage() {
	for (auto &v : memory_usage) {
		v = 0;
	}
	for (auto &cache : memory_usage_caches) {
		for (auto &v : cache.counters) {
			v = 0;
		}
	}
}

void BufferPool::MemoryUsage::UpdateUsedMemory(MemoryTag tag, int64_t size) {
	if (size == 0) {
		return;
	}
	auto tag_idx = static_cast<idx_t>(tag);
	if (static_cast<idx_t>(AbsValue(size)) < MEMORY_USAGE_TAG_CACHE_THRESHOLD) {
		// update cache and update global counter when cache exceeds threshold
		// Get corresponding cache slot based on current CPU core index
		// Two threads may access the same cache simultaneously,
		// ensuring correctness through atomic operations
		auto cache_idx = static_cast<idx_t>(TaskScheduler::GetEstimatedCPUId()) % MEMORY_USAGE_CACHE_COUNT;
		auto &cache = memory_usage_caches[cache_idx].counters;
		auto new_tag_size = cache[tag_idx].fetch_add(size, std::memory_order_relaxed) + size;
		if (static_cast<idx_t>(AbsValue(new_tag_size)) >= MEMORY_USAGE_TAG_CACHE_THRESHOLD) {
			// cached tag memory usage exceeds threshold
			auto tag_size = cache[tag_idx].exchange(0, std::memory_order_relaxed);
			memory_usage[tag_idx].fetch_add(tag_size, std::memory_order_relaxed);
		}
		if (static_cast<idx_t>(AbsValue(size)) >= MEMORY_USAGE_CACHE_THRESHOLD) {
			memory_usage[TOTAL_MEMORY_USAGE_INDEX].fetch_add(size, std::memory_order_relaxed);
			return;
		}
		auto new_total_size = cache[TOTAL_MEMORY_USAGE_INDEX].fetch_add(size, std::memory_order_relaxed) + size;
		if (static_cast<idx_t>(AbsValue(new_total_size)) >= MEMORY_USAGE_CACHE_THRESHOLD) {
			// cached total memory usage exceeds threshold
			auto total_size = cache[TOTAL_MEMORY_USAGE_INDEX].exchange(0, std::memory_order_relaxed);
			memory_usage[TOTAL_MEMORY_USAGE_INDEX].fetch_add(total_size, std::memory_order_relaxed);
		}
	} else {
		// update global counter
		memory_usage[tag_idx].fetch_add(size, std::memory_order_relaxed);
		memory_usage[TOTAL_MEMORY_USAGE_INDEX].fetch_add(size, std::memory_order_relaxed);
	}
}

} // namespace duckdb
