//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/table_index_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/storage/index_storage_info.hpp"

namespace duckdb {
class PartialBlockManager;
class SingleFileCheckpointWriter;
class BoundIndex;

//! Checkpointing result for an index. The shadow index may be empty either when
//! the index is unbound, or the index does not support deferred checkpointing.
struct IndexCheckpointResult {
	shared_ptr<const IndexStorageInfo> storage_info;
	unique_ptr<BoundIndex> shadow_index;
};

class TableIndexWriter {
protected:
	explicit TableIndexWriter(PartialBlockManager &partial_block_manager, StorageVersion version);

public:
	virtual ~TableIndexWriter();

public:
	//! Segments managed by this block manager will be packed across indexes.
	PartialBlockManager &GetSharedPartialBlockManager() const {
		return partial_block_manager;
	}
	//! Segments managed by this block manager will be packer on a per-index basis.
	virtual PartialBlockManager CreatePerIndexPartialBlockManager() = 0;
	//! Writes the index buffers to disk
	virtual void Flush() = 0;
	//! Get the targeted storage version for the current writer
	StorageVersion GetStorageVersion() const;

protected:
	//! Used to colocate blocks across indexes, used when index supports deferred checkpointing.
	PartialBlockManager &partial_block_manager;
	StorageVersion storage_version;
};

class SingleFileTableIndexWriter : public TableIndexWriter {
public:
	explicit SingleFileTableIndexWriter(SingleFileCheckpointWriter &checkpoint_manager, StorageVersion version);

private:
	PartialBlockManager CreatePerIndexPartialBlockManager() override;
	void Flush() override;

private:
	SingleFileCheckpointWriter &checkpoint_manager;
};

} // namespace duckdb
