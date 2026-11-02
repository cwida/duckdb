//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/partial_block_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/storage/block.hpp"
#include <cstdint>

namespace duckdb {

class BlockManager;
class BlockPackable;
class BufferHandle;
struct PartialBlock;

enum class PartialBlockType { FULL_CHECKPOINT, APPEND_TO_TABLE, IN_MEMORY_CHECKPOINT };

//! Packs allocations sharing a block manager into blocks across a checkpoint or append scope.
class PartialBlockManager {
public:
	//! Default flush threshold for allocations sharing a block; effectively unlimited for normal block sizes.
	static constexpr const idx_t DEFAULT_MAX_USE_COUNT = 1u << 20;
	//! No point letting map size grow unbounded. We'll drop blocks with the least free space first.
	static constexpr const idx_t MAX_BLOCK_MAP_SIZE = 1u << 31;

public:
	//! Use the default packing threshold unless supplied; it must not exceed the block size.
	PartialBlockManager(QueryContext context, BlockManager &block_manager, PartialBlockType partial_block_type,
	                    optional_idx max_partial_block_size = optional_idx(),
	                    uint32_t max_use_count = DEFAULT_MAX_USE_COUNT);
	//! Discard pending blocks without flushing or finalizing their packables.
	~PartialBlockManager();

public:
	//! Discard pending blocks without writing or finalizing; already flushed blocks are unaffected.
	void ClearBlocks();

	//! Absorb other's pending blocks, leaving it empty on success; may flush and finalize packables.
	//! Managers must be distinct, exclusively accessed, and share the block manager and mode.
	//! Coalescing blocks requires unassigned persistent block IDs.
	void Merge(PartialBlockManager &other);

	//! Flush and finalize all pending blocks, leaving the map empty on success; in-memory mode skips disk IO.
	void FlushPartialBlocks();

	//! Consume a valid source pin and pack its first byte_size bytes; 0 < byte_size <= block size.
	//! Returns a fixed destination only in FULL_CHECKPOINT mode, otherwise nullopt; may flush before returning.
	//! The packable must outlive its pending allocation, until flushed or discarded.
	//! The caller must permit adoption and modification of the source's backing storage.
	optional<BlockPointer> Write(BlockPackable &packable, BufferHandle &&source, uint32_t byte_size);
	//! Borrow a valid source pin and copy its first byte_size bytes synchronously without adopting its storage.
	//! The source is neither modified nor retained; destination and packable lifetime rules are as above.
	optional<BlockPointer> Write(BlockPackable &packable, const BufferHandle &source, uint32_t byte_size);

	//! Reserve a disk block ID, using checkpoint accounting in FULL_CHECKPOINT mode; not for in-memory storage.
	block_id_t GetFreeBlockId();

	//! Returns a reference to the underlying block manager.
	BlockManager &GetBlockManager() const;
	//! Returns the optional client context.
	optional_ptr<ClientContext> GetClientContext() const;

private:
	//! Remove the smallest fitting block from the map, or return nullptr if none fits.
	unique_ptr<PartialBlock> TakePartialBlock(uint32_t byte_size);
	//! Create an empty block; reserve its persistent ID eagerly only in FULL_CHECKPOINT mode.
	unique_ptr<PartialBlock> CreatePartialBlock();
	//! Take a reusable block or create one; the result is not registered until retained or flushed.
	unique_ptr<PartialBlock> AcquirePartialBlock(uint32_t byte_size);
	//! Remove the block with least free space if the map limit is exceeded; otherwise return nullptr.
	unique_ptr<PartialBlock> PruneFreeList();
	//! Consume the block, retaining it if reusable or flushing it otherwise; also flush any pruned map entry.
	void RetainOrFlush(unique_ptr<PartialBlock> partial_block);

private:
	//! The optional client context in which we use the partial block manager.
	//! The SingleFileCheckpointWriter contains a partial block manager, and the destructor of
	//! AttachedDatabase invokes CreateCheckpoint with that checkpoint writer.
	//! Thus, we're using the optional client context instead of a reference.
	optional_ptr<ClientContext> context;
	BlockManager &block_manager;
	PartialBlockType partial_block_type;
	mutex partial_block_lock;
	//! Owns nonempty, unflushed blocks keyed by AvailableSpace(); equal keys are allowed.
	//! Remove a block before changing its used size so the key stays consistent.
	multimap<idx_t, unique_ptr<PartialBlock>> partially_filled_blocks;

	//! Largest allocation eligible for reuse; retained blocks have at least block_size - this much free space.
	uint32_t max_partial_block_size;
	//! Flush threshold for the number of packables; merging can cross this threshold.
	uint32_t max_use_count;
};

} // namespace duckdb
