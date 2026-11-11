//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/struct_column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_writer.hpp"

namespace duckdb {

class StructColumnWriter : public ColumnWriter {
public:
	StructColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path_p,
	                   vector<unique_ptr<ColumnWriter>> child_writers_p)
	    : ColumnWriter(writer, std::move(column_schema), std::move(schema_path_p)) {
		child_writers = std::move(child_writers_p);
		for (auto &writer : child_writers) {
			writer->parent = *this;
		}
	}
	~StructColumnWriter() override = default;

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
};

} // namespace duckdb
