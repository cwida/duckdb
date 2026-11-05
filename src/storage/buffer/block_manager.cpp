#include "duckdb/storage/block_manager.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"

namespace duckdb {

BlockManager::BlockManager(BufferManager &buffer_manager, const optional_idx block_alloc_size_p,
                           const optional_idx block_header_size_p)
    : buffer_manager(buffer_manager), metadata_manager(make_uniq<MetadataManager>(*this, buffer_manager)),
      block_alloc_size(block_alloc_size_p), block_header_size(block_header_size_p) {
}

bool BlockManager::BlockIsRegistered(block_id_t block_id) {
	lock_guard<mutex> lock(blocks_lock);
	// check if the block already exists
	auto entry = blocks.find(block_id);
	if (entry == blocks.end()) {
		return false;
	}
	// already exists: check if it hasn't expired yet
	return !entry->second.expired();
}

shared_ptr<BlockHandle> BlockManager::TryGetBlock(block_id_t block_id) {
	lock_guard<mutex> lock(blocks_lock);
	// check if the block already exists
	auto entry = blocks.find(block_id);
	if (entry == blocks.end()) {
		// the block does not exist
		return nullptr;
	}
	// the block exists - try to lock it
	return entry->second.lock();
}

shared_ptr<BlockHandle> BlockManager::RegisterBlock(block_id_t block_id, MemoryTag tag) {
	lock_guard<mutex> lock(blocks_lock);
	// check if the block already exists
	auto entry = blocks.find(block_id);
	if (entry != blocks.end()) {
		// already exists: check if it hasn't expired yet
		auto existing_ptr = entry->second.lock();
		if (existing_ptr) {
			//! it hasn't! return it
			D_ASSERT(existing_ptr->GetMemory().GetMemoryTag() == tag);
			return existing_ptr;
		}
	}
	// create a new block pointer for this block
	auto result = make_shared_ptr<BlockHandle>(*this, block_id, tag);
	// register the block pointer in the set of blocks as a weak pointer
	blocks[block_id] = weak_ptr<BlockHandle>(result);
	return result;
}

shared_ptr<BlockHandle> BlockManager::ConvertToPersistent(QueryContext context, block_id_t block_id,
                                                          shared_ptr<BlockHandle> old_block, BufferHandle old_handle,
                                                          ConvertToPersistentMode mode, optional<MemoryTag> tag) {
	auto default_tag = old_block->GetMemory().GetMemoryTag();
	if (default_tag == MemoryTag::IN_MEMORY_TABLE) {
		default_tag = MemoryTag::BASE_TABLE;
	}
	// register a block with the new block id
	auto new_block = RegisterBlock(block_id, tag.value_or(default_tag));
	D_ASSERT(new_block->GetMemory().GetState() == BlockState::BLOCK_UNLOADED);
	D_ASSERT(new_block->GetMemory().GetReaders() == 0);

	if (mode == ConvertToPersistentMode::THREAD_SAFE) {
		// safe mode - create a copy of the old block and operate on that
		// this ensures we don't modify the old block - which allows other concurrent operations on the old block to
		// continue
		auto old_block_copy = buffer_manager.AllocateMemory(old_block->GetMemory().GetMemoryTag(), this, false);
		auto copy_pin = buffer_manager.Pin(old_block_copy);
		memcpy(copy_pin.GetDataMutable(), old_handle.Ptr(), GetBlockSize());
		old_block = std::move(old_block_copy);
		old_handle = std::move(copy_pin);
	}

	auto &old_block_memory = old_block->GetMemory();
	auto lock = old_block_memory.GetLock();
	D_ASSERT(old_block_memory.GetState() == BlockState::BLOCK_LOADED);
	D_ASSERT(old_block_memory.GetBuffer(lock));
	if (old_block_memory.GetReaders() > 1) {
		throw InternalException(
		    "BlockManager::ConvertToPersistent in destructive mode - cannot be called for block %d as old_block has "
		    "multiple readers active",
		    block_id);
	}

	// Temp buffers can be larger than the storage block size.
	// But persistent buffers cannot.
	D_ASSERT(old_block_memory.GetBuffer(lock)->AllocSize() <= GetBlockAllocSize());

	// convert the buffer to a block
	auto converted_buffer = ConvertBlock(block_id, *old_block_memory.GetBuffer(lock));

	// persist the new block to disk
	Write(context, *converted_buffer, block_id);

	// now convert the actual block
	old_block_memory.ConvertToPersistent(lock, *new_block, std::move(converted_buffer));

	// destroy the old buffer
	lock.unlock();
	old_handle.Destroy();
	old_block.reset();

	// potentially purge the queue
	bool purge_queue;
	{
		// AddToEvictionQueue requires the block lock. new_block was just created here, so this is uncontended.
		auto new_lock = new_block->GetMemory().GetLock();
		purge_queue = buffer_manager.GetBufferPool().AddToEvictionQueue(new_lock, new_block);
	}
	if (purge_queue) {
		buffer_manager.GetBufferPool().PurgeQueue(*new_block);
	}
	return new_block;
}

shared_ptr<BlockHandle> BlockManager::ConvertToPersistent(QueryContext context, block_id_t block_id,
                                                          shared_ptr<BlockHandle> old_block,
                                                          ConvertToPersistentMode mode, optional<MemoryTag> tag) {
	// pin the old block to ensure we have it loaded in memory
	auto handle = buffer_manager.Pin(old_block);
	return ConvertToPersistent(context, block_id, std::move(old_block), std::move(handle), mode, tag);
}

void BlockManager::UnregisterBlock(block_id_t id) {
	D_ASSERT(id < MAXIMUM_BLOCK);
	lock_guard<mutex> lock(blocks_lock);
	// on-disk block: erase from list of blocks in manager
	blocks.erase(id);
}

bool BlockManager::UnregisterExpiredBlock(block_id_t id) {
	D_ASSERT(id < MAXIMUM_BLOCK);
	lock_guard<mutex> lock(blocks_lock);
	auto entry = blocks.find(id);
	if (entry == blocks.end()) {
		// the block id was already unregistered
		return true;
	}
	if (!entry->second.expired()) {
		// a newer handle was registered for this block id - the registration is not ours to remove
		return false;
	}
	blocks.erase(entry);
	return true;
}

void BlockManager::UnregisterPersistentBlock(BlockHandle &block) {
	if (in_destruction) {
		return;
	}
	auto id = block.BlockId();
	D_ASSERT(id < MAXIMUM_BLOCK);
	UnregisterExpiredBlock(id);
}

MetadataManager &BlockManager::GetMetadataManager() {
	return *metadata_manager;
}

void BlockManager::Write(QueryContext context, FileBuffer &block, block_id_t block_id) {
	// Fallback to the old Write.
	Write(block, block_id);
}

void BlockManager::Truncate() {
}

} // namespace duckdb
