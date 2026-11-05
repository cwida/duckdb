//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/list_column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_writer.hpp"

namespace duckdb {

class ListColumnWriterState : public ColumnWriterState {
public:
	ListColumnWriterState(duckdb_parquet::RowGroup &row_group, idx_t col_idx) : row_group(row_group), col_idx(col_idx) {
	}
	~ListColumnWriterState() override = default;

	duckdb_parquet::RowGroup &row_group;
	idx_t col_idx;
	unique_ptr<ColumnWriterState> child_state;
	idx_t parent_index = 0;
	//! Null containers, excluding absent parents and empty containers
	idx_t own_null_count = 0;
	//! Containers with a present parent, including null and empty containers
	idx_t num_values = 0;
};

class ListColumnWriter : public ColumnWriter {
public:
	ListColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path_p,
	                 unique_ptr<ColumnWriter> child_writer_p)
	    : ColumnWriter(writer, std::move(column_schema), std::move(schema_path_p)) {
		child_writers.push_back(std::move(child_writer_p));
	}
	~ListColumnWriter() override = default;

public:
	unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override;
	bool HasAnalyze() override;
	void Analyze(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count) override;
	void FinalizeAnalyze(ColumnWriterState &state) override;
	void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count,
	             bool vector_can_span_multiple_pages) override;

	void BeginWrite(ColumnWriterState &state) override;
	void Write(ColumnWriterState &state, Vector &vector, idx_t count) override;
	void PrepareWrite(ColumnWriterState &state) override;
	void FinalizeWrite(ColumnWriterState &state) override;
	idx_t FinalizeSchema(duckdb_parquet::SchemaElementList &schemas) override;

protected:
	ColumnWriter &GetChildWriter();
	const ColumnWriter &GetChildWriter() const;
};

} // namespace duckdb
