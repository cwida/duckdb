#include "duckdb/storage/partial_block_manager.hpp"

#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/block_packable.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// PartialBlock
//===--------------------------------------------------------------------===//

struct PartialBlock {
	PartialBlock(BlockManager &block_manager, block_id_t block_id);

public:
	//! How much space is left in the current block for further packing.
	idx_t AvailableSpace() const;
	//! How many packables are residing in the current block.
	idx_t UseCount() const;
	//! Consume the source pin, adopting its buffer when eligible or copying otherwise.
	BlockPointer Store(BlockPackable &packable, BufferHandle &&source, uint32_t byte_size);
	//! Borrow the source pin and copy its bytes into the packing buffer.
	BlockPointer Store(BlockPackable &packable, const BufferHandle &source, uint32_t byte_size);
	//! Flush the block to disk and zero-initialize any tail free space.
	void Flush(QueryContext context, PartialBlockType type);
	//! Merge packables before either block has a persistent block ID.
	void Merge(PartialBlock &other);
	//! Remove all packables and the associated backing block.
	void Clear();

private:
	//! Whether the source can serve as the packing buffer without an allocation or copy.
	bool CanReuseBuffer(const BufferHandle &source);
	//! Zero alignment padding and register an allocation whose bytes are already in the destination.
	BlockPointer FinishStore(BlockPackable &packable, BufferHandle destination, uint32_t byte_size);

private:
	BlockManager &block_manager;
	//! The temporary packing buffer, replaced by a persistent handle when flushed to disk.
	shared_ptr<BlockHandle> block_handle;
	vector<std::pair<reference<BlockPackable>, uint32_t>> packables;
	//! The reserved database block id, or INVALID_BLOCK.
	block_id_t block_id;
	//! Next allocation offset, and also the current allocation size.
	uint32_t offset = 0;
};

PartialBlock::PartialBlock(BlockManager &block_manager, block_id_t block_id)
    : block_manager(block_manager), block_handle(nullptr), block_id(block_id) {
}

idx_t PartialBlock::AvailableSpace() const {
	return block_manager.GetBlockSize() - offset;
}

idx_t PartialBlock::UseCount() const {
	return packables.size();
}

bool PartialBlock::CanReuseBuffer(const BufferHandle &source) {
	const auto &source_handle = source.GetBlockHandle();
	if (source_handle->BlockId() < MAXIMUM_BLOCK) {
		// if the buffer has a backing disk block, we might overwrite other segments in the block when growing the
		// buffer for the partial block manager.
		return false;
	}

	auto &source_buffer = source.GetFileBuffer();
	if (source_buffer.GetBufferType() != FileBufferType::MANAGED_BUFFER) {
		// only ordinary managed storage for now
		return false;
	}
	if (!source_buffer.OwnsInternalBuffer()) {
		// memory mapped buffer is not supported
		return false;
	}
	if (source_buffer.AllocSize() != block_manager.GetBlockAllocSize()) {
		// allocated space should be equal to the block size so we never write outside the allocated region when writing
		// other packables
		return false;
	}
	if (source_buffer.GetHeaderSize() != block_manager.GetBlockHeaderSize()) {
		// prevent misinterpretation of payload bytes as header bytes and vice versa
		return false;
	}

	auto &source_memory = source.GetBlockHandle()->GetMemory();
	if (!source_memory.MustWriteToTemporaryFile()) {
		// under memory pressure we should spill in flight partial blocks, otherwise data may be lost when we want to
		// flush buffer to the backing disk blocks.
		return false;
	}
	if (&source_memory.GetBufferManager() != &block_manager.buffer_manager) {
		// moving buffers across managed memory pools might lead to accounting inconsistencies
		return false;
	}

	return true;
}

