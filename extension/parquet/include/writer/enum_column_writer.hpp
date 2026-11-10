//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/enum_column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <string>

#include "writer/primitive_column_writer.hpp"
#include "column_writer.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "parquet_types.h"
#include "writer/parquet_write_stats.hpp"

namespace duckdb {
class EnumWriterPageState;
class ParquetWriter;
class Vector;
class WriteStream;
struct ParquetColumnSchema;

class EnumColumnWriter : public PrimitiveColumnWriter {
public:
	EnumColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path_p);
	~EnumColumnWriter() override = default;

	unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override;
	unique_ptr<ColumnWriterStatistics> InitializeStatsState() override;

	void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p, ColumnWriterPageState *page_state_p,
	                 Vector &input_column, idx_t chunk_start, idx_t chunk_end) override;

	unique_ptr<ColumnWriterPageState> InitializePageState(PrimitiveColumnWriterState &state, idx_t page_idx) override;

	void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) override;

	duckdb_parquet::Encoding::type GetEncoding(PrimitiveColumnWriterState &state) override;

	bool HasDictionary(PrimitiveColumnWriterState &state) override;

	idx_t DictionarySize(PrimitiveColumnWriterState &state_p) override;

	void FlushDictionary(PrimitiveColumnWriterState &state, ColumnWriterStatistics *stats_p) override;

	idx_t GetRowSize(const Vector &vector, const idx_t index, const PrimitiveColumnWriterState &state) const override;

private:
	template <class T>
	void WriteEnumInternal(WriteStream &temp_writer, Vector &input_column, idx_t chunk_start, idx_t chunk_end,
	                       EnumWriterPageState &page_state);

	uint32_t bit_width;
};

} // namespace duckdb
