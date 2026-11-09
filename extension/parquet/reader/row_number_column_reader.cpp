#include "reader/row_number_column_reader.hpp"

#include "parquet_reader.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
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
class Vector;
struct ParquetColumnSchema;
struct SelectionVector;
struct TableFilterState;

//===--------------------------------------------------------------------===//
// Row NumberColumn Reader
//===--------------------------------------------------------------------===//
RowNumberColumnReader::RowNumberColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
    : ColumnReader(reader, schema) {
}

void RowNumberColumnReader::InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows,
                                           const duckdb_parquet::ColumnChunkList &columns, TProtocol &protocol_p) {
	// the row numbers are produced from the row count without reading a column chunk - every column chunk has a value
	// (or NULL) per row, so it cannot have fewer values than the row group has rows
	for (auto &column : columns) {
		if (column.__isset.meta_data && column.meta_data.num_values < NumericCast<int64_t>(row_group_num_rows)) {
			throw DataCorruptionException(
			    "Failed to read file \"%s\": metadata is corrupt. Column has %lld values but row group has %llu rows",
			    reader.GetFileName(), column.meta_data.num_values, row_group_num_rows);
		}
	}
	row_group_offset = 0;
	auto &row_groups = reader.GetFileMetadata()->row_groups;
	for (idx_t i = 0; i < row_group_idx_p; i++) {
		row_group_offset += row_groups[i].num_rows;
	}
}

void RowNumberColumnReader::Filter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,
                                   TableFilterState &filter_state, SelectionVector &sel, idx_t &approved_tuple_count,
                                   bool is_first_filter) {
	// check the row id stats if this filter has any chance of passing

	auto &num_values = input.num_values;
	auto prune_result = RowGroup::CheckRowIdFilter(filter, row_group_offset, row_group_offset + num_values);
	if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE) {
		// filter is always false - don't read anything
		approved_tuple_count = 0;
		Skip(num_values);
		return;
	}
	ColumnReader::Filter(input, result, filter, filter_state, sel, approved_tuple_count, is_first_filter);
}

idx_t RowNumberColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	auto &num_values = input.num_values;

	auto data_ptr = FlatVector::Writer<int64_t>(result, num_values);
	for (idx_t i = 0; i < num_values; i++) {
		data_ptr.WriteValue(UnsafeNumericCast<int64_t>(row_group_offset++));
	}
	return num_values;
}

//===--------------------------------------------------------------------===//
// Row Group Column Reader
//===--------------------------------------------------------------------===//
RowGroupColumnReader::RowGroupColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
    : ColumnReader(reader, schema) {
}

void RowGroupColumnReader::InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows,
                                          const duckdb_parquet::ColumnChunkList &columns, TProtocol &protocol_p) {
	row_group_idx = row_group_idx_p;
}

idx_t RowGroupColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	auto &num_values = input.num_values;

	// the row group number is constant for all rows within a row group - emit a constant vector
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<uint64_t>(result)[0] = UnsafeNumericCast<uint64_t>(row_group_idx);
	return num_values;
}

} // namespace duckdb
