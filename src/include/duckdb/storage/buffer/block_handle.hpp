//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/block_handle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

// Forward declaration.
class BlockManager;
class BufferHandle;
class BufferManager;
class DatabaseInstance;
class BlockHandle;

using BlockLock = unique_lock<mutex>;

enum class CanUnloadResult : uint8_t { CAN_UNLOAD, ALREADY_UNLOADED, PINNED, NO_TEMP_DIRECTORY };

class BlockMemory : public enable_shared_from_this<BlockMemory> {
public:
	BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p, idx_t block_alloc_size);
	BlockMemory(BufferManager &buffer_manager, block_id_t block_id_p, MemoryTag tag_p, unique_ptr<FileBuffer> buffer_p,
	            DestroyBufferUpon destroy_buffer_upon_p, idx_t size_p, BufferPoolReservation &&reservation);
	~BlockMemory();

public:
	//! Returns a const reference to the buffer manager.
	const BufferManager &GetBufferManager() const {
		return buffer_manager;
	}
	//! Returns a reference to the buffer manager.
	BufferManager &GetBufferManager() {
		return buffer_manager;
	}
	//! Returns the block ID.
	block_id_t BlockId() const {
		return block_id;
	}
	//! Locks the memory block.
	BlockLock GetLock() {
		return BlockLock(lock);
	}
	//! Verification-only: ensure that the lock matches this memory's lock.
	void VerifyMutex(BlockLock &l) const {
		D_ASSERT(l.owns_lock());
		D_ASSERT(l.mutex() == &lock);
	}
	//! Returns the block state.
	BlockState GetState() const {
		return state;
	}
	//! Sets the block state.
	void SetState(BlockState state_p) {
		state = state_p;
	}
	//! Returns true, if the block state is BLOCK_UNLOADED.
	bool IsUnloaded() const {
		return state == BlockState::BLOCK_UNLOADED;
	}
	//! Returns the number of readers.
	int32_t GetReaders() const {
		return readers.load(std::memory_order_relaxed);
	}
	//! Increments the number of readers prior to returning it.
	int32_t IncrementReaders(BlockLock &l) {
		VerifyMutex(l);
		if (readers.load(std::memory_order_relaxed) == 0) {
			// Without readers, only the lock holder updates the count
			readers.store(1, std::memory_order_release);
			return 1;
		}
		return readers.fetch_add(1, std::memory_order_acq_rel) + 1;
	}
	//! Decrements the number of readers prior to returning it.
	int32_t DecrementReaders(BlockLock &l) {
		VerifyMutex(l);
		return readers.fetch_sub(1, std::memory_order_acq_rel) - 1;
	}
	//! Adds a reader without the block lock if the block is already pinned
	bool TryIncrementPinnedReaders() {
		auto current = readers.load(std::memory_order_relaxed);
		return current > 0 && readers.compare_exchange_strong(current, current + 1, std::memory_order_acquire,
		                                                      std::memory_order_relaxed);
	}
	//! Removes a reader without the block lock if it is not the last one
	bool TryDecrementPinnedReaders() {
		auto current = readers.load(std::memory_order_relaxed);
		return current > 1 && readers.compare_exchange_strong(current, current - 1, std::memory_order_release,
		                                                      std::memory_order_relaxed);
	}
	//! Returns the memory tag.
	MemoryTag GetMemoryTag() const {
		return tag;
	}
	//! Returns the file buffer type.
	FileBufferType GetBufferType() const {
		return buffer_type;
	}
	//! Returns a reference to the unique file buffer pointer while holding the block lock.
	unique_ptr<FileBuffer> &GetBuffer(BlockLock &l) {
		VerifyMutex(l);
		return GetBuffer();
	}
	//! Returns a reference to the unique file buffer pointer.
	unique_ptr<FileBuffer> &GetBuffer() {
		return buffer;
	}
	//! Sets the file buffer.
	void SetBuffer(unique_ptr<FileBuffer> buffer_p) {
		buffer = std::move(buffer_p);
	}
	//! Returns the eviction sequence number.
	idx_t GetEvictionSequenceNumber() const {
		return eviction_seq_num.load(std::memory_order_acquire);
	}
	//! Increments the eviction sequence number prior to returning it.
	idx_t NextEvictionSequenceNumber(BlockLock &l) {
		VerifyMutex(l);
		auto new_sequence_number = eviction_seq_num.load(std::memory_order_relaxed) + 1;
		// Publish preceding dead-node accounting to lock-free queue readers.
		eviction_seq_num.store(new_sequence_number, std::memory_order_release);
		return new_sequence_number;
	}
	//! Returns true, if the block has a live (not yet dead-counted) entry in the eviction queue.
	bool HasLiveQueueEntry(BlockLock &l) const {
		VerifyMutex(l);
		return has_queue_entry;
	}
	//! Lock-free overload of HasLiveQueueEntry. Only safe for callers with exclusive ownership of the
	//! block memory (i.e., the destructor).
	bool HasLiveQueueEntry() const {
		return has_queue_entry;
	}
	//! Marks whether the block has a live entry in the eviction queue. Requires the block lock.
	void SetHasLiveQueueEntry(BlockLock &l, bool has_queue_entry_p) {
		VerifyMutex(l);
		has_queue_entry = has_queue_entry_p;
	}
	//! Get the LRU timestamp.
	int64_t GetLRUTimestamp() const {
		return lru_timestamp_msec;
	}
	//! Set the LRU timestamp.
	void SetLRUTimestamp(int64_t timestamp_msec) {
		lru_timestamp_msec = timestamp_msec;
	}
	//! Set the buffer destruction policy.
	void SetDestroyBufferUpon(DestroyBufferUpon destroy_buffer_upon_p) {
		destroy_buffer_upon = destroy_buffer_upon_p;
	}
	//! Returns true, if the buffer must be added to the eviction queue.
	bool MustAddToEvictionQueue() const {
		return destroy_buffer_upon != DestroyBufferUpon::UNPIN;
	}
	//! Returns true, if the buffer cannot be destroyed, but must be kept alive in a temporary file.
	bool MustWriteToTemporaryFile() const {
		return destroy_buffer_upon == DestroyBufferUpon::BLOCK;
	}
	//! Returns true, if the buffer can be loaded again after it was destroyed, i.e., it is backed
	//! by its disk block or kept alive in a temporary file. False for buffers that are destroyed
	//! upon eviction/unpin - including replaced persistent handles whose disk block may be
	//! rewritten in place and must not be read through them again.
	bool CanReload() const {
		return destroy_buffer_upon == DestroyBufferUpon::BLOCK;
	}
	//! Returns the memory usage.
	idx_t GetMemoryUsage() const {
		return memory_usage;
	}
	//! Sets the memory usage.
	void SetMemoryUsage(idx_t usage) {
		memory_usage = usage;
	}
	//! Get the memory charge while holding the block lock.
	BufferPoolReservation &GetMemoryCharge(BlockLock &l) {
		VerifyMutex(l);
		return GetMemoryCharge();
	}
	//! Get the memory charge.
	BufferPoolReservation &GetMemoryCharge() {
		return memory_charge;
	}
	//! Resize the memory charge.
	void ResizeMemory(BlockLock &l, idx_t alloc_size) {
		VerifyMutex(l);
		memory_charge.Resize(alloc_size);
	}
	//! Merge two memory charges.
	void MergeMemoryReservation(BlockLock &l, BufferPoolReservation reservation) {
		VerifyMutex(l);
		memory_charge.Merge(std::move(reservation));
	}
	//! Sets the eviction queue index.
	void SetEvictionQueueIndex(const idx_t index) {
		// The index can only be set once.
		D_ASSERT(eviction_queue_idx == DConstants::INVALID_INDEX);
		// It can only be set for managed buffers (for now).
		D_ASSERT(GetBufferType() == FileBufferType::MANAGED_BUFFER);
		eviction_queue_idx = index;
	}
	//! Returns the eviction queue index.
	idx_t GetEvictionQueueIndex() const {
		return eviction_queue_idx;
	}

