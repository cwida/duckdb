#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <sstream>

#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "parquet_metadata.hpp"
#include "parquet_page_index.hpp"
#include "parquet_statistics.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/file_glob_options.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/original/std/memory.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_file_metadata_cache.hpp"
#include "parquet_types.h"
#include "thrift/protocol/TCompactProtocol.h"
#include "thrift_tools.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {
class ClientContext;
class ExecutionContext;
enum class GeometryType : uint8_t;
enum class VertexType : uint8_t;

struct ParquetMetadataFilePaths {
	MultiFileListScanData scan_data;
	shared_ptr<MultiFileList> file_list;
	mutex file_lock;

	bool NextFile(OpenFileInfo &result) {
		D_ASSERT(file_list);
		unique_lock<mutex> lock(file_lock);
		return file_list->Scan(scan_data, result);
	}

	FileExpandResult GetExpandResult() {
		D_ASSERT(file_list);
		unique_lock<mutex> lock(file_lock);
		return file_list->GetExpandResult();
	}
};

struct ParquetMetaDataBindData : public TableFunctionData {
	unique_ptr<ParquetMetadataFilePaths> file_paths;

	//! The file list is consumed by the first execution
	bool SupportStatementCache() const override {
		return false;
	}
};

struct ParquetBloomProbeBindData : public ParquetMetaDataBindData {
	string probe_column_name;
	Value probe_constant;
};

enum class ParquetMetadataOperatorType : uint8_t {
	META_DATA,
	SCHEMA,
	KEY_VALUE_META_DATA,
	FILE_META_DATA,
	BLOOM_PROBE,
	FULL_METADATA,
	PAGE_INDEX
};

class ParquetMetadataFileProcessor {
public:
	ParquetMetadataFileProcessor() = default;
	virtual ~ParquetMetadataFileProcessor() = default;
	void Initialize(ClientContext &context, ParquetReader &reader) {
		InitializeInternal(context, reader);
	}
	virtual void InitializeInternal(ClientContext &context, ParquetReader &reader) {};
	virtual idx_t TotalRowCount(ParquetReader &reader) = 0;
	virtual void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) = 0;
	virtual bool ForceFlush() {
		return false;
	}
};

class ParquetMetaDataOperator {
public:
	template <ParquetMetadataOperatorType OP_TYPE>
	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names);
	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input);
	template <ParquetMetadataOperatorType OP_TYPE>
	static unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
	                                                     GlobalTableFunctionState *global_state);
	static void Function(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);
	static double Progress(ClientContext &context, const FunctionData *bind_data_p,
	                       const GlobalTableFunctionState *global_state);

	template <ParquetMetadataOperatorType OP_TYPE>
	static void BindSchema(vector<LogicalType> &return_types, vector<Identifier> &names);

	static OperatorPartitionData GetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input);
};

struct ParquetMetadataGlobalState : public GlobalTableFunctionState {
	ParquetMetadataGlobalState(unique_ptr<ParquetMetadataFilePaths> file_paths_p, ClientContext &context)
	    : file_paths(std::move(file_paths_p)) {
		auto expand_result = file_paths->GetExpandResult();
		if (expand_result == FileExpandResult::MULTIPLE_FILES) {
			max_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
		} else {
			max_threads = 1;
		}
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}

	bool NextFile(ClientContext &context, OpenFileInfo &result) {
		return file_paths->NextFile(result);
	}

	double GetProgress() const {
		// Not the most accurate, instantly assumes all files are done and equal
		unique_lock<mutex> lock(file_paths->file_lock);
		return static_cast<double>(file_paths->scan_data.current_file_idx) /
		       static_cast<double>(file_paths->file_list->GetTotalFileCount());
	}

	unique_ptr<ParquetMetadataFilePaths> file_paths;
	idx_t max_threads;
	mutex lock;
	idx_t current_file = 0;
};

struct ParquetMetadataLocalState : public LocalTableFunctionState {
	unique_ptr<ParquetReader> reader;
	unique_ptr<ParquetMetadataFileProcessor> processor;
	bool file_exhausted = true;
	idx_t row_idx = 0;
	idx_t total_rows = 0;
	optional_idx file_idx;

	void Initialize(ClientContext &context, OpenFileInfo &file_info, idx_t next_file_idx) {
		ParquetOptions parquet_options(context);
		reader = make_uniq<ParquetReader>(context, file_info, parquet_options);
		processor->Initialize(context, *reader);
		total_rows = processor->TotalRowCount(*reader);
		row_idx = 0;
		file_idx = next_file_idx;
		file_exhausted = false;
	}
};

template <class T>
static string ConvertParquetElementToString(T &&entry) {
	duckdb::stringstream ss;
	ss << entry;
	return ss.str();
}

template <class T>
static string PrintParquetElementToString(T &&entry) {
	duckdb::stringstream ss;
	entry.printTo(ss);
	return ss.str();
}

template <class T>
static Value ParquetElementString(T &&value, bool is_set) {
	if (!is_set) {
		return Value();
	}
	return Value(ConvertParquetElementToString(value));
}

static Value ParquetElementStringVal(const string &value, bool is_set) {
	if (!is_set) {
		return Value();
	}
	return Value(value);
}

template <class T>
static Value ParquetElementInteger(T &&value, bool is_iset) {
	if (!is_iset) {
		return Value();
	}
	return Value::INTEGER(value);
}

template <class T>
static Value ParquetElementBigint(T &&value, bool is_iset) {
	if (!is_iset) {
		return Value();
	}
	return Value::BIGINT(value);
}

static Value ParquetElementBoolean(bool value, bool is_iset) {
	if (!is_iset) {
		return Value();
	}
	return Value::BOOLEAN(value);
}

//===--------------------------------------------------------------------===//
// Row Group Meta Data
//===--------------------------------------------------------------------===//

