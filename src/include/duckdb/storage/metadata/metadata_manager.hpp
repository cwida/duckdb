//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/metadata/metadata_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"

namespace duckdb {
class DatabaseInstance;
struct MetadataBlockInfo;

struct MetadataBlock {
	MetadataBlock();
	// disable copy constructors
	MetadataBlock(const MetadataBlock &other) = delete;
	MetadataBlock &operator=(const MetadataBlock &) = delete;
	//! enable move constructors
	DUCKDB_API MetadataBlock(MetadataBlock &&other) noexcept;
	DUCKDB_API MetadataBlock &operator=(MetadataBlock &&) noexcept;

	shared_ptr<BlockHandle> block;
	block_id_t block_id;
	vector<uint8_t> free_blocks;
	atomic<bool> dirty;

	void Write(WriteStream &sink);
	static MetadataBlock Read(ReadStream &source);

	idx_t FreeBlocksToInteger();
	void FreeBlocksFromInteger(idx_t blocks);
	static vector<uint8_t> BlocksFromInteger(idx_t free_list);

	string ToString() const;
};

struct MetadataPointer {
	idx_t block_index : 56;
	uint8_t index : 8;
};

struct MetadataHandle {
	MetadataPointer pointer;
	BufferHandle handle;
};

class MetadataManager {
public:
	//! The amount of metadata blocks per storage block
	static constexpr const idx_t METADATA_BLOCK_COUNT = 64;

public:
	MetadataManager(BlockManager &block_manager, BufferManager &buffer_manager);
	~MetadataManager();

	BufferManager &GetBufferManager() const {
		return buffer_manager;
	}

	MetadataHandle AllocateHandle();
	MetadataHandle Pin(const MetadataPointer &pointer);

	MetadataHandle Pin(const QueryContext &context, const MetadataPointer &pointer);

	MetaBlockPointer GetDiskPointer(const MetadataPointer &pointer, uint32_t offset = 0);
	MetadataPointer FromDiskPointer(MetaBlockPointer pointer);
	MetadataPointer RegisterDiskPointer(MetaBlockPointer pointer);

	static BlockPointer ToBlockPointer(MetaBlockPointer meta_pointer, const idx_t metadata_block_size);
	static MetaBlockPointer FromBlockPointer(BlockPointer block_pointer, const idx_t metadata_block_size);

	//! Flush all blocks to disk
	void Flush(QueryContext context = QueryContext());

	bool BlockIsModified(const MetaBlockPointer &ptr);
	bool BlockHasBeenCleared(const MetaBlockPointer &ptr);

	void MarkBlocksAsModified();
	void ClearModifiedBlocks(const vector<MetaBlockPointer> &pointers);
	//! Sets the storage version of the file, which decides whether keeping a block also keeps its next block
	void SetStorageVersion(StorageVersion storage_version);
	//! The block that the next pointer of a block in use refers to, invalid at the end of its chain
	MetaBlockPointer GetNextBlock(MetaBlockPointer pointer);

	vector<MetadataBlockInfo> GetMetadataInfo() const;
	vector<shared_ptr<BlockHandle>> GetBlocks() const;
	idx_t BlockCount();

	void Write(WriteStream &sink);
	void Read(ReadStream &source);

	idx_t GetMetadataBlockSize() const;

protected:
	BlockManager &block_manager;
	BufferManager &buffer_manager;
	mutable mutex block_lock;
	unordered_map<block_id_t, MetadataBlock> blocks;
	unordered_map<block_id_t, idx_t> modified_blocks;
	//! Readers before v1.4.2 resolve the next pointer of a block when they enter it, so for files they can open the
	//! block a kept block points to must stay a metadata block
	bool keep_next_blocks = false;

protected:
	block_id_t AllocateNewBlock(unique_lock<mutex> &block_lock);
	block_id_t PeekNextBlockId() const;
	block_id_t GetNextBlockId() const;

	void AddBlock(unique_lock<mutex> &block_lock, MetadataBlock new_block, bool if_exists = false);
	void AddAndRegisterBlock(unique_lock<mutex> &block_lock, MetadataBlock block);
	void ConvertToTransient(unique_lock<mutex> &block_lock, MetadataBlock &block);
	MetadataPointer FromDiskPointerInternal(unique_lock<mutex> &block_lock, MetaBlockPointer pointer);
};

} // namespace duckdb
