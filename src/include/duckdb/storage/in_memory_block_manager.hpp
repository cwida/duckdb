//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/in_memory_block_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/block_manager.hpp"

namespace duckdb {

//! InMemoryBlockManager is an implementation for a BlockManager
class InMemoryBlockManager : public BlockManager {
public:
	using BlockManager::BlockManager;

	// LCOV_EXCL_START
	unique_ptr<Block> ConvertBlock(block_id_t block_id, FileBuffer &source_buffer) override {
		throw InternalException("Cannot perform IO in in-memory database - ConvertBlock!");
	}
	unique_ptr<Block> CreateBlock(block_id_t block_id, FileBuffer *source_buffer) override {
		throw InternalException("Cannot perform IO in in-memory database - CreateBlock!");
	}
	block_id_t GetFreeBlockId() override {
		throw InternalException("Cannot perform IO in in-memory database - GetFreeBlockId!");
	}
	block_id_t GetFreeBlockIdForCheckpoint() override {
		throw InternalException("Cannot perform IO in in-memory database - GetFreeBlockIdForCheckpoint!");
	}
	block_id_t PeekFreeBlockId() override {
		throw InternalException("Cannot perform IO in in-memory database - PeekFreeBlockId!");
	}
	bool IsRootBlock(MetaBlockPointer root) override {
		throw InternalException("Cannot perform IO in in-memory database - IsRootBlock!");
	}
	void MarkBlockAsCheckpointed(block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database - MarkBlockAsCheckpointed!");
	}
	void MarkBlockAsUsed(block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database - MarkBlockAsUsed!");
	}
	void MarkBlockAsModified(block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database - MarkBlockAsModified!");
	}
	void IncreaseBlockReferenceCount(block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database - IncreaseBlockReferenceCount!");
	}
	idx_t GetMetaBlock() override {
		throw InternalException("Cannot perform IO in in-memory database - GetMetaBlock!");
	}

	void Read(QueryContext context, Block &block) override {
		throw InternalException("Cannot perform IO in in-memory database - Read!");
	}
	void ReadBlocks(QueryContext context, FileBuffer &buffer, block_id_t start_block, idx_t block_count) override {
		throw InternalException("Cannot perform IO in in-memory database - ReadBlocks!");
	}
	void Write(FileBuffer &block, block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database - Write!");
	}
	void Write(QueryContext context, FileBuffer &block, block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database - Write with client context!");
	}
	void WriteHeader(QueryContext context, DatabaseHeader header, VisibilityBound bound) override {
		throw InternalException("Cannot perform IO in in-memory database - WriteHeader!");
	}
	bool InMemory() override {
		return true;
	}
	void FileSync() override {
		throw InternalException("Cannot perform IO in in-memory database - FileSync!");
	}
	idx_t TotalBlocks() override {
		throw InternalException("Cannot perform IO in in-memory database - TotalBlocks!");
	}
	idx_t FreeBlocks() override {
		throw InternalException("Cannot perform IO in in-memory database - FreeBlocks!");
	}
	// LCOV_EXCL_STOP
};
} // namespace duckdb
