//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/table_data_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"
#include "duckdb/storage/checkpoint/row_group_writer.hpp"
#include "duckdb/storage/index_storage_info.hpp"

namespace duckdb {
class TableIndexList;
class DuckTableEntry;
class TableStatistics;
class SingleFileCheckpointWriter;
class RowGroupCollection;
struct RowGroupPointer;

//! The table data writer is responsible for writing the data of a table to storage.
//
//! This is meant to encapsulate and abstract:
//!  - Storage/encoding of table metadata (block pointers)
//!  - Mapping management of data block locations
//! Abstraction will support, for example: tiering, versioning, or splitting into multiple block managers.
class TableDataWriter {
public:
	explicit TableDataWriter(TableCatalogEntry &table, QueryContext context);
	virtual ~TableDataWriter();

public:
	void WriteTableData(Serializer &metadata_serializer);

	virtual void WriteUnchangedTable(MetaBlockPointer pointer, const vector<MetaBlockPointer> &metadata_pointers,
	                                 idx_t total_rows, idx_t next_row_id) = 0;
	//! global_stats is empty when reusing existing table metadata.
	virtual void FinalizeTable(const TableStatistics &global_stats, DataTableInfo &info, RowGroupCollection &collection,
	                           const vector<shared_ptr<const IndexStorageInfo>> &index_infos,
	                           Serializer &serializer) = 0;

	virtual unique_ptr<RowGroupWriter> GetRowGroupWriter(RowGroup &row_group) = 0;
	virtual unique_ptr<TableIndexWriter> GetTableIndexWriter(StorageVersion version) = 0;

	virtual void AddRowGroup(RowGroupPointer &&row_group_pointer, unique_ptr<RowGroupWriter> writer);
	virtual CheckpointOptions GetCheckpointOptions() const = 0;
	virtual void FlushPartialBlocks() = 0;
	virtual MetadataManager &GetMetadataManager() = 0;
	//! Whether this is a SingleFileTableDataWriter
	virtual bool IsSingleFileWriter() const {
		return false;
	}
	optional_idx GetRowGroupCount() {
		return row_group_count;
	}
	void SetRowGroupCount(optional_idx row_group_count_p) {
		row_group_count = row_group_count_p;
	}
	bool GetRebuildIndexes() const {
		return rebuild_indexes;
	}
	void SetRebuildIndexes() {
		rebuild_indexes = true;
	}
	bool RequireLegacyStartRow() const {
		return require_legacy_start_row;
	}
	bool CanPersistRowIdGaps() const {
		return can_persist_rowid_gaps;
	}
	void SetRowIdsChanged() {
		row_ids_changed = true;
	}
	bool RowIdsChanged() const {
		return row_ids_changed;
	}

	AttachedDatabase &GetAttached();
	DatabaseInstance &GetDatabase();
	unique_ptr<TaskExecutor> CreateTaskExecutor();
	optional_ptr<ClientContext> TryGetClientContext() const;

protected:
	DuckTableEntry &table;
	optional_ptr<ClientContext> context;
	//! Pointers to the start of each row group.
	vector<RowGroupPointer> row_group_pointers;

	optional_idx row_group_count;
	bool rebuild_indexes = false;
	bool require_legacy_start_row = false;
	bool can_persist_rowid_gaps = false;
	atomic<bool> row_ids_changed {false};
};

class SingleFileTableDataWriter : public TableDataWriter {
public:
	SingleFileTableDataWriter(SingleFileCheckpointWriter &checkpoint_manager, TableCatalogEntry &table,
	                          MetadataWriter &table_data_writer);

public:
	void WriteUnchangedTable(MetaBlockPointer pointer, const vector<MetaBlockPointer> &metadata_pointers,
	                         idx_t total_rows, idx_t next_row_id) override;
	void FinalizeTable(const TableStatistics &global_stats, DataTableInfo &info, RowGroupCollection &collection,
	                   const vector<shared_ptr<const IndexStorageInfo>> &index_infos, Serializer &serializer) override;

	unique_ptr<RowGroupWriter> GetRowGroupWriter(RowGroup &row_group) override;
	unique_ptr<TableIndexWriter> GetTableIndexWriter(StorageVersion version) override;

	CheckpointOptions GetCheckpointOptions() const override;
	void FlushPartialBlocks() override;
	MetadataManager &GetMetadataManager() override;
	bool IsSingleFileWriter() const override {
		return true;
	}

private:
	SingleFileCheckpointWriter &checkpoint_manager;
	//! Writes the actual table data
	MetadataWriter &table_data_writer;
	//! The root pointer, if we are re-using metadata of the table
	MetaBlockPointer existing_pointer;
	optional_idx existing_rows;
	optional_idx existing_next_row_id;
	vector<MetaBlockPointer> existing_pointers;
};

} // namespace duckdb