public:
	void ChangeMemoryUsage(BlockLock &l, int64_t delta);
	void ConvertToPersistent(BlockLock &l, BlockHandle &new_block, unique_ptr<FileBuffer> new_buffer);
	void ResizeBuffer(BlockLock &l, idx_t block_size, idx_t block_header_size, int64_t memory_delta);
	//! Returns whether the block can be unloaded or the reason why it cannot be unloaded.
	//! The state here can change if the block lock is held.
	//! However, this method does not hold the block lock.
	CanUnloadResult CanUnload() const;
	unique_ptr<FileBuffer> UnloadAndTakeBlock(BlockLock &l, QueryContext context = QueryContext());
	void Unload(BlockLock &l, QueryContext context = QueryContext());

private:
	//! A reference to the buffer manager.
	BufferManager &buffer_manager;
	//! The block id of the block.
	const block_id_t block_id;
	//! The block-level lock.
	mutex lock;
	//! Whether the block is loaded or unloaded.
	atomic<BlockState> state;
	//! The number of concurrent readers. Only the block lock holder changes it from or to zero.
	atomic<int32_t> readers;
	//! The memory tag.
	const MemoryTag tag;
	//! The file buffer type.
	const FileBufferType buffer_type;
	//! A pointer to the loaded data, if any.
	unique_ptr<FileBuffer> buffer;
	//! The internal eviction sequence number. Monotonic: it is never reset, so an eviction queue
	//! entry is stale if and only if its sequence number differs from this one. Writers hold the block lock.
	atomic<idx_t> eviction_seq_num;
	//! Whether the block has a live entry in the eviction queue, i.e., an entry whose sequence
	//! number matches eviction_seq_num and which has not been counted as a dead node.
	//! Guarded by the block lock (read without it only by the destructor, which has exclusive ownership).
	bool has_queue_entry;
	//! The LRU timestamp for age-based eviction.
	atomic<int64_t> lru_timestamp_msec;
	//! When to destroy the data buffer.
	atomic<DestroyBufferUpon> destroy_buffer_upon;
	//! The memory usage of the block when loaded.
	//! Determines the memory to reserve when pinning/loading an unloaded block.
	atomic<idx_t> memory_usage;
	//! The current memory reservation/usage.
	BufferPoolReservation memory_charge;
	//! The eviction queue index, currently only FileBufferType::MANAGED_BUFFER.
	atomic<idx_t> eviction_queue_idx;
};

