//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/expression_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include "parquet_arena.h"

#include "column_reader.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/expression.hpp"
#include "parquet_column_schema.hpp"

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
class ClientContext;
class ThriftFileTransport;
class Vector;

//! A column reader that executes an expression over a child reader
class ExpressionColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::INVALID;

public:
	ExpressionColumnReader(ClientContext &context_p, vector<unique_ptr<ColumnReader>> child_readers,
	                       unique_ptr<Expression> expr, const ParquetColumnSchema &schema);
	ExpressionColumnReader(ClientContext &context_p, vector<unique_ptr<ColumnReader>> child_readers,
	                       unique_ptr<Expression> expr, unique_ptr<ParquetColumnSchema> owned_schema);

	//! Reader(s) to produce the input(s) for the expression
	vector<unique_ptr<ColumnReader>> child_readers;
	DataChunk intermediate_chunk;
	unique_ptr<Expression> expr;
	ExpressionExecutor executor;

	// If this reader was created on top of a child reader, after-the-fact, the schema needs to live somewhere
	unique_ptr<ParquetColumnSchema> owned_schema;

public:
	void InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows, const duckdb_parquet::ColumnChunkList &columns,
	                    TProtocol &protocol_p) override;

	idx_t Read(ColumnReaderInput &input, Vector &result) override;
	unique_ptr<BaseStatistics> Stats(idx_t row_group_idx_p, const duckdb_parquet::ColumnChunkList &columns) override;

	void Select(ColumnReaderInput &input, Vector &result, const SelectionVector &sel,
	            idx_t approved_tuple_count) override;

	void Skip(idx_t num_values) override;
	idx_t GroupRowsAvailable() override;

	void GatherColumnChunks(unordered_set<idx_t> &column_chunks) override {
		for (auto &child_reader : child_readers) {
			child_reader->GatherColumnChunks(column_chunks);
		}
	}

	idx_t FileOffset() const override {
		return child_readers[0]->FileOffset();
	}

	void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) override {
		for (auto &child_reader : child_readers) {
			child_reader->RegisterPrefetch(transport, allow_merge);
		}
	}
	void InitializePageIndex(const ParquetRowGroupPageIndex &page_index) override {
		for (auto &child_reader : child_readers) {
			child_reader->InitializePageIndex(page_index);
		}
	}

private:
	ClientContext &context;
	void InitializeChunk();
};

} // namespace duckdb