BlockPointer PartialBlock::Store(BlockPackable &packable, BufferHandle &&source, uint32_t byte_size) {
	BufferHandle owned_source(std::move(source));
	D_ASSERT(owned_source.IsValid());
	D_ASSERT(byte_size > 0);
	D_ASSERT(byte_size <= owned_source.GetFileBuffer().Size());

	if (!block_handle && CanReuseBuffer(owned_source)) {
		D_ASSERT(offset == 0);
		block_handle = owned_source.GetBlockHandle();
		return FinishStore(packable, std::move(owned_source), byte_size);
	}
	return Store(packable, owned_source, byte_size);
}

BlockPointer PartialBlock::Store(BlockPackable &packable, const BufferHandle &source, uint32_t byte_size) {
	D_ASSERT(source.IsValid());
	D_ASSERT(byte_size > 0);
	D_ASSERT(byte_size <= source.GetFileBuffer().Size());
	D_ASSERT(block_handle || offset == 0);
	D_ASSERT(ValueIsAligned(offset));
	D_ASSERT(AlignValue(idx_t(offset) + byte_size) <= block_manager.GetBlockSize());

	auto &buffer_manager = block_manager.buffer_manager;
	BufferHandle dst;
	if (block_handle) {
		D_ASSERT(source.GetBlockHandle() != block_handle);
		dst = buffer_manager.Pin(block_handle);
	} else {
		auto &source_memory = source.GetBlockHandle()->GetMemory();
		dst = buffer_manager.Allocate(source_memory.GetMemoryTag(), &block_manager, false);
		block_handle = dst.GetBlockHandle();
	}

	memcpy(dst.GetDataMutable() + offset, source.Ptr(), byte_size);
	packable.UsePackedCopy(block_handle, offset);
	return FinishStore(packable, std::move(dst), byte_size);
}

BlockPointer PartialBlock::FinishStore(BlockPackable &packable, BufferHandle destination, uint32_t byte_size) {
	D_ASSERT(destination.IsValid());
	D_ASSERT(destination.GetBlockHandle() == block_handle);
	D_ASSERT(destination.GetFileBuffer().Size() == block_manager.GetBlockSize());
	D_ASSERT(byte_size > 0);
	D_ASSERT(ValueIsAligned(offset));

	const auto unaligned_size = idx_t(offset) + byte_size;
	const auto aligned_size = AlignValue(unaligned_size);
	D_ASSERT(aligned_size <= block_manager.GetBlockSize());

	const BlockPointer block_ptr(block_id, offset);
	if (unaligned_size != aligned_size) {
		memset(destination.GetDataMutable() + unaligned_size, 0, aligned_size - unaligned_size);
	}

	packables.emplace_back(packable, offset);
	offset = NumericCast<uint32_t>(aligned_size);

	if (block_id != INVALID_BLOCK && UseCount() > 1) {
		block_manager.IncreaseBlockReferenceCount(block_id);
	}

	return block_ptr;
}

void PartialBlock::Flush(QueryContext context, PartialBlockType type) {
	if (!block_handle) {
		throw InternalException("Flush called on partial block that was already flushed");
	}

	if (type != PartialBlockType::IN_MEMORY_CHECKPOINT) {
		auto buffer_handle = block_manager.buffer_manager.Pin(block_handle);
		auto free_space = AvailableSpace();
		if (free_space > 0) {
			// zero any tail bytes to prevent leaking buffer data to disk
			memset(buffer_handle.GetDataMutable() + block_manager.GetBlockSize() - free_space, 0, free_space);
		}

		bool fetch_new_block = block_id == INVALID_BLOCK;
		if (fetch_new_block) {
			block_id = block_manager.GetFreeBlockId();
		}
		block_handle =
		    block_manager.ConvertToPersistent(context, block_id, std::move(block_handle), std::move(buffer_handle));

		if (fetch_new_block) {
			for (idx_t i = 1; i < UseCount(); i++) {
				block_manager.IncreaseBlockReferenceCount(block_id);
			}
		}
	}

	for (auto &[packable, offset] : packables) {
		packable.get().Finalize(block_handle, offset);
	}

	Clear();
}