class BlockHandle : public enable_shared_from_this<BlockHandle> {
public:
	BlockHandle(BlockManager &block_manager, block_id_t block_id, MemoryTag tag);
	BlockHandle(BlockManager &block_manager, block_id_t block_id, MemoryTag tag, unique_ptr<FileBuffer> buffer,
	            DestroyBufferUpon destroy_buffer_upon, idx_t size, BufferPoolReservation &&reservation);
	~BlockHandle();

public:
	//! Returns a reference to the block manager.
	BlockManager &GetBlockManager() const {
		return block_manager;
	}
	//! Returns the block id.
	block_id_t BlockId() const {
		return block_id;
	}
	//! Returns the block allocation size of this block.
	idx_t GetBlockAllocSize() const {
		return block_alloc_size;
	}
	//! Returns the block header size including the 8-byte checksum.
	idx_t GetBlockHeaderSize() const {
		return block_header_size;
	}
	//! Returns the size of the block that is available for usage, as determined by the block manager that created the
	//! block. The block_alloc_size can differ from the memory_usage for blocks managed by the temporary block manager,
	//! thus, this should only be called for persistent blocks.
	idx_t GetBlockSize() const {
		return block_alloc_size - block_header_size;
	}
	//! Returns a const reference to the memory of a block.
	const BlockMemory &GetMemory() const {
		return memory;
	}
	//! Returns a reference to the memory of a block.
	BlockMemory &GetMemory() {
		return memory;
	}
	//! Returns a weak pointer to the memory of a block.
	weak_ptr<BlockMemory> GetMemoryWeak() const {
		return weak_ptr<BlockMemory>(memory_p);
	}

public:
	BufferHandle LoadFromBuffer(BlockLock &l, data_ptr_t data, unique_ptr<FileBuffer> reusable_buffer,
	                            BufferPoolReservation reservation);
	BufferHandle Load(BlockLock &l, QueryContext context, unique_ptr<FileBuffer> buffer = nullptr);

private:
	//! The block manager, which loads the block.
	BlockManager &block_manager;

	//! The block allocation size, which is determined by the block manager creating the block.
	//! For non-temporary block managers the block_alloc_size corresponds to the memory_usage.
	//! If we are pinning/loading an unloaded block, then we know how much memory to reserve.
	//! This is NOT the actual memory available on a block.
	idx_t block_alloc_size;
	//! The size of the block header, including the checksum.
	idx_t block_header_size;
	//! The block id of the block.
	const block_id_t block_id;

	//! Pointer to the underlying memory of the block.
	const shared_ptr<BlockMemory> memory_p;
	//! Memory for fast access to the block memory.
	BlockMemory &memory;
};

} // namespace duckdb
