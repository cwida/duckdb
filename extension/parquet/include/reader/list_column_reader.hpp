//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/list_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "parquet_arena.h"
#include "reader/templated_column_reader.hpp"

namespace duckdb {

class ListColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::LIST;

public:
	ListColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema,
	                 unique_ptr<ColumnReader> child_column_reader_p);
	ColumnReader &GetChildReader();

	idx_t Read(ColumnReaderInput &input, Vector &result) override;

	void ApplyPendingSkips(data_ptr_t define_out, data_ptr_t repeat_out) override;

	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const duckdb_parquet::ColumnChunkList &columns,
	                    TProtocol &protocol_p) override {
		child_column_reader->InitializeRead(row_group_idx_p, row_group_num_rows, columns, protocol_p);
	}

	idx_t GroupRowsAvailable() override {
		return child_column_reader->GroupRowsAvailable() + overflow_child_count;
	}

	void GatherColumnChunks(unordered_set<idx_t> &column_chunks) override {
		child_column_reader->GatherColumnChunks(column_chunks);
	}

	void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) override {
		child_column_reader->RegisterPrefetch(transport, allow_merge);
	}

protected:
	template <class OP>
	idx_t ReadInternal(ColumnReaderInput &input, optional_ptr<Vector> result_out);

private:
	unique_ptr<ColumnReader> child_column_reader;
	ResizeableBuffer child_defines;
	ResizeableBuffer child_repeats;
	uint8_t *child_defines_ptr;
	uint8_t *child_repeats_ptr;

	VectorCache read_cache;
	Vector read_vector;

	idx_t overflow_child_count;
};

} // namespace duckdb
