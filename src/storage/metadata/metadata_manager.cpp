#include "duckdb/storage/metadata/metadata_manager.hpp"

#include "duckdb/common/serializer/read_stream.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

MetadataBlock::MetadataBlock() : block_id(INVALID_BLOCK), dirty(false) {
}

MetadataBlock::MetadataBlock(MetadataBlock &&other) noexcept : dirty(false) {
	std::swap(block, other.block);
	std::swap(block_id, other.block_id);
	std::swap(free_blocks, other.free_blocks);
	auto dirty_val = dirty.load();
	dirty = other.dirty.load();
	other.dirty = dirty_val;
}

MetadataBlock &MetadataBlock::operator=(MetadataBlock &&other) noexcept {
	std::swap(block, other.block);
	std::swap(block_id, other.block_id);
	std::swap(free_blocks, other.free_blocks);
	auto dirty_val = dirty.load();
	dirty = other.dirty.load();
	other.dirty = dirty_val;
	return *this;
}

string MetadataBlock::ToString() const {
	string result;
	for (idx_t i = 0; i < MetadataManager::METADATA_BLOCK_COUNT; i++) {
		if (std::find(free_blocks.begin(), free_blocks.end(), i) != free_blocks.end()) {
			if (!result.empty()) {
				result += ", ";
			}
			result += to_string(i);
		}
	}
	return "block_id: " + to_string(block_id) + " [" + result + "]";
}

MetadataManager::MetadataManager(BlockManager &block_manager, BufferManager &buffer_manager)
    : block_manager(block_manager), buffer_manager(buffer_manager) {
}

MetadataManager::~MetadataManager() {
}

MetadataHandle MetadataManager::AllocateHandle() {
	// check if there is any free space left in an existing block
	// if not allocate a new block
	MetadataPointer pointer;
	unique_lock<mutex> guard(block_lock);
	block_id_t free_block = INVALID_BLOCK;
	for (auto &kv : blocks) {
		auto &block = kv.second;
		D_ASSERT(kv.first == block.block_id);
		if (!block.free_blocks.empty()) {
			free_block = kv.first;
			break;
		}
	}
	guard.unlock();
	if (free_block == INVALID_BLOCK || free_block > PeekNextBlockId()) {
		free_block = AllocateNewBlock(guard);
	} else {
		guard.lock();
	}
	D_ASSERT(guard.owns_lock());
	D_ASSERT(free_block != INVALID_BLOCK);

	// select the first free metadata block we can find
	pointer.block_index = UnsafeNumericCast<idx_t>(free_block);
	auto &block = blocks[free_block];
	// the block is now dirty
	block.dirty = true;
	if (block.block->BlockId() < MAXIMUM_BLOCK) {
		// this block is a disk-backed block, yet we are planning to write to it
		// we need to convert it into a transient block before we can write to it
		ConvertToTransient(guard, block);
		D_ASSERT(block.block->BlockId() >= MAXIMUM_BLOCK);
	}
	D_ASSERT(!block.free_blocks.empty());
	pointer.index = block.free_blocks.back();
	// mark the block as used
	block.free_blocks.pop_back();
	D_ASSERT(pointer.index < METADATA_BLOCK_COUNT);
	guard.unlock();
	// pin the block
	return Pin(pointer);
}

MetadataHandle MetadataManager::Pin(const MetadataPointer &pointer) {
	return Pin(QueryContext(), pointer);
}

MetadataHandle MetadataManager::Pin(const QueryContext &context, const MetadataPointer &pointer) {
	D_ASSERT(pointer.index < METADATA_BLOCK_COUNT);
	while (true) {
		shared_ptr<BlockHandle> block_handle;
		{
			lock_guard<mutex> guard(block_lock);
			auto entry = blocks.find(UnsafeNumericCast<int64_t>(pointer.block_index));
			if (entry == blocks.end()) {
				throw InternalException("Trying to pin block %llu - but the block did not exist", pointer.block_index);
			}
			auto &block = entry->second;
#ifdef DEBUG
			for (auto &free_block : block.free_blocks) {
				if (free_block == pointer.index) {
					throw InternalException("Pinning block %d.%d but it is marked as a free block", block.block_id,
					                        free_block);
				}
			}
#endif
			block_handle = block.block;
		}

		auto buffer_handle = buffer_manager.Pin(block_handle);
		if (!buffer_handle.IsValid()) {
			// the handle was replaced by ConvertToTransient while we were pinning it and can no
			// longer be loaded from disk - retry with the current handle
			continue;
		}
		MetadataHandle handle;
		handle.pointer.block_index = pointer.block_index;
		handle.pointer.index = pointer.index;
		handle.handle = std::move(buffer_handle);
		return handle;
	}
}

