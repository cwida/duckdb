//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/in_memory_checkpoint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/checkpoint/row_group_writer.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/checkpoint_manager.hpp"
#include "duckdb/storage/partial_block_manager.hpp"

namespace duckdb {

class InMemoryCheckpointer final : public CheckpointWriter {
public:
	InMemoryCheckpointer(QueryContext context, AttachedDatabase &db, BlockManager &block_manager,
	                     StorageManager &storage_manager, CheckpointOptions options);

	void CreateCheckpoint() override;

	MetadataWriter &GetMetadataWriter() override;
	MetadataManager &GetMetadataManager() override;
	unique_ptr<TableDataWriter> GetTableDataWriter(TableCatalogEntry &table) override;
	optional_ptr<ClientContext> GetClientContext() const {
		return context;
	}
	CheckpointOptions GetCheckpointOptions() const {
		return options;
	}
	PartialBlockManager &GetPartialBlockManager() {
		return partial_block_manager;
	}

public:
	void WriteTable(TableCatalogEntry &table, Serializer &serializer) override;

private:
	optional_ptr<ClientContext> context;
	PartialBlockManager partial_block_manager;
	StorageManager &storage_manager;
	CheckpointOptions options;
};

class InMemoryTableDataWriter : public TableDataWriter {
public:
	InMemoryTableDataWriter(InMemoryCheckpointer &checkpoint_manager, TableCatalogEntry &table);

public:
	void WriteUnchangedTable(MetaBlockPointer pointer, const vector<MetaBlockPointer> &metadata_pointers,
	                         idx_t total_rows, idx_t next_row_id) override;
	void FinalizeTable(const TableStatistics &global_stats, DataTableInfo &info, RowGroupCollection &collection,
	                   const vector<shared_ptr<const IndexStorageInfo>> &index_infos, Serializer &serializer) override;
	unique_ptr<RowGroupWriter> GetRowGroupWriter(RowGroup &row_group) override;
	unique_ptr<TableIndexWriter> GetTableIndexWriter(StorageVersion version) override;
	void FlushPartialBlocks() override;
	CheckpointOptions GetCheckpointOptions() const override;
	MetadataManager &GetMetadataManager() override;

private:
	InMemoryCheckpointer &checkpoint_manager;
};

class InMemoryRowGroupWriter : public RowGroupWriter {
public:
	InMemoryRowGroupWriter(TableDataWriter &writer, TableCatalogEntry &table,
	                       PartialBlockManager &partial_block_manager, InMemoryCheckpointer &checkpoint_manager);

public:
	CheckpointOptions GetCheckpointOptions() const override;
	WriteStream &GetPayloadWriter() override;
	MetaBlockPointer GetMetaBlockPointer() override;
	optional_ptr<MetadataManager> GetMetadataManager() override;

private:
	//! Underlying writer object
	InMemoryCheckpointer &checkpoint_manager;
	// Nop metadata writer
	MemoryStream metadata_writer;
};

} // namespace duckdb