void PartialBlock::Merge(PartialBlock &other) {
	D_ASSERT(&other != this);
	D_ASSERT(block_handle && other.block_handle);
	D_ASSERT(block_handle != other.block_handle);
	D_ASSERT(other.offset <= AvailableSpace());

	if (&block_manager != &other.block_manager) {
		throw InternalException("Cannot merge partial blocks with different block managers");
	}
	if (block_id != INVALID_BLOCK || other.block_id != INVALID_BLOCK) {
		throw InternalException("Cannot merge partial blocks with assigned block IDs");
	}

	auto &buffer_manager = block_manager.buffer_manager;
	auto old_handle = buffer_manager.Pin(other.block_handle);
	auto new_handle = buffer_manager.Pin(block_handle);
	memcpy(new_handle.GetDataMutable() + offset, old_handle.Ptr(), other.offset);

	for (auto &[packable, other_offset] : other.packables) {
		packables.emplace_back(packable, NumericCast<uint32_t>(other_offset + offset));
	}

	offset += other.offset;

	other.Clear();
}

void PartialBlock::Clear() {
	block_handle.reset();
	packables.clear();
}

//===--------------------------------------------------------------------===//
// PartialBlockManager
//===--------------------------------------------------------------------===//

PartialBlockManager::PartialBlockManager(QueryContext context, BlockManager &block_manager,
                                         PartialBlockType partial_block_type, optional_idx max_partial_block_size_p,
                                         uint32_t max_use_count)
    : context(context.GetClientContext()), block_manager(block_manager), partial_block_type(partial_block_type),
      max_use_count(max_use_count) {
	if (max_partial_block_size_p.IsValid()) {
		max_partial_block_size = NumericCast<uint32_t>(max_partial_block_size_p.GetIndex());
	} else {
		// Use the default maximum partial block size with a ratio of 20% free and 80% utilization.
		max_partial_block_size = NumericCast<uint32_t>(block_manager.GetBlockSize() / 5 * 4);
	}
	D_ASSERT(max_partial_block_size <= block_manager.GetBlockSize());
}

PartialBlockManager::~PartialBlockManager() = default;

block_id_t PartialBlockManager::GetFreeBlockId() {
	if (partial_block_type == PartialBlockType::FULL_CHECKPOINT) {
		return block_manager.GetFreeBlockIdForCheckpoint();
	} else {
		return block_manager.GetFreeBlockId();
	}
}

void PartialBlockManager::Merge(PartialBlockManager &other) {
	if (&other == this) {
		throw InternalException("Cannot merge into itself");
	}
	if (&block_manager != &other.block_manager || partial_block_type != other.partial_block_type) {
		throw InternalException("Cannot merge partial block managers with different block managers or block types");
	}

	for (auto &[other_offset, other_block] : other.partially_filled_blocks) {
		if (!other_block) {
			throw InternalException("Empty partially filled block found");
		}

		auto used_space = NumericCast<uint32_t>(block_manager.GetBlockSize() - other_offset);
		if (used_space > max_partial_block_size) {
			RetainOrFlush(std::move(other_block));
			continue;
		}
		if (auto partial_block = TakePartialBlock(used_space)) {
			partial_block->Merge(*other_block);
			RetainOrFlush(std::move(partial_block));
			continue;
		}

		RetainOrFlush(std::move(other_block));
	}

	other.partially_filled_blocks.clear();
}

void PartialBlockManager::ClearBlocks() {
	partially_filled_blocks.clear();
}

void PartialBlockManager::FlushPartialBlocks() {
	for (auto &e : partially_filled_blocks) {
		e.second->Flush(context, partial_block_type);
	}
	partially_filled_blocks.clear();
}