void MetadataManager::ConvertToTransient(unique_lock<mutex> &block_lock, MetadataBlock &metadata_block) {
	D_ASSERT(block_lock.owns_lock());
	auto old_block = metadata_block.block;
	block_lock.unlock();
	// pin the old block
	auto old_buffer = buffer_manager.Pin(old_block);

	// allocate a new transient block to replace it
	auto new_buffer = buffer_manager.Allocate(MemoryTag::METADATA, &block_manager, false);
	auto new_block = new_buffer.GetBlockHandle();

	// copy the data to the transient block
	memcpy(new_buffer.GetDataMutable(), old_buffer.Ptr(), block_manager.GetBlockSize());

	// the disk block now belongs to the transient block and is rewritten in place by a later
	// checkpoint - the old handle must not reload it from disk after its buffer is evicted
	{
		auto old_block_lock = old_block->GetMemory().GetLock();
		old_block->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::EVICTION);
	}

	// unregister the old block
	block_manager.UnregisterBlock(metadata_block.block_id);

	block_lock.lock();
	metadata_block.block = std::move(new_block);
	metadata_block.dirty = true;
}

block_id_t MetadataManager::AllocateNewBlock(unique_lock<mutex> &block_lock) {
	D_ASSERT(!block_lock.owns_lock());
	auto new_block_id = GetNextBlockId();

	MetadataBlock new_block;
	auto handle = buffer_manager.Allocate(MemoryTag::METADATA, &block_manager, false);
	new_block.block = handle.GetBlockHandle();
	new_block.block_id = new_block_id;
	for (idx_t i = 0; i < METADATA_BLOCK_COUNT; i++) {
		new_block.free_blocks.push_back(NumericCast<uint8_t>(METADATA_BLOCK_COUNT - i - 1));
	}
	new_block.dirty = true;
	// zero-initialize the handle
	memset(handle.GetDataMutable(), 0, block_manager.GetBlockSize());

	block_lock.lock();
	AddBlock(block_lock, std::move(new_block));
	return new_block_id;
}

void MetadataManager::AddBlock(unique_lock<mutex> &block_lock, MetadataBlock new_block, bool if_exists) {
	D_ASSERT(block_lock.owns_lock());
	if (blocks.find(new_block.block_id) != blocks.end()) {
		if (if_exists) {
			return;
		}
		throw InternalException("Block id with id %llu already exists", new_block.block_id);
	}
	blocks[new_block.block_id] = std::move(new_block);
}

void MetadataManager::AddAndRegisterBlock(unique_lock<mutex> &block_lock, MetadataBlock block) {
	if (block.block) {
		throw InternalException("Calling AddAndRegisterBlock on block that already exists");
	}
	if (block.block_id >= MAXIMUM_BLOCK) {
		throw InternalException("AddAndRegisterBlock called with a transient block id");
	}
	block_lock.unlock();
	block.block = block_manager.RegisterBlock(block.block_id, MemoryTag::METADATA);
	block_lock.lock();
	AddBlock(block_lock, std::move(block), true);
}

MetaBlockPointer MetadataManager::GetDiskPointer(const MetadataPointer &pointer, uint32_t offset) {
	idx_t block_pointer = idx_t(pointer.block_index);
	block_pointer |= idx_t(pointer.index) << 56ULL;
	return MetaBlockPointer(block_pointer, offset);
}

block_id_t MetaBlockPointer::GetBlockId() const {
	return block_id_t(block_pointer & ~(idx_t(0xFF) << 56ULL));
}

uint32_t MetaBlockPointer::GetBlockIndex() const {
	return block_pointer >> 56ULL;
}

MetadataPointer MetadataManager::FromDiskPointer(MetaBlockPointer pointer) {
	unique_lock<mutex> guard(block_lock);
	return FromDiskPointerInternal(guard, pointer);
}

