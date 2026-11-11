//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/write_overflow_strings_to_disk.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/checkpoint/string_checkpoint_state.hpp"

namespace duckdb {

class PartialBlockManager;

class WriteOverflowStringsToDisk : public OverflowStringWriter {
public:
	explicit WriteOverflowStringsToDisk(PartialBlockManager &partial_block_manager);
	~WriteOverflowStringsToDisk() override;

	//! The block manager
	PartialBlockManager &partial_block_manager;

	//! Temporary buffer
	BufferHandle handle;
	//! The block on-disk to which we are writing
	block_id_t block_id;
	//! The offset within the current block
	idx_t offset;

public:
	void WriteString(UncompressedStringSegmentState &state, string_t string, block_id_t &result_block,
	                 int32_t &result_offset) override;
	void Flush() override;

private:
	void AllocateNewBlock(UncompressedStringSegmentState &state, block_id_t new_block_id);
	idx_t GetStringSpace() const;
};

} // namespace duckdb
