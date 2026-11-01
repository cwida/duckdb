#include "parquet_column_schema.hpp"

#include <utility>

#include "parquet_reader.hpp"
#include "column_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "parquet_statistics.hpp"

namespace duckdb {

optional_idx ParquetColumnSchema::GetChildIndexByName(const string &name) const {
	for (idx_t i = 0; i < children.size(); i++) {
		auto &child = children[i];
		if (name == child.name) {
			return i;
		}
	}
	return optional_idx();
}

const ParquetColumnSchema &ParquetColumnSchema::GetChildByIndex(idx_t index) const {
	if (index >= children.size()) {
		throw InternalException("ParquetColumnSchema::GetChildByIndex: index (%d) out of range (size: %d)", index,
		                        children.size());
	}
	return children[index];
}

void ParquetColumnSchema::SetSchemaIndex(idx_t schema_idx) {
	D_ASSERT(!schema_index.IsValid());
	schema_index = schema_idx;
}

void ParquetColumnSchema::ValidateColumnMetadata(const ColumnChunk &column, int64_t row_group_num_rows,
                                                 bool validate_row_count, const char *file_name) const {
	if (!column.__isset.meta_data) {
		return;
	}
	auto &metadata = column.meta_data;
	if (metadata.data_page_offset < 0 || metadata.dictionary_page_offset < 0 || metadata.index_page_offset < 0 ||
	    metadata.total_compressed_size < 0) {
		if (file_name) {
			throw DataCorruptionException(
			    "Failed to read file \"%s\": metadata is corrupt. Column chunk has a negative offset or size",
			    file_name);
		}
		throw DataCorruptionException("Parquet metadata is corrupt. Column chunk has a negative offset or size");
	}
	if (metadata.num_values < 0) {
		if (file_name) {
			throw DataCorruptionException("Failed to read file \"%s\": metadata is corrupt. Column has invalid "
			                              "number of values (%lld)",
			                              file_name, metadata.num_values);
		}
		throw DataCorruptionException("Parquet metadata is corrupt. Column has invalid number of values (%lld)",
		                              metadata.num_values);
	}
	if (validate_row_count && !type.IsNested() && max_repeat == 0 && metadata.num_values != row_group_num_rows) {
		if (file_name) {
			throw DataCorruptionException(
			    "Failed to read file \"%s\": metadata is corrupt. Column has %lld values but row group has %lld rows",
			    file_name, metadata.num_values, row_group_num_rows);
		}
		throw DataCorruptionException("Parquet metadata is corrupt. Column has %lld values but row group has %lld rows",
		                              metadata.num_values, row_group_num_rows);
	}
}

//! Writer constructors

ParquetColumnSchema ParquetColumnSchema::FromLogicalType(const Identifier &name, const LogicalType &type,
                                                         idx_t max_define, idx_t max_repeat, idx_t column_index,
                                                         duckdb_parquet::FieldRepetitionType::type repetition_type,
                                                         bool allow_geometry, ParquetColumnSchemaType schema_type) {
	ParquetColumnSchema res;
	res.name = name.GetIdentifierName();
	res.max_define = max_define;
	res.max_repeat = max_repeat;
	res.column_index = column_index;
	res.repetition_type = repetition_type;
	res.schema_type = schema_type;
	res.type = type;
	res.allow_geometry = allow_geometry;
	return res;
}

//! Reader constructors

ParquetColumnSchema ParquetColumnSchema::FromSchemaElement(const duckdb_parquet::SchemaElement &element,
                                                           idx_t max_define, idx_t max_repeat, idx_t schema_index,
                                                           idx_t column_index, ParquetColumnSchemaType schema_type,
                                                           const ParquetOptions &options) {
	ParquetColumnSchema res;
	res.name = element.name;
	res.max_define = max_define;
	res.max_repeat = max_repeat;
	res.schema_index = schema_index;
	res.column_index = column_index;
	res.schema_type = schema_type;
	res.type = ParquetReader::DeriveLogicalType(element, options, res);
	return res;
}

ParquetColumnSchema ParquetColumnSchema::FromParentSchema(ParquetColumnSchema parent, LogicalType result_type,
                                                          ParquetColumnSchemaType schema_type) {
	ParquetColumnSchema res;
	res.name = parent.name;
	res.max_define = parent.max_define;
	res.max_repeat = parent.max_repeat;
	D_ASSERT(parent.schema_index.IsValid());
	res.schema_index = parent.schema_index;
	res.column_index = parent.column_index;
	res.schema_type = schema_type;
	res.type = std::move(result_type);
	res.children.push_back(std::move(parent));
	return res;
}

ParquetColumnSchema ParquetColumnSchema::FromChildSchemas(const string &name, const LogicalType &type, idx_t max_define,
                                                          idx_t max_repeat, idx_t schema_index, idx_t column_index,
                                                          vector<ParquetColumnSchema> &&children,
                                                          ParquetColumnSchemaType schema_type) {
	ParquetColumnSchema res;
	res.name = name;
	res.max_define = max_define;
	res.max_repeat = max_repeat;
	res.schema_index = schema_index;
	res.column_index = column_index;
	res.schema_type = schema_type;
	res.type = type;
	res.children = std::move(children);
	return res;
}

ParquetColumnSchema ParquetColumnSchema::FileRowNumber() {
	ParquetColumnSchema res;
	res.name = "file_row_number";
	res.max_define = 0;
	res.max_repeat = 0;
	res.schema_index = 0;
	res.column_index = 0;
	res.schema_type = ParquetColumnSchemaType::FILE_ROW_NUMBER;
	res.type = LogicalType::BIGINT, res.repetition_type = duckdb_parquet::FieldRepetitionType::type::OPTIONAL;
	return res;
}

ParquetColumnSchema ParquetColumnSchema::FileRowGroupNumber() {
	ParquetColumnSchema res;
	res.name = "file_row_group_number";
	res.max_define = 0;
	res.max_repeat = 0;
	res.schema_index = 0;
	res.column_index = 0;
	res.schema_type = ParquetColumnSchemaType::FILE_ROW_GROUP_NUMBER;
	res.type = LogicalType::UBIGINT;
	res.repetition_type = duckdb_parquet::FieldRepetitionType::type::OPTIONAL;
	return res;
}

unique_ptr<BaseStatistics> ParquetColumnSchema::Stats(const FileMetaData &file_meta_data,
                                                      const ParquetOptions &parquet_options, idx_t row_group_idx_p,
                                                      const duckdb_parquet::ColumnChunkList &columns) const {
	if (schema_type == ParquetColumnSchemaType::EXPRESSION) {
		return nullptr;
	}
	D_ASSERT(row_group_idx_p < file_meta_data.row_groups.size());
	auto &row_group = file_meta_data.row_groups[row_group_idx_p];
	if (row_group.num_rows < 0) {
		throw InvalidInputException("Parquet metadata is corrupt. Row group has invalid number of rows (%lld)",
		                            row_group.num_rows);
	}
	if (schema_type == ParquetColumnSchemaType::COLUMN && column_index < columns.size()) {
		ValidateColumnMetadata(columns[column_index], row_group.num_rows, true);
	}
	if (schema_type == ParquetColumnSchemaType::FILE_ROW_GROUP_NUMBER) {
		// the row group number is constant within a row group - set min and max to the row group index
		auto stats = NumericStats::CreateUnknown(type);
		NumericStats::SetMin(stats, Value::UBIGINT(UnsafeNumericCast<uint64_t>(row_group_idx_p)));
		NumericStats::SetMax(stats, Value::UBIGINT(UnsafeNumericCast<uint64_t>(row_group_idx_p)));
		stats.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		return stats.ToUnique();
	}
	if (schema_type == ParquetColumnSchemaType::FILE_ROW_NUMBER) {
		auto &row_groups = file_meta_data.row_groups;
		if (row_groups[row_group_idx_p].num_rows == 0) {
			return NumericStats::CreateEmpty(type).ToUnique();
		}

		idx_t row_group_offset_min = 0;
		for (idx_t i = 0; i < row_group_idx_p; i++) {
			row_group_offset_min += row_groups[i].num_rows;
		}

		auto stats = NumericStats::CreateUnknown(type);
		NumericStats::SetMin(stats, Value::BIGINT(UnsafeNumericCast<int64_t>(row_group_offset_min)));
		NumericStats::SetMax(stats, Value::BIGINT(UnsafeNumericCast<int64_t>(
		                                row_group_offset_min + row_groups[row_group_idx_p].num_rows - 1)));
		stats.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		return stats.ToUnique();
	}
	return ParquetStatisticsUtils::TransformColumnStatistics(*this, columns, parquet_options.can_have_nan);
}

} // namespace duckdb