MetadataPointer MetadataManager::FromDiskPointerInternal(unique_lock<mutex> &block_lock, MetaBlockPointer pointer) {
	auto block_id = pointer.GetBlockId();
	auto index = pointer.GetBlockIndex();
	if (index >= METADATA_BLOCK_COUNT) {
		throw IOException("Metadata block index %llu exceeds metadata block count %llu", index, METADATA_BLOCK_COUNT);
	}

	auto entry = blocks.find(block_id);
	if (entry == blocks.end()) { // LCOV_EXCL_START
		throw InternalException("Failed to load metadata pointer (id %llu, idx %llu, ptr %llu)\n", block_id, index,
		                        pointer.block_pointer);
	} // LCOV_EXCL_STOP
	MetadataPointer result;
	result.block_index = UnsafeNumericCast<idx_t>(block_id);
	result.index = UnsafeNumericCast<uint8_t>(index);
	return result;
}

MetadataPointer MetadataManager::RegisterDiskPointer(MetaBlockPointer pointer) {
	unique_lock<mutex> guard(block_lock);

	auto block_id = pointer.GetBlockId();
	MetadataBlock block;
	block.block_id = block_id;
	AddAndRegisterBlock(guard, std::move(block));
	return FromDiskPointerInternal(guard, pointer);
}

BlockPointer MetadataManager::ToBlockPointer(MetaBlockPointer meta_pointer, const idx_t metadata_block_size) {
	auto index = meta_pointer.GetBlockIndex();
	if (index >= MetadataManager::METADATA_BLOCK_COUNT) {
		throw IOException("Metadata block index %llu exceeds metadata block count %llu", index,
		                  MetadataManager::METADATA_BLOCK_COUNT);
	}
	BlockPointer result;
	result.block_id = meta_pointer.GetBlockId();
	result.offset = index * NumericCast<uint32_t>(metadata_block_size) + meta_pointer.offset;
	D_ASSERT(result.offset < metadata_block_size * MetadataManager::METADATA_BLOCK_COUNT);
	return result;
}

MetaBlockPointer MetadataManager::FromBlockPointer(BlockPointer block_pointer, const idx_t metadata_block_size) {
	if (!block_pointer.IsValid()) {
		return MetaBlockPointer();
	}
	idx_t index = block_pointer.offset / metadata_block_size;
	auto offset = block_pointer.offset % metadata_block_size;
	if (index >= MetadataManager::METADATA_BLOCK_COUNT) {
		throw IOException("Metadata block offset %llu exceeds metadata block capacity %llu", block_pointer.offset,
		                  metadata_block_size * MetadataManager::METADATA_BLOCK_COUNT);
	}
	D_ASSERT(offset < metadata_block_size);
	MetaBlockPointer result;
	result.block_pointer = idx_t(block_pointer.block_id) | index << 56ULL;
	result.offset = UnsafeNumericCast<uint32_t>(offset);
	return result;
}

idx_t MetadataManager::BlockCount() {
	return blocks.size();
}

void MetadataManager::Flush(QueryContext context) {
	// Write the blocks of the metadata manager to disk.
	const idx_t total_metadata_size = GetMetadataBlockSize() * METADATA_BLOCK_COUNT;

	unique_lock<mutex> guard(block_lock, std::defer_lock);
	for (auto &kv : blocks) {
		auto &block = kv.second;
		if (!block.dirty) {
			if (block.block->BlockId() >= MAXIMUM_BLOCK) {
				throw InternalException("Transient blocks must always be marked as dirty");
			}
			continue;
		}
		auto block_handle = block.block;
		auto handle = buffer_manager.Pin(block_handle);
		// zero-initialize the few leftover bytes
		memset(handle.GetDataMutable() + total_metadata_size, 0, block_manager.GetBlockSize() - total_metadata_size);
		D_ASSERT(kv.first == block.block_id);
		if (block_handle->BlockId() >= MAXIMUM_BLOCK) {
			// Convert the temporary block to a persistent block.
			// we cannot use ConvertToPersistent as another thread might still be reading the block
			// so we use the safe version of ConvertToPersistent
			auto new_block = block_manager.ConvertToPersistent(context, kv.first, std::move(block_handle),
			                                                   std::move(handle), ConvertToPersistentMode::THREAD_SAFE);

			guard.lock();
			block.block = std::move(new_block);
			guard.unlock();
		} else {
			// Already a persistent block, so we only need to write it.
			D_ASSERT(block.block->BlockId() == block.block_id);
			block_manager.Write(context, handle.GetFileBuffer(), block.block_id);
		}
		// the block is no longer dirty
		block.dirty = false;
	}
}

