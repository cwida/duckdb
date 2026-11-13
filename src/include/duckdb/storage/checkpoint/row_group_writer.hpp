//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/row_group_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/checkpoint_manager.hpp"

namespace duckdb {
struct ColumnCheckpointState;
class CheckpointWriter;
class ColumnData;
class ColumnSegment;
class RowGroup;
class BaseStatistics;
class SegmentStatistics;
class MetadataWriter;

// Writes data for an entire row group.
class RowGroupWriter {
public:
	RowGroupWriter(TableDataWriter &writer, TableCatalogEntry &table, PartialBlockManager &partial_block_manager);
	virtual ~RowGroupWriter() {
	}

	const vector<CompressionType> &GetCompressionTypes() const {
		return compression_types;
	}

	virtual CheckpointOptions GetCheckpointOptions() const = 0;
	virtual WriteStream &GetPayloadWriter() = 0;
	virtual MetaBlockPointer GetMetaBlockPointer() = 0;
	virtual optional_ptr<MetadataManager> GetMetadataManager() = 0;
	virtual void StartWritingColumns(vector<MetaBlockPointer> &column_metadata) {
	}
	virtual void FinishWritingColumns() {
	}

	DatabaseInstance &GetDatabase();
	AttachedDatabase &GetAttachedDatabase();
	TableDataWriter &GetTableWriter() {
		return writer;
	}
	PartialBlockManager &GetPartialBlockManager() {
		return partial_block_manager;
	}
	void SetHasUnloadedColumn(idx_t column_idx) {
		has_unloaded_columns.insert(column_idx);
	}
	unordered_set<idx_t> GetUnloadedColumns() const {
		return has_unloaded_columns;
	}

protected:
	TableDataWriter &writer;
	TableCatalogEntry &table;
	PartialBlockManager &partial_block_manager;
	vector<CompressionType> compression_types;
	unordered_set<idx_t> has_unloaded_columns;
};

// Writes data for an entire row group.
class SingleFileRowGroupWriter : public RowGroupWriter {
public:
	SingleFileRowGroupWriter(TableCatalogEntry &table, PartialBlockManager &partial_block_manager,
	                         TableDataWriter &writer, MetadataWriter &table_data_writer);

public:
	CheckpointOptions GetCheckpointOptions() const override;
	WriteStream &GetPayloadWriter() override;
	MetaBlockPointer GetMetaBlockPointer() override;
	optional_ptr<MetadataManager> GetMetadataManager() override;
	void StartWritingColumns(vector<MetaBlockPointer> &column_metadata) override;
	void FinishWritingColumns() override;

private:
	//! MetadataWriter is a cursor on a given BlockManager. This returns the
	//! cursor against which we should write payload data for the specified RowGroup.
	MetadataWriter &table_data_writer;
};

} // namespace duckdb
