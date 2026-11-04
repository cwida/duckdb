#include "duckdb/storage/checkpoint/write_overflow_strings_to_disk.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/partial_block_manager.hpp"

namespace duckdb {

WriteOverflowStringsToDisk::WriteOverflowStringsToDisk(PartialBlockManager &partial_block_manager)
    : partial_block_manager(partial_block_manager), block_id(INVALID_BLOCK), offset(0) {
}

WriteOverflowStringsToDisk::~WriteOverflowStringsToDisk() {
	// verify that the overflow writer has been flushed
	D_ASSERT(Exception::UncaughtException() || offset == 0);
}

void UncompressedStringSegmentState::InitializeOnDiskBlocks(vector<block_id_t> blocks) {
	D_ASSERT(on_disk_blocks.empty());
	D_ASSERT(on_disk_block_set.empty());
	D_ASSERT(handles.empty());
	on_disk_block_set.reserve(blocks.size());
	on_disk_block_set.insert(blocks.begin(), blocks.end());
	if (on_disk_block_set.size() != blocks.size()) {
		throw DataCorruptionException("Corrupted string segment: duplicate overflow block IDs");
	}
	on_disk_blocks = std::move(blocks);
}

shared_ptr<BlockHandle> UncompressedStringSegmentState::GetHandle(BlockManager &manager_p, block_id_t block_id) {
	lock_guard<mutex> lock(block_lock);
	auto entry = handles.find(block_id);
	if (entry != handles.end()) {
		return entry->second;
	}
	auto &manager = block_manager ? *block_manager : manager_p;
	if (on_disk_block_set.find(block_id) == on_disk_block_set.end()) {
		throw DataCorruptionException(
		    "Corrupted uncompressed string segment: overflow string block is not owned by the segment");
	}
	auto result = manager.RegisterBlock(block_id, MemoryTag::OVERFLOW_STRINGS);
	handles.insert(make_pair(block_id, result));
	return result;
}

void UncompressedStringSegmentState::RegisterBlock(BlockManager &manager_p, block_id_t block_id) {
	lock_guard<mutex> lock(block_lock);
	auto entry = handles.find(block_id);
	if (entry != handles.end()) {
		throw InternalException("UncompressedStringSegmentState::RegisterBlock - block id %llu already exists",
		                        block_id);
	}
	auto &manager = block_manager ? *block_manager : manager_p;
	auto result = manager.RegisterBlock(block_id, MemoryTag::OVERFLOW_STRINGS);
	handles.insert(make_pair(block_id, std::move(result)));
	on_disk_blocks.push_back(block_id);
	on_disk_block_set.insert(block_id);
}

string UncompressedStringSegmentState::GetSegmentInfo() const {
	if (on_disk_blocks.empty()) {
		return "";
	}
	string result = StringUtil::Join(on_disk_blocks, on_disk_blocks.size(), ", ",
	                                 [&](block_id_t block) { return to_string(block); });
	return "Overflow String Block Ids: " + result;
}

void UncompressedStringSegmentState::InsertOverflowBlock(block_id_t block_id, reference<StringBlock> block) {
	auto write_lock = overflow_blocks_lock.GetExclusiveLock();
	overflow_blocks.insert(make_pair(block_id, block));
}

reference<StringBlock> UncompressedStringSegmentState::FindOverflowBlock(block_id_t block_id) {
	auto read_lock = overflow_blocks_lock.GetSharedLock();
	auto entry = overflow_blocks.find(block_id);
	if (entry == overflow_blocks.end()) {
		throw DataCorruptionException(
		    "Corrupted uncompressed string segment: overflow string block is not owned by the segment");
	}
	return entry->second;
}

void WriteOverflowStringsToDisk::WriteString(UncompressedStringSegmentState &state, string_t string,
                                             block_id_t &result_block, int32_t &result_offset) {
	auto &block_manager = partial_block_manager.GetBlockManager();
	auto &buffer_manager = block_manager.buffer_manager;
	if (!handle.IsValid()) {
		handle = buffer_manager.Allocate(MemoryTag::OVERFLOW_STRINGS, &block_manager);
	}
	// first write the length of the string
	if (block_id == INVALID_BLOCK || offset + 2 * sizeof(uint32_t) >= GetStringSpace()) {
		AllocateNewBlock(state, partial_block_manager.GetFreeBlockId());
	}
	result_block = block_id;
	result_offset = UnsafeNumericCast<int32_t>(offset);

	// write the length field
	auto data_ptr = handle.GetDataMutable();
	auto string_length = string.GetSize();
	Store<uint32_t>(UnsafeNumericCast<uint32_t>(string_length), data_ptr + offset);
	offset += sizeof(uint32_t);

	// now write the remainder of the string
	auto strptr = string.GetData();
	auto remaining = UnsafeNumericCast<uint32_t>(string_length);
	while (remaining > 0) {
		uint32_t to_write = MinValue<uint32_t>(remaining, UnsafeNumericCast<uint32_t>(GetStringSpace() - offset));
		if (to_write > 0) {
			memcpy(data_ptr + offset, strptr, to_write);

			remaining -= to_write;
			offset += to_write;
			strptr += to_write;
		}
		if (remaining > 0) {
			D_ASSERT(offset == GetStringSpace());
			// there is still remaining stuff to write
			// now write the current block to disk and allocate a new block
			AllocateNewBlock(state, partial_block_manager.GetFreeBlockId());
		}
	}
}

void WriteOverflowStringsToDisk::Flush() {
	if (block_id != INVALID_BLOCK && offset > 0) {
		// zero-initialize the empty part of the overflow string buffer (if any)
		if (offset < GetStringSpace()) {
			memset(handle.GetDataMutable() + offset, 0, GetStringSpace() - offset);
		}
		// write to disk
		auto &block_manager = partial_block_manager.GetBlockManager();
		block_manager.Write(partial_block_manager.GetClientContext(), handle.GetFileBuffer(), block_id);
	}
	block_id = INVALID_BLOCK;
	offset = 0;
}

void WriteOverflowStringsToDisk::AllocateNewBlock(UncompressedStringSegmentState &state, block_id_t new_block_id) {
	if (block_id != INVALID_BLOCK) {
		// there is an old block, write it first
		// write the new block id at the end of the previous block
		Store<block_id_t>(new_block_id, handle.GetDataMutable() + GetStringSpace());
		Flush();
	}
	offset = 0;
	block_id = new_block_id;
	auto &block_manager = partial_block_manager.GetBlockManager();
	state.RegisterBlock(block_manager, new_block_id);
}

idx_t WriteOverflowStringsToDisk::GetStringSpace() const {
	auto &block_manager = partial_block_manager.GetBlockManager();
	return block_manager.GetBlockSize() - sizeof(block_id_t);
}

} // namespace duckdb
