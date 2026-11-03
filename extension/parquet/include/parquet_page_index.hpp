//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_page_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/vector.hpp"
#include "parquet_types.h"

namespace duckdb_apache {
namespace thrift {
namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache

namespace duckdb {
class TableFilter;
struct ParquetColumnSchema;

//! A range of rows [begin, end) of a row group that may still contain matching rows
struct ParquetPageRowRange {
	idx_t begin;
	idx_t end;
};

//! The page index (ColumnIndex / OffsetIndex) of the column chunks of one row group that a scan uses
struct ParquetRowGroupPageIndex {
	//! Backs the Thrift page index objects of the current row group
	ArenaAllocator arena {Allocator::DefaultAllocator()};
	//! The OffsetIndex per column chunk of the row group (empty entries for chunks without one)
	vector<unique_ptr<duckdb_parquet::OffsetIndex>> offset_indexes;
	//! The rows of the row group that may match the filters, sorted and non-overlapping
	vector<ParquetPageRowRange> row_ranges;

	void Reset() {
		offset_indexes.clear();
		row_ranges.clear();
		arena.Reset();
	}
	bool HasOffsetIndex(idx_t column_chunk_idx) const {
		return column_chunk_idx < offset_indexes.size() && offset_indexes[column_chunk_idx];
	}
};

//! Helpers for the Parquet page index, see https://parquet.apache.org/docs/file-format/pageindex/
struct ParquetPageIndexUtils {
	static bool HasColumnIndex(const duckdb_parquet::ColumnChunk &column_chunk);
	static bool HasOffsetIndex(const duckdb_parquet::ColumnChunk &column_chunk);
	//! Reads the ColumnIndex of a column chunk, the protocol must be positioned at the ColumnIndex
	static void ReadColumnIndex(duckdb_apache::thrift::protocol::TProtocol &protocol,
	                            const duckdb_parquet::ColumnChunk &column_chunk, duckdb_parquet::ColumnIndex &result);
	//! Reads the OffsetIndex of a column chunk, the protocol must be positioned at the OffsetIndex
	static void ReadOffsetIndex(duckdb_apache::thrift::protocol::TProtocol &protocol,
	                            const duckdb_parquet::ColumnChunk &column_chunk, duckdb_parquet::OffsetIndex &result);
	//! Whether the OffsetIndex describes consecutive pages that start at the first row and lie inside the byte
	//! range of their own column chunk
	static bool OffsetIndexIsValid(const duckdb_parquet::OffsetIndex &offset_index, idx_t row_group_num_rows,
	                               idx_t file_size, const duckdb_parquet::ColumnChunk &column_chunk);
	//! Whether the ColumnIndex has consistent entries for the given number of pages
	static bool ColumnIndexIsValid(const duckdb_parquet::ColumnIndex &column_index, idx_t page_count);
	//! The number of rows of a page, from the first row index of the page and of its successor
	static idx_t PageRowCount(const duckdb_parquet::OffsetIndex &offset_index, idx_t page_idx,
	                          idx_t row_group_num_rows);
	//! Evaluates the filter against the bounds of every page: result[page] is false when the page cannot match
	static void FilterPages(const ParquetColumnSchema &schema, const duckdb_parquet::ColumnIndex &column_index,
	                        const TableFilter &filter, bool can_have_nan, vector<bool> &result);
	//! The rows of the pages that may match, as sorted non-overlapping ranges
	static vector<ParquetPageRowRange> PagesToRowRanges(const duckdb_parquet::OffsetIndex &offset_index,
	                                                    const vector<bool> &page_may_match, idx_t row_group_num_rows);
	//! Intersects two sorted range lists
	static vector<ParquetPageRowRange> IntersectRowRanges(const vector<ParquetPageRowRange> &left,
	                                                      const vector<ParquetPageRowRange> &right);
	//! The number of rows covered by the ranges
	static idx_t RowCount(const vector<ParquetPageRowRange> &ranges);
	//! Whether the rows [begin, end) of a page overlap any of the ranges (sorted, non-overlapping)
	static bool PageOverlapsRowRanges(const vector<ParquetPageRowRange> &ranges, idx_t begin, idx_t end);
};

} // namespace duckdb