unique_ptr<PartialBlock> PartialBlockManager::TakePartialBlock(uint32_t byte_size) {
	auto entry = partially_filled_blocks.lower_bound(byte_size);
	if (entry == partially_filled_blocks.end()) {
		// no available partial blocks have enough space available to fit the requested size
		return nullptr;
	}

	auto partial_block = std::move(entry->second);
	partially_filled_blocks.erase(entry);

	D_ASSERT(partial_block->AvailableSpace() < block_manager.GetBlockSize());
	D_ASSERT(ValueIsAligned(block_manager.GetBlockSize() - partial_block->AvailableSpace()));

	return partial_block;
}

unique_ptr<PartialBlock> PartialBlockManager::CreatePartialBlock() {
	block_id_t block_id;
	if (partial_block_type == PartialBlockType::FULL_CHECKPOINT) {
		block_id = GetFreeBlockId();
	} else {
		block_id = INVALID_BLOCK;
	}

	return make_uniq<PartialBlock>(block_manager, block_id);
}

unique_ptr<PartialBlock> PartialBlockManager::AcquirePartialBlock(uint32_t byte_size) {
	D_ASSERT(byte_size <= block_manager.GetBlockSize());
	unique_ptr<PartialBlock> partial_block;

	if (byte_size <= max_partial_block_size) {
		partial_block = TakePartialBlock(byte_size);
	}
	if (!partial_block) {
		partial_block = CreatePartialBlock();
	}

	return partial_block;
}

unique_ptr<PartialBlock> PartialBlockManager::PruneFreeList() {
	if (partially_filled_blocks.size() <= MAX_BLOCK_MAP_SIZE) {
		return nullptr;
	}

	auto entry = partially_filled_blocks.begin();
	auto block = std::move(entry->second);
	partially_filled_blocks.erase(entry);

	return block;
}

void PartialBlockManager::RetainOrFlush(unique_ptr<PartialBlock> partial_block) {
	auto min_reusable_space = block_manager.GetBlockSize() - max_partial_block_size;
	auto available_space = partial_block->AvailableSpace();

	if (partial_block->UseCount() >= max_use_count) {
		partial_block->Flush(context, partial_block_type);
		return;
	}
	if (available_space < min_reusable_space) {
		partial_block->Flush(context, partial_block_type);
		return;
	}

	partially_filled_blocks.insert(make_pair(partial_block->AvailableSpace(), std::move(partial_block)));

	if (auto pruned_block = PruneFreeList()) {
		pruned_block->Flush(context, partial_block_type);
	}
}

optional<BlockPointer> PartialBlockManager::Write(BlockPackable &packable, BufferHandle &&source, uint32_t byte_size) {
	BufferHandle owned_source(std::move(source));
	unique_lock<mutex> lock(partial_block_lock);

	auto block = AcquirePartialBlock(byte_size);
	BlockPointer block_ptr = block->Store(packable, std::move(owned_source), byte_size);
	RetainOrFlush(std::move(block));

	if (partial_block_type == PartialBlockType::FULL_CHECKPOINT) {
		D_ASSERT(block_ptr.IsValid());
		return block_ptr;
	}

	return nullopt;
}

optional<BlockPointer> PartialBlockManager::Write(BlockPackable &packable, const BufferHandle &source,
                                                  uint32_t byte_size) {
	unique_lock<mutex> lock(partial_block_lock);

	auto block = AcquirePartialBlock(byte_size);
	BlockPointer block_ptr = block->Store(packable, source, byte_size);
	RetainOrFlush(std::move(block));

	if (partial_block_type == PartialBlockType::FULL_CHECKPOINT) {
		D_ASSERT(block_ptr.IsValid());
		return block_ptr;
	}

	return nullopt;
}

BlockManager &PartialBlockManager::GetBlockManager() const {
	return block_manager;
}

optional_ptr<ClientContext> PartialBlockManager::GetClientContext() const {
	return context;
}

} // namespace duckdb
