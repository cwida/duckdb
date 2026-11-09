//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_column_schema.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>
#include <string>

#include "duckdb.hpp"
#include "parquet_types.h"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

using namespace duckdb_parquet; // NOLINT

using duckdb_parquet::ConvertedType;
using duckdb_parquet::FieldRepetitionType;
using duckdb_parquet::SchemaElement;

using duckdb_parquet::FileMetaData;
struct ParquetOptions;

enum class ParquetColumnSchemaType { COLUMN, FILE_ROW_NUMBER, EXPRESSION, VARIANT, GEOMETRY, FILE_ROW_GROUP_NUMBER };

enum class ParquetExtraTypeInfo {
	NONE,
	IMPALA_TIMESTAMP,
	UNIT_NS,
	UNIT_MS,
	UNIT_MICROS,
	DECIMAL_BYTE_ARRAY,
	DECIMAL_INT32,
	DECIMAL_INT64,
	FLOAT16
};

constexpr LogicalTypeId ParquetTimestampLogicalType(ParquetExtraTypeInfo type_info) {
	switch (type_info) {
	case ParquetExtraTypeInfo::IMPALA_TIMESTAMP:
	case ParquetExtraTypeInfo::UNIT_MS:
	case ParquetExtraTypeInfo::UNIT_MICROS:
		return LogicalTypeId::TIMESTAMP;
	case ParquetExtraTypeInfo::UNIT_NS:
		return LogicalTypeId::TIMESTAMP_NS;
	default:
		return LogicalTypeId::INVALID;
	}
}

constexpr LogicalTypeId ParquetTimestampTzLogicalType(ParquetExtraTypeInfo type_info) {
	switch (type_info) {
	case ParquetExtraTypeInfo::UNIT_NS:
		return LogicalTypeId::TIMESTAMP_TZ_NS;
	case ParquetExtraTypeInfo::UNIT_MS:
	case ParquetExtraTypeInfo::UNIT_MICROS:
		return LogicalTypeId::TIMESTAMP_TZ;
	default:
		return LogicalTypeId::INVALID;
	}
}

constexpr LogicalTypeId ParquetTimeLogicalType(ParquetExtraTypeInfo type_info) {
	switch (type_info) {
	case ParquetExtraTypeInfo::UNIT_NS:
		return LogicalTypeId::TIME_NS;
	case ParquetExtraTypeInfo::UNIT_MS:
	case ParquetExtraTypeInfo::UNIT_MICROS:
		return LogicalTypeId::TIME;
	default:
		return LogicalTypeId::INVALID;
	}
}

constexpr LogicalTypeId ParquetTimeTzLogicalType(ParquetExtraTypeInfo type_info) {
	switch (type_info) {
	case ParquetExtraTypeInfo::UNIT_MS:
	case ParquetExtraTypeInfo::UNIT_MICROS:
	case ParquetExtraTypeInfo::UNIT_NS:
		return LogicalTypeId::TIME_TZ;
	default:
		return LogicalTypeId::INVALID;
	}
}

struct ParquetColumnSchema {
public:
	ParquetColumnSchema() = default;
	ParquetColumnSchema(ParquetColumnSchema &&other) = default;
	ParquetColumnSchema(const ParquetColumnSchema &other) = default;
	ParquetColumnSchema &operator=(ParquetColumnSchema &&other) = default;

public:
	//! Writer constructors
	static ParquetColumnSchema FromLogicalType(const Identifier &name, const LogicalType &type, idx_t max_define,
	                                           idx_t max_repeat, idx_t column_index,
	                                           duckdb_parquet::FieldRepetitionType::type repetition_type,
	                                           bool allow_geometry,
	                                           ParquetColumnSchemaType schema_type = ParquetColumnSchemaType::COLUMN);

public:
	//! Reader constructors
	static ParquetColumnSchema FromSchemaElement(const SchemaElement &element, idx_t max_define, idx_t max_repeat,
	                                             idx_t schema_index, idx_t column_index, ParquetColumnSchemaType type,
	                                             const ParquetOptions &options);
	static ParquetColumnSchema FromParentSchema(ParquetColumnSchema parent, LogicalType result_type,
	                                            ParquetColumnSchemaType schema_type);
	static ParquetColumnSchema FromChildSchemas(const string &name, const LogicalType &type, idx_t max_define,
	                                            idx_t max_repeat, idx_t schema_index, idx_t column_index,
	                                            vector<ParquetColumnSchema> &&children,
	                                            ParquetColumnSchemaType schema_type = ParquetColumnSchemaType::COLUMN);
	static ParquetColumnSchema FileRowNumber();
	static ParquetColumnSchema FileRowGroupNumber();

public:
	unique_ptr<BaseStatistics> Stats(const FileMetaData &file_meta_data, const ParquetOptions &parquet_options,
	                                 idx_t row_group_idx_p, const duckdb_parquet::ColumnChunkList &columns) const;
	void ValidateColumnMetadata(const duckdb_parquet::ColumnChunk &column, int64_t row_group_num_rows,
	                            bool validate_row_count, const char *file_name = nullptr) const;

public:
	optional_idx GetChildIndexByName(const string &name) const;
	const ParquetColumnSchema &GetChildByIndex(idx_t index) const;

public:
	void SetSchemaIndex(idx_t schema_idx);

public:
	string name;
	idx_t max_define;
	idx_t max_repeat;
	//! Populated by FinalizeSchema if used in the parquet_writer path
	optional_idx schema_index;
	idx_t column_index;
	ParquetColumnSchemaType schema_type;
	LogicalType type;
	optional_idx parent_schema_index;
	uint32_t type_length = 0;
	uint32_t type_scale = 0;
	duckdb_parquet::Type::type parquet_type = duckdb_parquet::Type::INT32;
	ParquetExtraTypeInfo type_info = ParquetExtraTypeInfo::NONE;
	vector<ParquetColumnSchema> children;
	optional_idx field_id;
	//! Whether a column is nullable or not
	duckdb_parquet::FieldRepetitionType::type repetition_type = duckdb_parquet::FieldRepetitionType::OPTIONAL;
	//! Whether the column can be recognized as a GEOMETRY type
	bool allow_geometry = false;
};

} // namespace duckdb
