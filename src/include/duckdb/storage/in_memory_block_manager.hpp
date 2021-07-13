//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/in_memory_block_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/block_manager.hpp"

namespace duckdb {

//! InMemoryBlockManager is an implementation for a BlockManager
class InMemoryBlockManager : public BlockManager {
public:
	void StartCheckpoint() override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
	unique_ptr<Block> CreateBlock() override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
	block_id_t GetFreeBlockId() override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
	block_id_t GetMetaBlock() override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
	void Read(Block &block) override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
	void Write(FileBuffer &block, block_id_t block_id) override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
	void WriteHeader(DatabaseHeader header) override {
		throw InternalException("Cannot perform IO in in-memory database!");
	}
};
} // namespace duckdb