class ParquetRowGroupMetadataProcessor : public ParquetMetadataFileProcessor {
public:
	void InitializeInternal(ClientContext &context, ParquetReader &reader) override;
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;

private:
	vector<ParquetColumnSchema> column_schemas;
};

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::META_DATA>(vector<LogicalType> &return_types,
                                                                                 vector<Identifier> &names) {
	names.emplace_back("file_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("row_group_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("row_group_num_rows");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("row_group_num_columns");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("row_group_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("file_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("num_values");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("path_in_schema");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("stats_min");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("stats_max");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("stats_null_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("stats_distinct_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("stats_min_value");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("stats_max_value");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("compression");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("encodings");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("index_page_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("dictionary_page_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("data_page_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("total_compressed_size");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("total_uncompressed_size");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("key_value_metadata");
	return_types.emplace_back(LogicalType::MAP(LogicalType::BLOB, LogicalType::BLOB));

	names.emplace_back("bloom_filter_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("bloom_filter_length");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("min_is_exact");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("max_is_exact");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("row_group_compressed_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("geo_bbox");
	return_types.emplace_back(LogicalType::STRUCT({
	    {"xmin", LogicalType::DOUBLE},
	    {"xmax", LogicalType::DOUBLE},
	    {"ymin", LogicalType::DOUBLE},
	    {"ymax", LogicalType::DOUBLE},
	    {"zmin", LogicalType::DOUBLE},
	    {"zmax", LogicalType::DOUBLE},
	    {"mmin", LogicalType::DOUBLE},
	    {"mmax", LogicalType::DOUBLE},
	}));

	names.emplace_back("geo_types");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("encoding_stats");
	return_types.emplace_back(LogicalType::LIST(LogicalType::STRUCT({
	    {"page_type", LogicalType::VARCHAR},
	    {"encoding", LogicalType::VARCHAR},
	    {"count", LogicalType::INTEGER},
	})));

	names.emplace_back("stats_nan_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_index_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_index_length");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("offset_index_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("offset_index_length");
	return_types.emplace_back(LogicalType::BIGINT);
}

static Value ConvertParquetStats(const LogicalType &type, const ParquetColumnSchema &schema_ele, bool stats_is_set,
                                 const std::string &stats) {
	if (!stats_is_set) {
		return Value(LogicalType::VARCHAR);
	}
	return ParquetStatisticsUtils::ConvertValue(type, schema_ele, stats).DefaultCastAs(LogicalType::VARCHAR);
}

static Value ConvertParquetGeoStatsBBOX(const duckdb_parquet::GeospatialStatistics &stats) {
	if (!stats.__isset.bbox) {
		return Value(LogicalType::STRUCT({
		    {"xmin", LogicalType::DOUBLE},
		    {"xmax", LogicalType::DOUBLE},
		    {"ymin", LogicalType::DOUBLE},
		    {"ymax", LogicalType::DOUBLE},
		    {"zmin", LogicalType::DOUBLE},
		    {"zmax", LogicalType::DOUBLE},
		    {"mmin", LogicalType::DOUBLE},
		    {"mmax", LogicalType::DOUBLE},
		}));
	}

	return Value::STRUCT({
	    {"xmin", Value::DOUBLE(stats.bbox.xmin)},
	    {"xmax", Value::DOUBLE(stats.bbox.xmax)},
	    {"ymin", Value::DOUBLE(stats.bbox.ymin)},
	    {"ymax", Value::DOUBLE(stats.bbox.ymax)},
	    {"zmin", stats.bbox.__isset.zmin ? Value::DOUBLE(stats.bbox.zmin) : Value(LogicalTypeId::DOUBLE)},
	    {"zmax", stats.bbox.__isset.zmax ? Value::DOUBLE(stats.bbox.zmax) : Value(LogicalTypeId::DOUBLE)},
	    {"mmin", stats.bbox.__isset.mmin ? Value::DOUBLE(stats.bbox.mmin) : Value(LogicalTypeId::DOUBLE)},
	    {"mmax", stats.bbox.__isset.mmax ? Value::DOUBLE(stats.bbox.mmax) : Value(LogicalTypeId::DOUBLE)},
	});
}

static Value ConvertParquetEncodingStats(const duckdb_parquet::ColumnMetaData &col_meta) {
	auto stat_type = LogicalType::STRUCT({
	    {"page_type", LogicalType::VARCHAR},
	    {"encoding", LogicalType::VARCHAR},
	    {"count", LogicalType::INTEGER},
	});
	if (!col_meta.__isset.encoding_stats) {
		return Value(LogicalType::LIST(stat_type));
	}
	vector<Value> stats;
	stats.reserve(col_meta.encoding_stats.size());
	for (auto &entry : col_meta.encoding_stats) {
		stats.push_back(Value::STRUCT({
		    {"page_type", Value(ConvertParquetElementToString(entry.page_type))},
		    {"encoding", Value(ConvertParquetElementToString(entry.encoding))},
		    {"count", Value::INTEGER(entry.count)},
		}));
	}
	return Value::LIST(stat_type, std::move(stats));
}

static Value ConvertParquetGeoStatsTypes(const duckdb_parquet::GeospatialStatistics &stats) {
	if (!stats.__isset.geospatial_types) {
		return Value(LogicalType::LIST(LogicalType::VARCHAR));
	}
	vector<Value> types;
	types.reserve(stats.geospatial_types.size());

	GeometryTypeSet type_set = GeometryTypeSet::Empty();
	for (auto &type : stats.geospatial_types) {
		const auto geom_type = (type % 1000);
		const auto vert_type = (type / 1000);
		if (geom_type < 1 || geom_type > 7) {
			throw InvalidInputException("Unsupported geometry type in Parquet geo metadata");
		}
		if (vert_type < 0 || vert_type > 3) {
			throw InvalidInputException("Unsupported geometry vertex type in Parquet geo metadata");
		}
		type_set.Add(static_cast<GeometryType>(geom_type), static_cast<VertexType>(vert_type));
	}

	for (auto &type_name : type_set.ToString(true)) {
		types.push_back(Value(type_name));
	}
	return Value::LIST(LogicalType::VARCHAR, types);
}

void ParquetRowGroupMetadataProcessor::InitializeInternal(ClientContext &context, ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	column_schemas.clear();
	for (idx_t schema_idx = 0; schema_idx < meta_data->schema.size(); schema_idx++) {
		auto &schema_element = meta_data->schema[schema_idx];
		if (schema_element.num_children > 0) {
			continue;
		}
		ParquetColumnSchema column_schema;
		column_schema.type = reader.DeriveLogicalType(schema_element, column_schema);
		column_schemas.push_back(std::move(column_schema));
	}
}

idx_t ParquetRowGroupMetadataProcessor::TotalRowCount(ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	return meta_data->row_groups.size() * column_schemas.size();
}

void ParquetRowGroupMetadataProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx,
                                               ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	idx_t col_idx = row_idx % column_schemas.size();
	idx_t row_group_idx = row_idx / column_schemas.size();

	auto &row_group = meta_data->row_groups[row_group_idx];

	auto &column = row_group.columns[col_idx];
	auto &column_schema = column_schemas[col_idx];
	auto &col_meta = column.meta_data;
	auto &stats = col_meta.statistics;
	auto &column_type = column_schema.type;

	// file_name
	output[0].get().Append(reader.file.path);
	// row_group_id
	output[1].get().Append(Value::BIGINT(UnsafeNumericCast<int64_t>(row_group_idx)));
	// row_group_num_rows
	output[2].get().Append(Value::BIGINT(row_group.num_rows));
	// row_group_num_columns
	output[3].get().Append(Value::BIGINT(UnsafeNumericCast<int64_t>(row_group.columns.size())));
	// row_group_bytes
	output[4].get().Append(Value::BIGINT(row_group.total_byte_size));
	// column_id
	output[5].get().Append(Value::BIGINT(UnsafeNumericCast<int64_t>(col_idx)));
	// file_offset
	output[6].get().Append(ParquetElementBigint(column.file_offset, row_group.__isset.file_offset));
	// num_values
	output[7].get().Append(Value::BIGINT(col_meta.num_values));
	// path_in_schema
	output[8].get().Append(StringUtil::Join(col_meta.path_in_schema, ", ",
	                                        [](const duckdb_parquet::ParquetString &part) { return string(part); }));
	// type
	output[9].get().Append(ConvertParquetElementToString(col_meta.type));
	// stats_min
	output[10].get().Append(ConvertParquetStats(column_type, column_schema, stats.__isset.min, stats.min));
	// stats_max
	output[11].get().Append(ConvertParquetStats(column_type, column_schema, stats.__isset.max, stats.max));
	// stats_null_count
	output[12].get().Append(ParquetElementBigint(stats.null_count, stats.__isset.null_count));
	// stats_distinct_count
	output[13].get().Append(ParquetElementBigint(stats.distinct_count, stats.__isset.distinct_count));
	// stats_min_value
	output[14].get().Append(ConvertParquetStats(column_type, column_schema, stats.__isset.min_value, stats.min_value));
	// stats_max_value
	output[15].get().Append(ConvertParquetStats(column_type, column_schema, stats.__isset.max_value, stats.max_value));
	// compression
	output[16].get().Append(ConvertParquetElementToString(col_meta.codec));
	// encodings
	vector<string> encoding_string;
	encoding_string.reserve(col_meta.encodings.size());
	for (auto &encoding : col_meta.encodings) {
		encoding_string.push_back(ConvertParquetElementToString(encoding));
	}
	output[17].get().Append(Value(StringUtil::Join(encoding_string, ", ")));
	// index_page_offset
	output[18].get().Append(ParquetElementBigint(col_meta.index_page_offset, col_meta.__isset.index_page_offset));
	// dictionary_page_offset
	output[19].get().Append(
	    ParquetElementBigint(col_meta.dictionary_page_offset, col_meta.__isset.dictionary_page_offset));
	// data_page_offset
	output[20].get().Append(Value::BIGINT(col_meta.data_page_offset));
	// total_compressed_size
	output[21].get().Append(Value::BIGINT(col_meta.total_compressed_size));
	// total_uncompressed_size
	output[22].get().Append(Value::BIGINT(col_meta.total_uncompressed_size));
	// key_value_metadata
	vector<Value> map_keys, map_values;
	for (auto &entry : col_meta.key_value_metadata) {
		map_keys.push_back(Value::BLOB_RAW(string(entry.key)));
		map_values.push_back(Value::BLOB_RAW(string(entry.value)));
	}
	output[23].get().Append(
	    Value::MAP(LogicalType::BLOB, LogicalType::BLOB, std::move(map_keys), std::move(map_values)));
	// bloom_filter_offset
	output[24].get().Append(ParquetElementBigint(col_meta.bloom_filter_offset, col_meta.__isset.bloom_filter_offset));
	// bloom_filter_length
	output[25].get().Append(ParquetElementBigint(col_meta.bloom_filter_length, col_meta.__isset.bloom_filter_length));
	// min_is_exact
	output[26].get().Append(ParquetElementBoolean(stats.is_min_value_exact, stats.__isset.is_min_value_exact));
	// max_is_exact
	output[27].get().Append(ParquetElementBoolean(stats.is_max_value_exact, stats.__isset.is_max_value_exact));
	// row_group_compressed_bytes
	output[28].get().Append(
	    ParquetElementBigint(row_group.total_compressed_size, row_group.__isset.total_compressed_size));
	// geo_stats_bbox, LogicalType::STRUCT(...)
	output[29].get().Append(ConvertParquetGeoStatsBBOX(col_meta.geospatial_statistics));

	// geo_stats_types, LogicalType::LIST(LogicalType::VARCHAR)
	output[30].get().Append(ConvertParquetGeoStatsTypes(col_meta.geospatial_statistics));

	// encoding_stats, LogicalType::LIST(LogicalType::STRUCT(...))
	output[31].get().Append(ConvertParquetEncodingStats(col_meta));
	// stats_nan_count
	output[32].get().Append(ParquetElementBigint(stats.nan_count, stats.__isset.nan_count));
	// column_index_offset
	output[33].get().Append(ParquetElementBigint(column.column_index_offset, column.__isset.column_index_offset));
	// column_index_length
	output[34].get().Append(ParquetElementBigint(column.column_index_length, column.__isset.column_index_length));
	// offset_index_offset
	output[35].get().Append(ParquetElementBigint(column.offset_index_offset, column.__isset.offset_index_offset));
	// offset_index_length
	output[36].get().Append(ParquetElementBigint(column.offset_index_length, column.__isset.offset_index_length));
}

//===--------------------------------------------------------------------===//
// Schema Data
//===--------------------------------------------------------------------===//

class ParquetSchemaProcessor : public ParquetMetadataFileProcessor {
public:
	void InitializeInternal(ClientContext &context, ParquetReader &reader) override;
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;

private:
	vector<Value> element_types;
};

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::SCHEMA>(vector<LogicalType> &return_types,
                                                                              vector<Identifier> &names) {
	names.emplace_back("file_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("type_length");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("repetition_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("num_children");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("converted_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("scale");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("precision");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("field_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("logical_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("duckdb_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("column_id");
	return_types.emplace_back(LogicalType::BIGINT);
}

static Value ParquetLogicalTypeToString(const duckdb_parquet::LogicalType &type, bool is_set) {
	if (!is_set) {
		return Value();
	}
	if (type.__isset.STRING) {
		return Value(PrintParquetElementToString(type.STRING));
	}
	if (type.__isset.MAP) {
		return Value(PrintParquetElementToString(type.MAP));
	}
	if (type.__isset.LIST) {
		return Value(PrintParquetElementToString(type.LIST));
	}
	if (type.__isset.ENUM) {
		return Value(PrintParquetElementToString(type.ENUM));
	}
	if (type.__isset.DECIMAL) {
		return Value(PrintParquetElementToString(type.DECIMAL));
	}
	if (type.__isset.DATE) {
		return Value(PrintParquetElementToString(type.DATE));
	}
	if (type.__isset.TIME) {
		return Value(PrintParquetElementToString(type.TIME));
	}
	if (type.__isset.TIMESTAMP) {
		return Value(PrintParquetElementToString(type.TIMESTAMP));
	}
	if (type.__isset.INTEGER) {
		return Value(PrintParquetElementToString(type.INTEGER));
	}
	if (type.__isset.UNKNOWN) {
		return Value(PrintParquetElementToString(type.UNKNOWN));
	}
	if (type.__isset.JSON) {
		return Value(PrintParquetElementToString(type.JSON));
	}
	if (type.__isset.BSON) {
		return Value(PrintParquetElementToString(type.BSON));
	}
	if (type.__isset.UUID) {
		return Value(PrintParquetElementToString(type.UUID));
	}
	if (type.__isset.FLOAT16) {
		return Value(PrintParquetElementToString(type.FLOAT16));
	}
	if (type.__isset.GEOMETRY) {
		return Value(PrintParquetElementToString(type.GEOMETRY));
	}
	if (type.__isset.GEOGRAPHY) {
		return Value(PrintParquetElementToString(type.GEOGRAPHY));
	}
	return Value();
}

static void CollectElementTypes(const ParquetColumnSchema &schema, vector<Value> &element_types) {
	// a group collapsed into its only child has the type of that child
	for (auto &index : {schema.parent_schema_index, schema.schema_index}) {
		if (index.IsValid() && index.GetIndex() < element_types.size() && element_types[index.GetIndex()].IsNull()) {
			element_types[index.GetIndex()] = Value(schema.type.ToString());
		}
	}
	for (auto &child : schema.children) {
		CollectElementTypes(child, element_types);
	}
}

void ParquetSchemaProcessor::InitializeInternal(ClientContext &context, ParquetReader &reader) {
	element_types.assign(reader.GetFileMetadata()->schema.size(), Value());
	for (auto &column : reader.root_schema->children) {
		CollectElementTypes(column, element_types);
	}
}

idx_t ParquetSchemaProcessor::TotalRowCount(ParquetReader &reader) {
	return reader.GetFileMetadata()->schema.size();
}

void ParquetSchemaProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	const auto &column = meta_data->schema[row_idx];

	// file_name
	output[0].get().Append(reader.file.path);
	// name
	output[1].get().Append(string(column.name));
	// type
	output[2].get().Append(ParquetElementString(column.type, column.__isset.type));
	// type_length
	output[3].get().Append(ParquetElementInteger(column.type_length, column.__isset.type_length));
	// repetition_type
	output[4].get().Append(ParquetElementString(column.repetition_type, column.__isset.repetition_type));
	// num_children
	output[5].get().Append(ParquetElementBigint(column.num_children, column.__isset.num_children));
	// converted_type
	output[6].get().Append(ParquetElementString(column.converted_type, column.__isset.converted_type));
	// scale
	output[7].get().Append(ParquetElementBigint(column.scale, column.__isset.scale));
	// precision
	output[8].get().Append(ParquetElementBigint(column.precision, column.__isset.precision));
	// field_id
	output[9].get().Append(ParquetElementBigint(column.field_id, column.__isset.field_id));
	// logical_type
	output[10].get().Append(ParquetLogicalTypeToString(column.logicalType, column.__isset.logicalType));
	// duckdb_type
	output[11].get().Append(element_types[row_idx]);
	// column_id
	output[12].get().Append(Value::BIGINT(UnsafeNumericCast<int64_t>(row_idx)));
}

//===--------------------------------------------------------------------===//
// KV Meta Data
//===--------------------------------------------------------------------===//

class ParquetKeyValueMetadataProcessor : public ParquetMetadataFileProcessor {
public:
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;
};

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::KEY_VALUE_META_DATA>(
    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("file_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("key");
	return_types.emplace_back(LogicalType::BLOB);

	names.emplace_back("value");
	return_types.emplace_back(LogicalType::BLOB);
}

idx_t ParquetKeyValueMetadataProcessor::TotalRowCount(ParquetReader &reader) {
	return reader.GetFileMetadata()->key_value_metadata.size();
}

void ParquetKeyValueMetadataProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx,
                                               ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	auto &entry = meta_data->key_value_metadata[row_idx];

	output[0].get().Append(Value(reader.file.path));
	output[1].get().Append(Value::BLOB_RAW(string(entry.key)));
	output[2].get().Append(Value::BLOB_RAW(string(entry.value)));
}

//===--------------------------------------------------------------------===//
// File Meta Data
//===--------------------------------------------------------------------===//

class ParquetFileMetadataProcessor : public ParquetMetadataFileProcessor {
public:
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;
};

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::FILE_META_DATA>(vector<LogicalType> &return_types,
                                                                                      vector<Identifier> &names) {
	names.emplace_back("file_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("created_by");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("num_rows");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("num_row_groups");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("format_version");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("encryption_algorithm");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("footer_signing_key_metadata");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("file_size_bytes");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("footer_size");
	return_types.emplace_back(LogicalType::UBIGINT);

	names.emplace_back("column_orders");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
}

idx_t ParquetFileMetadataProcessor::TotalRowCount(ParquetReader &reader) {
	return 1;
}

void ParquetFileMetadataProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();

	// file_name
	output[0].get().Append(Value(reader.file.path));
	// created_by
	output[1].get().Append(ParquetElementStringVal(meta_data->created_by, meta_data->__isset.created_by));
	// num_rows
	output[2].get().Append(Value::BIGINT(meta_data->num_rows));
	// num_row_groups
	output[3].get().Append(Value::BIGINT(UnsafeNumericCast<int64_t>(meta_data->row_groups.size())));
	// format_version
	output[4].get().Append(Value::BIGINT(meta_data->version));
	// encryption_algorithm
	output[5].get().Append(
	    ParquetElementString(meta_data->encryption_algorithm, meta_data->__isset.encryption_algorithm));
	// footer_signing_key_metadata
	output[6].get().Append(ParquetElementStringVal(meta_data->footer_signing_key_metadata,
	                                               meta_data->__isset.footer_signing_key_metadata));
	// file_size_bytes
	output[7].get().Append(Value::UBIGINT(reader.GetHandle().GetFileSize()));
	// footer_size
	output[8].get().Append(Value::UBIGINT(reader.metadata->footer_size));
	// column_orders
	Value column_orders_value;
	if (meta_data->__isset.column_orders) {
		vector<Value> column_orders;
		column_orders.reserve(meta_data->column_orders.size());
		for (auto &column_order : meta_data->column_orders) {
			column_orders.push_back(Value(ConvertParquetElementToString(column_order)));
		}
		column_orders_value = Value::LIST(LogicalType::VARCHAR, column_orders);
	}
	output[9].get().Append(column_orders_value);
}

//===--------------------------------------------------------------------===//
// Bloom Probe
//===--------------------------------------------------------------------===//

class ParquetBloomProbeProcessor : public ParquetMetadataFileProcessor {
public:
	ParquetBloomProbeProcessor(const string &probe_column, const Value &probe_value);

	void InitializeInternal(ClientContext &context, ParquetReader &reader) override;
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;

private:
	string probe_column_name;
	Value probe_constant;
	optional_idx probe_column_idx;

	unique_ptr<duckdb_apache::thrift::protocol::TCompactProtocolT<ThriftFileTransport>> protocol;
	optional_ptr<Allocator> allocator;
	unique_ptr<ExpressionFilter> filter;
	optional<ParquetBloomFilterHashStrategy> hash_strategy;
};

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::BLOOM_PROBE>(vector<LogicalType> &return_types,
                                                                                   vector<Identifier> &names) {
	names.emplace_back("file_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("row_group_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("bloom_filter_excludes");
	return_types.emplace_back(LogicalType::BOOLEAN);
}

ParquetBloomProbeProcessor::ParquetBloomProbeProcessor(const string &probe_column, const Value &probe_value)
    : probe_column_name(probe_column), probe_constant(probe_value) {
}

void ParquetBloomProbeProcessor::InitializeInternal(ClientContext &context, ParquetReader &reader) {
	probe_column_idx = optional_idx::Invalid();

	for (idx_t column_idx = 0; column_idx < reader.columns.size(); column_idx++) {
		if (reader.columns[column_idx].name == probe_column_name) {
			probe_column_idx = column_idx;
			break;
		}
	}

	if (!probe_column_idx.IsValid()) {
		throw InvalidInputException("Column %s not found in %s", probe_column_name, reader.file.path);
	}

	auto transport = duckdb_base_std::make_shared<ThriftFileTransport>(context, reader.GetHandle(), false);
	protocol = make_uniq<duckdb_apache::thrift::protocol::TCompactProtocolT<ThriftFileTransport>>(std::move(transport));
	allocator = &BufferAllocator::Get(context);
	auto column_type = reader.GetColumns()[probe_column_idx.GetIndex()].type;
	filter = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL,
	                                                  probe_constant.CastAs(context, column_type));
	auto &column_schema = reader.root_schema->children[probe_column_idx.GetIndex()];
	hash_strategy =
	    ParquetStatisticsUtils::GetBloomFilterHashStrategy(column_schema, reader.GetIntervalBloomFilterVersion());
}

idx_t ParquetBloomProbeProcessor::TotalRowCount(ParquetReader &reader) {
	return reader.GetFileMetadata()->row_groups.size();
}

void ParquetBloomProbeProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	auto &row_group = meta_data->row_groups[row_idx];
	auto &column = row_group.columns[probe_column_idx.GetIndex()];

	D_ASSERT(!probe_constant.IsNull());

	auto &column_schema = reader.root_schema->children[probe_column_idx.GetIndex()];
	auto bloom_excludes =
	    hash_strategy && ParquetStatisticsUtils::BloomFilterExcludes(*filter, column.meta_data, *protocol, *allocator,
	                                                                 column_schema, *hash_strategy);

	output[0].get().Append(Value(reader.file.path));
	output[1].get().Append(Value::BIGINT(NumericCast<int64_t>(row_idx)));
	output[2].get().Append(Value::BOOLEAN(bloom_excludes));
}

//===--------------------------------------------------------------------===//
// Page Index
//===--------------------------------------------------------------------===//

//! One row per data page: the OffsetIndex (location, first row) and ColumnIndex (bounds, null count) entries
class ParquetPageIndexProcessor : public ParquetMetadataFileProcessor {
public:
	void InitializeInternal(ClientContext &context, ParquetReader &reader) override;
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;

private:
	struct ColumnChunkPageIndex {
		idx_t row_group_idx;
		idx_t column_idx;
		//! The first output row of this column chunk
		idx_t first_row;
		idx_t page_count;
		bool has_column_index = false;
		duckdb_parquet::ColumnIndex column_index;
		bool has_offset_index = false;
		duckdb_parquet::OffsetIndex offset_index;

		explicit ColumnChunkPageIndex(ArenaAllocator &arena) : column_index(arena), offset_index(arena) {
		}
	};

	vector<ParquetColumnSchema> column_schemas;
	//! Backs the Thrift page index objects of the file
	ArenaAllocator arena {Allocator::DefaultAllocator()};
	//! The column chunks that have a page index, in file order
	vector<ColumnChunkPageIndex> chunks;
	idx_t total_rows = 0;
	//! The chunk the previous row came from - rows are read in order
	idx_t current_chunk = 0;
};

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::PAGE_INDEX>(vector<LogicalType> &return_types,
                                                                                  vector<Identifier> &names) {
	names.emplace_back("file_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("row_group_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("path_in_schema");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("page_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("page_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("compressed_page_size");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("first_row_index");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("num_rows");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("null_page");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("min_value");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("max_value");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("null_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("boundary_order");
	return_types.emplace_back(LogicalType::VARCHAR);
}

void ParquetPageIndexProcessor::InitializeInternal(ClientContext &context, ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	column_schemas.clear();
	for (idx_t schema_idx = 0; schema_idx < meta_data->schema.size(); schema_idx++) {
		auto &schema_element = meta_data->schema[schema_idx];
		if (schema_element.num_children > 0) {
			continue;
		}
		ParquetColumnSchema column_schema;
		column_schema.type = reader.DeriveLogicalType(schema_element, column_schema);
		column_schemas.push_back(std::move(column_schema));
	}

	chunks.clear();
	total_rows = 0;
	current_chunk = 0;
	if (reader.parquet_options.encryption_config) {
		// the page index of an encrypted file is encrypted as well - not supported (yet)
		return;
	}
	auto transport = duckdb_base_std::make_shared<ThriftFileTransport>(context, reader.GetHandle(), false);
	auto &trans = *transport;
	duckdb_apache::thrift::protocol::TCompactProtocolT<ThriftFileTransport> protocol(std::move(transport));
	for (idx_t row_group_idx = 0; row_group_idx < meta_data->row_groups.size(); row_group_idx++) {
		auto &row_group = meta_data->row_groups[row_group_idx];
		// the indexes of a row group are stored next to each other: fetch them in one go
		idx_t index_begin = NumericLimits<idx_t>::Maximum();
		idx_t index_end = 0;
		for (auto &column : row_group.columns) {
			if (ParquetPageIndexUtils::HasColumnIndex(column)) {
				const auto begin = NumericCast<idx_t>(column.column_index_offset);
				index_begin = MinValue(index_begin, begin);
				index_end = MaxValue(index_end, begin + NumericCast<idx_t>(column.column_index_length));
			}
			if (ParquetPageIndexUtils::HasOffsetIndex(column)) {
				const auto begin = NumericCast<idx_t>(column.offset_index_offset);
				index_begin = MinValue(index_begin, begin);
				index_end = MaxValue(index_end, begin + NumericCast<idx_t>(column.offset_index_length));
			}
		}
		if (index_end == 0) {
			continue;
		}
		if (index_end > trans.GetSize()) {
			throw InvalidInputException(
			    "Failed to read file \"%s\": metadata is corrupt. Page index lies outside the file", reader.file.path);
		}
		trans.Prefetch(index_begin, index_end - index_begin);
		for (idx_t column_idx = 0; column_idx < row_group.columns.size(); column_idx++) {
			auto &column = row_group.columns[column_idx];
			ColumnChunkPageIndex chunk(arena);
			chunk.row_group_idx = row_group_idx;
			chunk.column_idx = column_idx;
			chunk.first_row = total_rows;
			chunk.page_count = 0;
			if (ParquetPageIndexUtils::HasOffsetIndex(column)) {
				ParquetPageIndexUtils::ReadOffsetIndex(protocol, column, chunk.offset_index);
				chunk.has_offset_index = true;
				chunk.page_count = chunk.offset_index.page_locations.size();
			}
			if (ParquetPageIndexUtils::HasColumnIndex(column)) {
				ParquetPageIndexUtils::ReadColumnIndex(protocol, column, chunk.column_index);
				chunk.has_column_index = true;
				chunk.page_count = MaxValue<idx_t>(chunk.page_count, chunk.column_index.null_pages.size());
			}
			if (chunk.page_count == 0) {
				continue;
			}
			total_rows += chunk.page_count;
			chunks.push_back(std::move(chunk));
		}
		trans.ClearPrefetch();
	}
}

idx_t ParquetPageIndexProcessor::TotalRowCount(ParquetReader &reader) {
	return total_rows;
}

void ParquetPageIndexProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) {
	auto meta_data = reader.GetFileMetadata();
	if (current_chunk >= chunks.size() || row_idx < chunks[current_chunk].first_row) {
		current_chunk = 0;
	}
	while (chunks[current_chunk].first_row + chunks[current_chunk].page_count <= row_idx) {
		current_chunk++;
	}
	auto &chunk = chunks[current_chunk];
	const auto page_idx = row_idx - chunk.first_row;
	auto &row_group = meta_data->row_groups[chunk.row_group_idx];
	auto &column = row_group.columns[chunk.column_idx];
	auto &column_schema = column_schemas[chunk.column_idx];

	// file_name
	output[0].get().Append(Value(reader.file.path));
	// row_group_id
	output[1].get().Append(Value::BIGINT(NumericCast<int64_t>(chunk.row_group_idx)));
	// column_id
	output[2].get().Append(Value::BIGINT(NumericCast<int64_t>(chunk.column_idx)));
	// path_in_schema
	output[3].get().Append(
	    Value(StringUtil::Join(column.meta_data.path_in_schema, ", ",
	                           [](const duckdb_parquet::ParquetString &part) { return string(part); })));
	// page_id
	output[4].get().Append(Value::BIGINT(NumericCast<int64_t>(page_idx)));

	const bool has_location = chunk.has_offset_index && page_idx < chunk.offset_index.page_locations.size();
	Value offset;
	Value compressed_page_size;
	Value first_row_index;
	Value num_rows;
	if (has_location) {
		auto &locations = chunk.offset_index.page_locations;
		auto &location = locations[page_idx];
		offset = Value::BIGINT(location.offset);
		compressed_page_size = Value::BIGINT(location.compressed_page_size);
		first_row_index = Value::BIGINT(location.first_row_index);
		const auto next_row =
		    page_idx + 1 < locations.size() ? locations[page_idx + 1].first_row_index : row_group.num_rows;
		num_rows = Value::BIGINT(next_row - location.first_row_index);
	}
	// page_offset
	output[5].get().Append(offset);
	// compressed_page_size
	output[6].get().Append(compressed_page_size);
	// first_row_index
	output[7].get().Append(first_row_index);
	// num_rows
	output[8].get().Append(num_rows);

	const bool has_bounds = chunk.has_column_index && page_idx < chunk.column_index.null_pages.size() &&
	                        page_idx < chunk.column_index.min_values.size() &&
	                        page_idx < chunk.column_index.max_values.size();
	Value null_page;
	Value min_value;
	Value max_value;
	Value null_count;
	Value boundary_order;
	if (has_bounds) {
		auto &column_index = chunk.column_index;
		const bool is_null_page = column_index.null_pages[page_idx];
		null_page = Value::BOOLEAN(is_null_page);
		min_value =
		    ConvertParquetStats(column_schema.type, column_schema, !is_null_page, column_index.min_values[page_idx]);
		max_value =
		    ConvertParquetStats(column_schema.type, column_schema, !is_null_page, column_index.max_values[page_idx]);
		if (column_index.__isset.null_counts && page_idx < column_index.null_counts.size()) {
			null_count = Value::BIGINT(column_index.null_counts[page_idx]);
		}
		boundary_order = Value(ConvertParquetElementToString(column_index.boundary_order));
	}
	// null_page
	output[9].get().Append(null_page);
	// min_value
	output[10].get().Append(min_value);
	// max_value
	output[11].get().Append(max_value);
	// null_count
	output[12].get().Append(null_count);
	// boundary_order
	output[13].get().Append(boundary_order);
}

//===--------------------------------------------------------------------===//
// Full Metadata
//===--------------------------------------------------------------------===//

class FullMetadataProcessor : public ParquetMetadataFileProcessor {
public:
	FullMetadataProcessor() = default;

	void InitializeInternal(ClientContext &context, ParquetReader &reader) override;
	idx_t TotalRowCount(ParquetReader &reader) override;
	void ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) override;
	bool ForceFlush() override {
		return true;
	}

private:
	void PopulateMetadata(ParquetMetadataFileProcessor &processor, Vector &output, ParquetReader &reader);

	ParquetFileMetadataProcessor file_processor;
	ParquetRowGroupMetadataProcessor row_group_processor;
	ParquetSchemaProcessor schema_processor;
	ParquetKeyValueMetadataProcessor kv_processor;
};

void FullMetadataProcessor::PopulateMetadata(ParquetMetadataFileProcessor &processor, Vector &output,
                                             ParquetReader &reader) {
	auto count = processor.TotalRowCount(reader);
	auto *result_data = FlatVector::GetDataMutable<list_entry_t>(output);
	auto &result_struct = ListVector::GetChildMutable(output);
	auto &result_struct_entries = StructVector::GetEntries(result_struct);

	ListVector::Reserve(output, count);

	auto output_idx = output.size();

	result_data[output_idx].offset = 0;
	result_data[output_idx].length = count;

	FlatVector::ValidityMutable(output).SetValid(output_idx);
	FlatVector::SetSize(output, output_idx + 1);

	vector<reference<Vector>> vectors;
	for (auto &entry : result_struct_entries) {
		vectors.push_back(std::ref(entry));
		entry.SetVectorType(VectorType::FLAT_VECTOR);
		auto &validity = FlatVector::ValidityMutable(entry);
		validity.Initialize(count);
	}
	for (idx_t i = 0; i < count; i++) {
		processor.ReadRow(vectors, i, reader);
	}
	ListVector::SetListSize(output, count);
}

template <>
void ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::FULL_METADATA>(vector<LogicalType> &return_types,
                                                                                     vector<Identifier> &names) {
	names.emplace_back("parquet_file_metadata");
	vector<LogicalType> file_meta_types;
	vector<Identifier> file_meta_names;
	ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::FILE_META_DATA>(file_meta_types, file_meta_names);
	child_list_t<LogicalType> file_meta_children;
	for (idx_t i = 0; i < file_meta_types.size(); i++) {
		file_meta_children.emplace_back(make_pair(file_meta_names[i].GetIdentifierName(), file_meta_types[i]));
	}
	return_types.emplace_back(LogicalType::LIST(LogicalType::STRUCT(std::move(file_meta_children))));

	names.emplace_back("parquet_metadata");
	vector<LogicalType> row_group_types;
	vector<Identifier> row_group_names;
	ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::META_DATA>(row_group_types, row_group_names);
	child_list_t<LogicalType> row_group_children;
	for (idx_t i = 0; i < row_group_types.size(); i++) {
		row_group_children.emplace_back(make_pair(row_group_names[i].GetIdentifierName(), row_group_types[i]));
	}
	return_types.emplace_back(LogicalType::LIST(LogicalType::STRUCT(std::move(row_group_children))));

	names.emplace_back("parquet_schema");
	vector<LogicalType> schema_types;
	vector<Identifier> schema_names;
	ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::SCHEMA>(schema_types, schema_names);
	child_list_t<LogicalType> schema_children;
	for (idx_t i = 0; i < schema_types.size(); i++) {
		schema_children.emplace_back(make_pair(schema_names[i].GetIdentifierName(), schema_types[i]));
	}
	return_types.emplace_back(LogicalType::LIST(LogicalType::STRUCT(std::move(schema_children))));

	names.emplace_back("parquet_kv_metadata");
	vector<LogicalType> kv_types;
	vector<Identifier> kv_names;
	ParquetMetaDataOperator::BindSchema<ParquetMetadataOperatorType::KEY_VALUE_META_DATA>(kv_types, kv_names);
	child_list_t<LogicalType> kv_children;
	for (idx_t i = 0; i < kv_types.size(); i++) {
		kv_children.emplace_back(make_pair(kv_names[i].GetIdentifierName(), kv_types[i]));
	}
	return_types.emplace_back(LogicalType::LIST(LogicalType::STRUCT(std::move(kv_children))));
}

void FullMetadataProcessor::InitializeInternal(ClientContext &context, ParquetReader &reader) {
	file_processor.Initialize(context, reader);
	row_group_processor.Initialize(context, reader);
	schema_processor.Initialize(context, reader);
	kv_processor.Initialize(context, reader);
}

idx_t FullMetadataProcessor::TotalRowCount(ParquetReader &reader) {
	return 1;
}

void FullMetadataProcessor::ReadRow(vector<reference<Vector>> &output, idx_t row_idx, ParquetReader &reader) {
	PopulateMetadata(file_processor, output[0].get(), reader);
	PopulateMetadata(row_group_processor, output[1].get(), reader);
	PopulateMetadata(schema_processor, output[2].get(), reader);
	PopulateMetadata(kv_processor, output[3].get(), reader);
}

//===--------------------------------------------------------------------===//
// Template Function Implementation
//===--------------------------------------------------------------------===//

template <ParquetMetadataOperatorType OP_TYPE>
unique_ptr<FunctionData> ParquetMetaDataOperator::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	// Extract file paths from input using MultiFileReader (handles both single files and arrays)
	auto multi_file_reader = MultiFileReader::CreateDefault("ParquetMetadata");
	auto glob_input = FileGlobInput(FileGlobOptions::FALLBACK_GLOB, "parquet");

	auto result = make_uniq<ParquetMetaDataBindData>();
	// Bind schema based on operation type
	if (OP_TYPE == ParquetMetadataOperatorType::BLOOM_PROBE) {
		auto probe_bind_data = make_uniq<ParquetBloomProbeBindData>();
		D_ASSERT(input.inputs.size() == 3);
		if (input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
			throw InvalidInputException("Can't have NULL parameters for parquet_bloom_probe");
		}
		probe_bind_data->probe_column_name = input.inputs[1].CastAs(context, LogicalType::VARCHAR).GetValue<string>();
		probe_bind_data->probe_constant = input.inputs[2];
		result = std::move(probe_bind_data);
	}

	result->file_paths = make_uniq<ParquetMetadataFilePaths>();
	result->file_paths->file_list = multi_file_reader->CreateFileList(context, input.inputs[0], glob_input);
	D_ASSERT(!result->file_paths->file_list->IsEmpty());
	result->file_paths->file_list->InitializeScan(result->file_paths->scan_data);

	BindSchema<OP_TYPE>(return_types, names);

	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ParquetMetaDataOperator::InitGlobal(ClientContext &context,
                                                                         TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->CastNoConst<ParquetMetaDataBindData>();
	return make_uniq<ParquetMetadataGlobalState>(std::move(bind_data.file_paths), context);
}

template <ParquetMetadataOperatorType OP_TYPE>
unique_ptr<LocalTableFunctionState> ParquetMetaDataOperator::InitLocal(ExecutionContext &context,
                                                                       TableFunctionInitInput &input,
                                                                       GlobalTableFunctionState *global_state) {
	auto &bind_data = input.bind_data->Cast<ParquetMetaDataBindData>();
	auto res = make_uniq<ParquetMetadataLocalState>();
	switch (OP_TYPE) {
	case ParquetMetadataOperatorType::META_DATA:
		res->processor = make_uniq<ParquetRowGroupMetadataProcessor>();
		break;
	case ParquetMetadataOperatorType::SCHEMA:
		res->processor = make_uniq<ParquetSchemaProcessor>();
		break;
	case ParquetMetadataOperatorType::KEY_VALUE_META_DATA:
		res->processor = make_uniq<ParquetKeyValueMetadataProcessor>();
		break;
	case ParquetMetadataOperatorType::FILE_META_DATA:
		res->processor = make_uniq<ParquetFileMetadataProcessor>();
		break;
	case ParquetMetadataOperatorType::BLOOM_PROBE: {
		const auto &probe_bind_data = static_cast<const ParquetBloomProbeBindData &>(bind_data);
		res->processor =
		    make_uniq<ParquetBloomProbeProcessor>(probe_bind_data.probe_column_name, probe_bind_data.probe_constant);
		break;
	}
	case ParquetMetadataOperatorType::FULL_METADATA: {
		res->processor = make_uniq<FullMetadataProcessor>();
		break;
	}
	case ParquetMetadataOperatorType::PAGE_INDEX:
		res->processor = make_uniq<ParquetPageIndexProcessor>();
		break;
	default:
		throw InternalException("Unsupported ParquetMetadataOperatorType");
	}
	return unique_ptr_cast<LocalTableFunctionState, ParquetMetadataLocalState>(std::move(res));
}

void ParquetMetaDataOperator::Function(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &global_state = data_p.global_state->Cast<ParquetMetadataGlobalState>();
	auto &local_state = data_p.local_state->Cast<ParquetMetadataLocalState>();

	idx_t output_count = 0;

	vector<reference<Vector>> output_vectors;
	for (idx_t i = 0; i < output.ColumnCount(); i++) {
		output_vectors.push_back(std::ref(output.data[i]));
	}

	while (output_count < STANDARD_VECTOR_SIZE) {
		// Check if we need a new file
		if (local_state.file_exhausted) {
			if (output_count > 0) {
				// we already have rows - emit them first
				break;
			}
			idx_t next_file_idx;
			OpenFileInfo next_file;
			{
				lock_guard<mutex> guard(global_state.lock);
				if (!global_state.file_paths->NextFile(next_file)) {
					break; // No more files to process
				}
				next_file_idx = global_state.current_file++;
			}

			local_state.Initialize(context, next_file, next_file_idx);
		}

		idx_t left_in_vector = STANDARD_VECTOR_SIZE - output_count;
		idx_t left_in_file = local_state.total_rows - local_state.row_idx;
		idx_t rows_to_output = 0;
		if (left_in_file <= left_in_vector) {
			local_state.file_exhausted = true;
			rows_to_output = left_in_file;
		} else {
			rows_to_output = left_in_vector;
		}

		for (idx_t i = 0; i < rows_to_output; ++i) {
			local_state.processor->ReadRow(output_vectors, local_state.row_idx + i, *local_state.reader);
		}
		output_count += rows_to_output;
		local_state.row_idx += rows_to_output;

		if (local_state.processor->ForceFlush()) {
			break;
		}
	}
}

OperatorPartitionData ParquetMetaDataOperator::GetPartitionData(ClientContext &context,
                                                                TableFunctionGetPartitionInput &input) {
	auto &local_state = input.local_state->Cast<ParquetMetadataLocalState>();
	return OperatorPartitionData(local_state.file_idx.GetIndex());
}

double ParquetMetaDataOperator::Progress(ClientContext &context, const FunctionData *bind_data_p,
                                         const GlobalTableFunctionState *global_state) {
	auto &global_data = global_state->Cast<ParquetMetadataGlobalState>();
	return global_data.GetProgress() * 100.0;
}

ParquetMetaDataFunction::ParquetMetaDataFunction()
    : TableFunction("parquet_metadata", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::META_DATA>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::META_DATA>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}

ParquetSchemaFunction::ParquetSchemaFunction()
    : TableFunction("parquet_schema", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::SCHEMA>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::SCHEMA>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}

ParquetKeyValueMetadataFunction::ParquetKeyValueMetadataFunction()
    : TableFunction("parquet_kv_metadata", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::KEY_VALUE_META_DATA>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::KEY_VALUE_META_DATA>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}

ParquetFileMetadataFunction::ParquetFileMetadataFunction()
    : TableFunction("parquet_file_metadata", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::FILE_META_DATA>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::FILE_META_DATA>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}

ParquetBloomProbeFunction::ParquetBloomProbeFunction()
    : TableFunction("parquet_bloom_probe",
                    FunctionSignature()
                        .AddPositionalOnly("path", LogicalType::VARCHAR)
                        .AddPositionalOnly("column_name", LogicalType::VARCHAR)
                        .AddPositionalOnly("value", LogicalType::ANY),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::BLOOM_PROBE>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::BLOOM_PROBE>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}

ParquetPageIndexFunction::ParquetPageIndexFunction()
    : TableFunction("parquet_page_index", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::PAGE_INDEX>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::PAGE_INDEX>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}

ParquetFullMetadataFunction::ParquetFullMetadataFunction()
    : TableFunction("parquet_full_metadata", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
                    ParquetMetaDataOperator::Function,
                    ParquetMetaDataOperator::Bind<ParquetMetadataOperatorType::FULL_METADATA>,
                    ParquetMetaDataOperator::InitGlobal,
                    ParquetMetaDataOperator::InitLocal<ParquetMetadataOperatorType::FULL_METADATA>) {
	table_scan_progress = ParquetMetaDataOperator::Progress;
	get_partition_data = ParquetMetaDataOperator::GetPartitionData;
}
} // namespace duckdb
