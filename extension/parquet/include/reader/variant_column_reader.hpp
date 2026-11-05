//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/variant_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "parquet_arena.h"
#include "reader/templated_column_reader.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

class VariantColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::STRUCT;

public:
	VariantColumnReader(ClientContext &context, const ParquetReader &reader, const ParquetColumnSchema &schema,
	                    vector<unique_ptr<ColumnReader>> child_readers_p);
	VariantColumnReader(ClientContext &context, const ParquetReader &reader, const ParquetColumnSchema &schema,
	                    vector<unique_ptr<ColumnReader>> child_readers_p, const struct ColumnIndex &index);

	ClientContext &context;
	//! Optional: ColumnIndex used when pushing down extract to the scan
	const struct ColumnIndex index;
	//! Optional: Extract path distilled from 'index' (if set)
	const vector<VariantPathComponent> extract_path;
	vector<unique_ptr<ColumnReader>> child_readers;

public:
	ColumnReader &GetChildReader(idx_t child_idx);

	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const duckdb_parquet::ColumnChunkList &columns,
	                    TProtocol &protocol_p) override;

	idx_t Read(ColumnReaderInput &input, Vector &result) override;

	unique_ptr<BaseStatistics> Stats(idx_t row_group_idx_p, const duckdb_parquet::ColumnChunkList &columns) override;

	void Skip(idx_t num_values) override;
	idx_t GroupRowsAvailable() override;
	void Convert(Vector &metadata, Vector &group, Vector &result, idx_t count);
	void PrepareChunk(DataChunk &chunk, idx_t &capacity, const vector<LogicalType> &types, idx_t count);
	void GatherColumnChunks(unordered_set<idx_t> &column_chunks) override;
	void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) override;
	void InitializePageIndex(const ParquetRowGroupPageIndex &page_index) override;
	static bool TypedValueLayoutToType(const LogicalType &typed_value, LogicalType &logical_type);

protected:
	idx_t metadata_reader_idx;
	idx_t value_reader_idx;

	DataChunk intermediate_chunk;
	idx_t intermediate_capacity = 0;

	DataChunk shredded_chunk;
	idx_t shredded_capacity = 0;
};

} // namespace duckdb
