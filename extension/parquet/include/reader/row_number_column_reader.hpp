//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/row_number_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include "parquet_arena.h"

#include "duckdb/common/limits.hpp"
#include "column_reader.hpp"
#include "reader/templated_column_reader.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb_apache {
namespace thrift {
namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache
namespace duckdb_parquet {
class ColumnChunk;
} // namespace duckdb_parquet

namespace duckdb {
class ParquetReader;
class TableFilter;
class ThriftFileTransport;
class Vector;
struct ParquetColumnSchema;
struct SelectionVector;
struct TableFilterState;

//! Reads a file-absolute row number as a virtual column that's not actually stored in the file
class RowNumberColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::INT64;

public:
	RowNumberColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema);

public:
	idx_t Read(ColumnReaderInput &input, Vector &result) override;
	void Filter(ColumnReaderInput &input, Vector &result, const TableFilter &filter, TableFilterState &filter_state,
	            SelectionVector &sel, idx_t &approved_tuple_count, bool is_first_filter) override;

	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const duckdb_parquet::ColumnChunkList &columns,
	                    TProtocol &protocol_p) override;

	void Skip(idx_t num_values) override {
		row_group_offset += num_values;
	}
	idx_t GroupRowsAvailable() override {
		return NumericLimits<idx_t>::Maximum();
	};
	void GatherColumnChunks(unordered_set<idx_t> &column_chunks) override {
	}
	idx_t FileOffset() const override {
		return 0;
	}
	void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) override {
	}

private:
	idx_t row_group_offset;
};

//! Reads the file-relative row group number as a virtual column that's not actually stored in the file
class RowGroupColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::INT64;

public:
	RowGroupColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema);

public:
	idx_t Read(ColumnReaderInput &input, Vector &result) override;

	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const duckdb_parquet::ColumnChunkList &columns,
	                    TProtocol &protocol_p) override;

	void Skip(idx_t num_values) override {
	}
	idx_t GroupRowsAvailable() override {
		return NumericLimits<idx_t>::Maximum();
	};
	void GatherColumnChunks(unordered_set<idx_t> &column_chunks) override {
	}
	idx_t FileOffset() const override {
		return 0;
	}
	void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) override {
	}

private:
	//! The index of the row group currently being read
	idx_t row_group_idx = 0;
};

} // namespace duckdb
