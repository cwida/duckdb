//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/string_checkpoint_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {
struct UncompressedStringSegmentState;

class OverflowStringWriter {
public:
	virtual ~OverflowStringWriter() {
	}

	virtual void WriteString(UncompressedStringSegmentState &state, string_t string, block_id_t &result_block,
	                         int32_t &result_offset) = 0;
	virtual void Flush() = 0;
};

struct StringBlock {
	shared_ptr<BlockHandle> block;
	idx_t offset;
	idx_t size;
	unique_ptr<StringBlock> next;
};

struct UncompressedStringSegmentState : public CompressedSegmentState {
	~UncompressedStringSegmentState() override;

	//! The string block holding strings that do not fit in the main block
	//! FIXME: this should be replaced by a heap that also allows freeing of unused strings
	unique_ptr<StringBlock> head;
	//! Map of block id to string block
	unordered_map<block_id_t, reference<StringBlock>> overflow_blocks;
	//! Overflow string writer (if any), if not set overflow strings will be written to memory blocks
	unique_ptr<OverflowStringWriter> overflow_writer;
	//! The block manager with which to write
	optional_ptr<BlockManager> block_manager;

public:
	void InitializeOnDiskBlocks(vector<block_id_t> blocks);

	const vector<block_id_t> &GetOnDiskBlocks() const {
		D_ASSERT(on_disk_blocks.size() == on_disk_block_set.size());
		return on_disk_blocks;
	}

	shared_ptr<BlockHandle> GetHandle(BlockManager &manager, block_id_t block_id);

	void RegisterBlock(BlockManager &manager, block_id_t block_id);

	string GetSegmentInfo() const override;

	void InsertOverflowBlock(block_id_t block_id, reference<StringBlock> block);
	reference<StringBlock> FindOverflowBlock(block_id_t block_id);

private:
	//! Overflow block IDs in write order, preserved for serialization.
	vector<block_id_t> on_disk_blocks;
	//! Membership index for on_disk_blocks.
	unordered_set<block_id_t> on_disk_block_set;

	mutex block_lock;
	unordered_map<block_id_t, shared_ptr<BlockHandle>> handles;

	StorageLock overflow_blocks_lock;
};

} // namespace duckdb