void MetadataManager::Write(WriteStream &sink) {
	sink.Write<uint64_t>(blocks.size());
	for (auto &kv : blocks) {
		kv.second.Write(sink);
	}
}

void MetadataManager::Read(ReadStream &source) {
	auto block_count = source.Read<uint64_t>();
	for (idx_t i = 0; i < block_count; i++) {
		auto block = MetadataBlock::Read(source);

		unique_lock<mutex> guard(block_lock);
		auto entry = blocks.find(block.block_id);
		if (entry == blocks.end()) {
			// block does not exist yet
			AddAndRegisterBlock(guard, std::move(block));
		} else {
			// block was already created - only copy over the free list
			entry->second.free_blocks = std::move(block.free_blocks);
		}
	}
}

void MetadataBlock::Write(WriteStream &sink) {
	sink.Write<block_id_t>(block_id);
	sink.Write<idx_t>(FreeBlocksToInteger());
}

idx_t MetadataManager::GetMetadataBlockSize() const {
	return AlignValueFloor(block_manager.GetBlockSize() / METADATA_BLOCK_COUNT);
}

MetadataBlock MetadataBlock::Read(ReadStream &source) {
	MetadataBlock result;
	result.block_id = source.Read<block_id_t>();
	auto free_list = source.Read<idx_t>();
	result.FreeBlocksFromInteger(free_list);
	return result;
}

idx_t MetadataBlock::FreeBlocksToInteger() {
	idx_t result = 0;
	for (idx_t i = 0; i < free_blocks.size(); i++) {
		D_ASSERT(free_blocks[i] < idx_t(64));
		idx_t mask = idx_t(1) << idx_t(free_blocks[i]);
		result |= mask;
	}
	return result;
}

vector<uint8_t> MetadataBlock::BlocksFromInteger(idx_t free_list) {
	vector<uint8_t> blocks;
	for (idx_t i = 64; i > 0; i--) {
		auto index = i - 1;
		idx_t mask = idx_t(1) << index;
		if (free_list & mask) {
			blocks.push_back(UnsafeNumericCast<uint8_t>(index));
		}
	}
	return blocks;
}

void MetadataBlock::FreeBlocksFromInteger(idx_t free_list) {
	free_blocks.clear();
	if (free_list == 0) {
		return;
	}
	free_blocks = BlocksFromInteger(free_list);
}

void MetadataManager::MarkBlocksAsModified() {
	unique_lock<mutex> guard(block_lock);
	// for any blocks that were modified in the last checkpoint - set them to free blocks currently
	for (auto &kv : modified_blocks) {
		auto block_id = kv.first;
		idx_t modified_list = kv.second;
		auto entry = blocks.find(block_id);
		D_ASSERT(entry != blocks.end());
		auto &block = entry->second;
		idx_t current_free_blocks = block.FreeBlocksToInteger();
		// merge the current set of free blocks with the modified blocks
		idx_t new_free_blocks = current_free_blocks | modified_list;
		if (new_free_blocks == NumericLimits<idx_t>::Maximum()) {
			// if new free_blocks is all blocks - mark entire block as modified
			blocks.erase(entry);

			guard.unlock();
			block_manager.MarkBlockAsModified(block_id);
			guard.lock();
		} else {
			// set the new set of free blocks
			block.FreeBlocksFromInteger(new_free_blocks);
		}
	}

	modified_blocks.clear();

	for (auto &kv : blocks) {
		auto &block = kv.second;
		idx_t free_list = block.FreeBlocksToInteger();
		idx_t occupied_list = ~free_list;
		modified_blocks[block.block_id] = occupied_list;
	}
}

