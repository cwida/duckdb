//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/primitive_column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "column_writer.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"
#include "writer/parquet_write_stats.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "parquet_statistics.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "parquet_types.h"

namespace duckdb {
class ParquetWriter;
class Vector;
class WriteStream;
struct ParquetColumnSchema;
struct PrimitiveDictionaryTargetData;
struct ParquetPageIndexEntry;

struct PageInformation {
	idx_t offset = 0;
	idx_t row_count = 0;
	idx_t empty_count = 0;
	idx_t estimated_page_size = 0;
	idx_t null_count = 0;
};

struct PageWriteInformation {
	duckdb_parquet::PageHeader page_header;
	unique_ptr<MemoryStream> temp_writer;
	unique_ptr<ColumnWriterPageState> page_state;
	//! Statistics of this page alone, only tracked when the page index is written
	unique_ptr<ColumnWriterStatistics> page_stats;
	//! The number of (top-level) rows that start in this page, for the OffsetIndex
	idx_t num_rows = 0;
	//! The number of NULL values in this page (including NULLs inherited from a parent), for the ColumnIndex
	idx_t null_count = 0;
	idx_t write_page_idx = 0;
	idx_t write_count = 0;
	idx_t max_write_count = 0;
	size_t compressed_size;
	data_ptr_t compressed_data;
	AllocatedData compressed_buf;
	unique_ptr<AsyncWriteBuffer> prepared_header;
	unique_ptr<AsyncWriteBuffer> prepared_payload;
};

class PrimitiveColumnWriterState : public ColumnWriterState {
public:
	PrimitiveColumnWriterState(ParquetWriter &writer_p, duckdb_parquet::RowGroup &row_group, idx_t col_idx)
	    : writer(writer_p), row_group(row_group), col_idx(col_idx) {
		page_info.emplace_back();
	}
	~PrimitiveColumnWriterState() override = default;

	ParquetWriter &writer;
	duckdb_parquet::RowGroup &row_group;
	idx_t col_idx;
	vector<PageInformation> page_info;
	vector<PageWriteInformation> write_info;
	unique_ptr<ColumnWriterStatistics> stats_state;
	idx_t current_page = 0;
	//! Whether every data page starts at a row boundary, which the page index requires
	bool pages_start_at_rows = true;

	unique_ptr<ParquetBloomFilter> bloom_filter;
};

//! Base class for writing non-compound types (ex. numerics, strings)
class PrimitiveColumnWriter : public ColumnWriter {
public:
	PrimitiveColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path);
	~PrimitiveColumnWriter() override = default;

	//! We limit the uncompressed page size to 100MB
	//! The max size in Parquet is 2GB, but we choose a more conservative limit
	static constexpr const idx_t MAX_UNCOMPRESSED_PAGE_SIZE = 104857600ULL;
	//! The default uncompressed page size when the page index is written: 1 MiB, like parquet-mr (Spark), Arrow and
	//! parquet-rs. Pages are the unit readers skip with the page index, so a column chunk is split into several pages.
	//! Without the page index the default stays MAX_UNCOMPRESSED_PAGE_SIZE: one page per column chunk.
	static constexpr const idx_t DEFAULT_DATA_PAGE_SIZE_LIMIT = 1048576ULL;
	//! Dictionary pages must be below 2GB. Unlike data pages, there's only one dictionary page.
	//! For this reason we go with a much higher, but still a conservative upper bound of 1GB;
	static constexpr const idx_t MAX_UNCOMPRESSED_DICT_PAGE_SIZE = 1073741824ULL;

public:
	unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override;
	void Prepare(ColumnWriterState &state, ColumnWriterState *parent, Vector &vector, idx_t count,
	             bool vector_can_span_multiple_pages) override;
	void BeginWrite(ColumnWriterState &state) override;
	void Write(ColumnWriterState &state, Vector &vector, idx_t count) override;
	void PrepareWrite(ColumnWriterState &state) override;
	void FinalizeWrite(ColumnWriterState &state) override;
	idx_t FinalizeSchema(vector<duckdb_parquet::SchemaElement> &schemas) override;

protected:
	static void WriteLevels(Allocator &allocator, WriteStream &temp_writer, const unsafe_vector<uint16_t> &levels,
	                        idx_t max_value, idx_t start_offset, idx_t count, optional_idx null_count = optional_idx());

	virtual duckdb_parquet::Encoding::type GetEncoding(PrimitiveColumnWriterState &state);

	void NextPage(PrimitiveColumnWriterState &state);
	void FlushPage(PrimitiveColumnWriterState &state);

	//! Initializes the state used to track statistics during writing. Only used for scalar types.
	virtual unique_ptr<ColumnWriterStatistics> InitializeStatsState();

	//! Initialize the writer for a specific page. Only used for scalar types.
	virtual unique_ptr<ColumnWriterPageState> InitializePageState(PrimitiveColumnWriterState &state, idx_t page_idx);

	//! Flushes the writer for a specific page. Only used for scalar types.
	virtual void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state);

	//! Retrieves the row size of a vector at the specified location. Only used for scalar types.
	virtual idx_t GetRowSize(const Vector &vector, const idx_t index, const PrimitiveColumnWriterState &state) const;
	//! Writes a (subset of a) vector to the specified serializer. Only used for scalar types.
	virtual void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats, ColumnWriterPageState *page_state,
	                         Vector &vector, idx_t chunk_start, idx_t chunk_end) = 0;

	virtual bool HasDictionary(PrimitiveColumnWriterState &state_p) {
		return false;
	}
	//! The number of elements in the dictionary
	virtual idx_t DictionarySize(PrimitiveColumnWriterState &state_p);
	void WriteDictionary(PrimitiveColumnWriterState &state, unique_ptr<MemoryStream> temp_writer, idx_t row_count);
	void WriteDictionary(PrimitiveColumnWriterState &state, PrimitiveDictionaryTargetData target_data, idx_t row_count);
	virtual void FlushDictionary(PrimitiveColumnWriterState &state, ColumnWriterStatistics *stats);

	void SetParquetStatistics(PrimitiveColumnWriterState &state, duckdb_parquet::ColumnChunk &column);
	void RegisterToRowGroup(duckdb_parquet::RowGroup &row_group);
	//! Adds a page to the page index of the column chunk
	void AddPageToPageIndex(PrimitiveColumnWriterState &state, const PageWriteInformation &write_info,
	                        idx_t page_offset, idx_t header_size, ParquetPageIndexEntry &page_index,
	                        idx_t &first_row_index);
	//! Completes the page index of the column chunk (boundary order, whether a ColumnIndex can be written)
	void FinalizePageIndex(PrimitiveColumnWriterState &state, ParquetPageIndexEntry &page_index);
};

} // namespace duckdb
