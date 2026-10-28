//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/struct_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "reader/templated_column_reader.hpp"

namespace duckdb {

class StructColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::STRUCT;

public:
	StructColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema,
	                   vector<unique_ptr<ColumnReader>> child_readers_p);

	vector<unique_ptr<ColumnReader>> child_readers;

public:
	ColumnReader &GetChildReader(idx_t child_idx);

	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const vector<ColumnChunk> &columns,
	                    TProtocol &protocol_p) override;

	idx_t Read(ColumnReaderInput &input, Vector &result) override;

	void Skip(idx_t num_values) override;
	idx_t GroupRowsAvailable() override;
	void GatherColumnChunks(unordered_set<idx_t> &column_chunks) override;
	void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) override;
	void InitializePageIndex(const ParquetRowGroupPageIndex &page_index) override;
};

} // namespace duckdb