void MetadataManager::ClearModifiedBlocks(const vector<MetaBlockPointer> &pointers) {
	if (pointers.empty()) {
		return;
	}
	vector<MetaBlockPointer> next_pointers;
	if (keep_next_blocks) {
		// several pointers can be in one block, whose header is read once
		unordered_set<idx_t> kept_blocks;
		for (auto &pointer : pointers) {
			if (!kept_blocks.insert(pointer.block_pointer).second) {
				continue;
			}
			auto next_block = GetNextBlock(pointer);
			if (next_block.IsValid()) {
				next_pointers.push_back(next_block);
			}
		}
	}
	unique_lock<mutex> guard(block_lock);
	for (auto &pointer : pointers) {
		auto block_id = pointer.GetBlockId();
		auto block_index = pointer.GetBlockIndex();
		auto entry = modified_blocks.find(block_id);
		if (entry == modified_blocks.end()) {
			throw InternalException("ClearModifiedBlocks - Block id %llu not found in modified_blocks", block_id);
		}
		auto &modified_list = entry->second;
		// unset the bit
		modified_list &= ~(1ULL << block_index);
	}
	for (auto &pointer : next_pointers) {
		// the next block was in use before this checkpoint, otherwise it is left as it is
		auto entry = modified_blocks.find(pointer.GetBlockId());
		if (entry != modified_blocks.end()) {
			entry->second &= ~(1ULL << pointer.GetBlockIndex());
		}
	}
}

void MetadataManager::SetStorageVersion(StorageVersion storage_version) {
	keep_next_blocks = storage_version < StorageVersion::V1_5_0;
}

MetaBlockPointer MetadataManager::GetNextBlock(MetaBlockPointer pointer) {
	auto metadata_pointer = FromDiskPointer(pointer);
	auto handle = Pin(metadata_pointer);
	auto next_block = Load<idx_t>(handle.handle.Ptr() + metadata_pointer.index * GetMetadataBlockSize());
	return MetaBlockPointer(next_block, 0);
}

bool MetadataManager::BlockIsModified(const MetaBlockPointer &pointer) {
	unique_lock<mutex> guard(block_lock);
	auto block_id = pointer.GetBlockId();
	auto entry = blocks.find(block_id);
	if (entry == blocks.end()) {
		throw InternalException("BlockIsNotModified - Block id %llu not found in blocks", block_id);
	}
	auto entry2 = modified_blocks.find(block_id);
	return entry2 != modified_blocks.end();
}

bool MetadataManager::BlockHasBeenCleared(const MetaBlockPointer &pointer) {
	unique_lock<mutex> guard(block_lock);
	auto block_id = pointer.GetBlockId();
	auto block_index = pointer.GetBlockIndex();
	auto entry = modified_blocks.find(block_id);
	if (entry == modified_blocks.end()) {
		throw InternalException("BlockHasBeenCleared - Block id %llu not found in modified_blocks", block_id);
	}
	auto &modified_list = entry->second;
	return (modified_list & (1ULL << block_index)) == 0ULL;
}

vector<MetadataBlockInfo> MetadataManager::GetMetadataInfo() const {
	vector<MetadataBlockInfo> result;
	unique_lock<mutex> guard(block_lock);
	for (auto &block : blocks) {
		MetadataBlockInfo block_info;
		block_info.block_id = block.second.block_id;
		block_info.total_blocks = MetadataManager::METADATA_BLOCK_COUNT;
		for (auto free_block : block.second.free_blocks) {
			block_info.free_list.push_back(free_block);
		}
		std::sort(block_info.free_list.begin(), block_info.free_list.end());
		result.push_back(std::move(block_info));
	}
	std::sort(result.begin(), result.end(),
	          [](const MetadataBlockInfo &a, const MetadataBlockInfo &b) { return a.block_id < b.block_id; });
	return result;
}

vector<shared_ptr<BlockHandle>> MetadataManager::GetBlocks() const {
	vector<shared_ptr<BlockHandle>> result;
	unique_lock<mutex> guard(block_lock);
	for (auto &entry : blocks) {
		result.push_back(entry.second.block);
	}
	return result;
}

block_id_t MetadataManager::PeekNextBlockId() const {
	return block_manager.PeekFreeBlockId();
}

block_id_t MetadataManager::GetNextBlockId() const {
	return block_manager.GetFreeBlockIdForCheckpoint();
}

} // namespace duckdb
