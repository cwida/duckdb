#include "parquet_extension.hpp"

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "duckdb/parser/expression/constant_expression.hpp"
#include "parquet_geometry.hpp"
#include "parquet_crypto.hpp"
#include "parquet_metadata.hpp"
#include "parquet_writer.hpp"
#include "parquet_shredding.hpp"
#include "zstd_file_system.hpp"
#include "writer/primitive_column_writer.hpp"
#include "writer/variant_column_writer.hpp"
#include "reader/variant_column_reader.hpp"

#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "parquet_multi_file_info.hpp"
#include "column_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/copy_option_mode.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "parquet_field_id.hpp"
#include "parquet_types.h"
#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"

namespace duckdb {
class ClientContext;
class DataChunk;
class PhysicalOperator;

struct ParquetWriteBindData : public TableFunctionData {
	vector<LogicalType> sql_types;
	vector<string> column_names;
	duckdb_parquet::CompressionCodec::type codec = duckdb_parquet::CompressionCodec::SNAPPY;
	vector<pair<string, string>> kv_metadata;
	idx_t row_group_size = DEFAULT_ROW_GROUP_SIZE;
	idx_t row_group_size_bytes = NumericLimits<idx_t>::Maximum();

	//! Encryption configuration
	shared_ptr<ParquetEncryptionConfig> encryption_config;

	//! After how many distinct values should we abandon dictionary compression and bloom filters?
	//! Defaults to 1/5th of the row group size if unset (in templated_column_writer.hpp)
	//! This needs to be set dynamically because row groups can be much smaller than "row_group_size" set here,
	//! e.g., due to less data or row_group_size_bytes
	optional_idx dictionary_size_limit;

	//! This is huge but we grow it starting from 1 MB
	idx_t string_dictionary_page_size_limit = PrimitiveColumnWriter::MAX_UNCOMPRESSED_DICT_PAGE_SIZE;

	//! The maximum uncompressed size of a data page
	//! 0 when not set by the user: the default then depends on whether the page index is written
	idx_t data_page_size_limit = 0;

	bool enable_bloom_filters = true;
	bool write_page_index = false;
	//! What false positive rate are we willing to accept for bloom filters
	double bloom_filter_false_positive_ratio = 0.01;
	idx_t bloom_filter_fallback_max_bytes = ParquetBloomFilterMemory::DEFAULT_LIMIT;

	//! After how many row groups to rotate to a new file
	optional_idx row_groups_per_file;

	ChildFieldIDs field_ids;
	ShreddingType shredding_types;
	//! The compression level, higher value is more
	int64_t compression_level = ZStdFileSystem::DefaultCompressionLevel();
	//! Per-column NOT NULL flags
	vector<bool> not_null_columns;

	//! Which encodings to include when writing
	ParquetVersion parquet_version = ParquetVersion::V1;

	//! Which geo-parquet version to use when writing
	GeoParquetVersion geoparquet_version = GeoParquetVersion::V1;

	//! Whether Parquet timestamp columns should be written as INT96 for Impala / Spark compatibility
	bool write_timestamp_as_int96 = false;

	//! Whether TIMESTAMP (without timezone) should be marked as adjusted to UTC in the Parquet schema
	TimeStampIsAdjustedToUTC timestamp_is_adjusted_to_utc = TimeStampIsAdjustedToUTC::AUTO;
};

ParquetWriteLocalState::ParquetWriteLocalState(ClientContext &context, const vector<LogicalType> &types)
    : buffer(context, types) {
	buffer.SetPartitionIndex(0); // Makes the buffer manager less likely to spill this data
	buffer.InitializeAppend(append_state);
}

static void ParquetListCopyOptions(ClientContext &context, CopyOptionsInput &input) {
	auto &copy_options = input.options;
	copy_options["compression"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["codec"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["field_ids"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	copy_options["kv_metadata"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	copy_options["encryption_config"] = CopyOption(LogicalType::ANY, CopyOptionMode::READ_WRITE);
	copy_options["dictionary_size_limit"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["string_dictionary_page_size_limit"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["data_page_size_limit"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["bloom_filter_false_positive_ratio"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	copy_options["debug_use_openssl"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_WRITE);
	copy_options["write_bloom_filter"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	copy_options["bloom_filter_fallback_max_bytes"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	copy_options["write_page_index"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	copy_options["compression_level"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["parquet_version"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	copy_options["binary_as_string"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["file_row_number"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["can_have_nan"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["int96_as"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_ONLY);
	copy_options["geoparquet_version"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	copy_options["shredding"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	copy_options["write_timestamp_as_int96"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	copy_options["timestamp_is_adjusted_to_utc"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);

	// Deprecated
	copy_options["row_group_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["chunk_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["row_group_size_bytes"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
	copy_options["row_groups_per_file"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	copy_options["dictionary_compression_ratio_threshold"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
}

static unique_ptr<FunctionData> ParquetWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                                 const vector<Identifier> &names,
                                                 const vector<LogicalType> &sql_types) {
	D_ASSERT(names.size() == sql_types.size());
	bool compression_level_set = false;
	auto bind_data = make_uniq<ParquetWriteBindData>();
	for (auto &[option_name, option_values] : input.info.options) {
		if (option_values.size() != 1) {
			// All parquet write options require exactly one argument
			throw BinderException("%s requires exactly one argument",
			                      StringUtil::Upper(option_name.GetIdentifierName()));
		}
		if (option_name == "compression" || option_name == "codec") {
			const auto roption = StringUtil::Lower(option_values[0].ToString());
			if (roption == "uncompressed") {
				bind_data->codec = duckdb_parquet::CompressionCodec::UNCOMPRESSED;
			} else if (roption == "snappy") {
				bind_data->codec = duckdb_parquet::CompressionCodec::SNAPPY;
			} else if (roption == "gzip") {
				bind_data->codec = duckdb_parquet::CompressionCodec::GZIP;
			} else if (roption == "zstd") {
				bind_data->codec = duckdb_parquet::CompressionCodec::ZSTD;
			} else if (roption == "brotli") {
				bind_data->codec = duckdb_parquet::CompressionCodec::BROTLI;
			} else if (roption == "lz4" || roption == "lz4_raw") {
				/* LZ4 is technically another compression scheme, but deprecated and arrow also uses them
				 * interchangeably */
				bind_data->codec = duckdb_parquet::CompressionCodec::LZ4_RAW;
			} else {
				throw BinderException(
				    "Expected %s argument to be any of [uncompressed, brotli, gzip, snappy, lz4, lz4_raw or zstd]",
				    option_name);
			}
		} else if (option_name == "field_ids") {
			if (option_values[0].type().id() == LogicalTypeId::VARCHAR &&
			    StringUtil::CIEquals(StringValue::Get(option_values[0]), "auto")) {
				idx_t field_id = 0;
				FieldID::GenerateFieldIDs(bind_data->field_ids, field_id, names, sql_types);
			} else {
				unordered_set<uint32_t> unique_field_ids;
				case_insensitive_map_t<LogicalType> name_to_type_map;
				for (idx_t col_idx = 0; col_idx < names.size(); col_idx++) {
					if (names[col_idx] == FieldID::DUCKDB_FIELD_ID) {
						throw BinderException("Cannot have a column named \"%s\" when writing FIELD_IDS",
						                      FieldID::DUCKDB_FIELD_ID);
					}
					name_to_type_map.emplace(names[col_idx], sql_types[col_idx]);
				}
				FieldID::GetFieldIDs(option_values[0], bind_data->field_ids, unique_field_ids, name_to_type_map);
			}
		} else if (option_name == "shredding") {
			if (option_values[0].type().id() == LogicalTypeId::VARCHAR &&
			    StringUtil::CIEquals(StringValue::Get(option_values[0]), "auto")) {
				throw NotImplementedException("The 'auto' option is not yet implemented for 'shredding'");
			} else {
				identifier_set_t variant_names;
				for (idx_t col_idx = 0; col_idx < names.size(); col_idx++) {
					if (sql_types[col_idx].id() != LogicalTypeId::VARIANT) {
						continue;
					}
					variant_names.emplace(names[col_idx]);
				}
				auto &shredding_types_value = option_values[0];
				if (shredding_types_value.type().id() != LogicalTypeId::STRUCT) {
					throw BinderException("SHREDDING value should be a STRUCT of column names to types, i.e: {col1: "
					                      "'INTEGER[]', col2: 'BOOLEAN'}");
				}
				const auto &struct_type = shredding_types_value.type();
				const auto &struct_children = StructValue::GetChildren(shredding_types_value);
				D_ASSERT(StructType::GetChildTypes(struct_type).size() == struct_children.size());
				for (idx_t i = 0; i < struct_children.size(); i++) {
					const auto &col_name = StructType::GetChildName(struct_type, i).GetIdentifierName();
					auto it = variant_names.find(Identifier(col_name));
					if (it == variant_names.end()) {
						string names;
						for (const auto &entry : variant_names) {
							if (!names.empty()) {
								names += ", ";
							}
							names += entry;
						}
						if (names.empty()) {
							throw BinderException("VARIANT by name \"%s\" specified in SHREDDING not found. There are "
							                      "no VARIANT columns present.",
							                      col_name);
						} else {
							throw BinderException(
							    "VARIANT by name \"%s\" specified in SHREDDING not found. Consider using "
							    "WRITE_PARTITION_COLUMNS if this "
							    "column is a partition column. Available names of VARIANT columns: [%s]",
							    col_name, names);
						}
					}
					const auto &child_value = struct_children[i];
					bind_data->shredding_types.AddChild(*it, ShreddingType::GetShreddingTypes(child_value, context));
				}
			}
		} else if (option_name == "kv_metadata") {
			auto &kv_struct = option_values[0];
			auto &kv_struct_type = kv_struct.type();
			if (kv_struct_type.id() != LogicalTypeId::STRUCT) {
				throw BinderException("Expected kv_metadata argument to be a STRUCT");
			}
			auto values = StructValue::GetChildren(kv_struct);
			for (idx_t i = 0; i < values.size(); i++) {
				auto &value = values[i];
				auto key = StructType::GetChildName(kv_struct_type, i);
				if (value.IsNull()) {
					throw BinderException("kv_metadata value for key %s cannot be NULL", key);
				}
				// If the value is a blob, write the raw blob bytes
				// otherwise, cast to string
				if (value.type().id() == LogicalTypeId::BLOB) {
					bind_data->kv_metadata.emplace_back(key, StringValue::Get(value));
				} else {
					bind_data->kv_metadata.emplace_back(key, value.ToString());
				}
			}
		} else if (option_name == "encryption_config") {
			bind_data->encryption_config = ParquetEncryptionConfig::Create(context, option_values[0]);
		} else if (option_name == "dictionary_compression_ratio_threshold" || option_name == "debug_use_openssl" ||
		           option_name == "row_group_size" || option_name == "chunk_size" ||
		           option_name == "row_group_size_bytes" || option_name == "row_groups_per_file") {
			// deprecated, ignore setting
		} else if (option_name == "dictionary_size_limit") {
			auto val = option_values[0].GetValue<int64_t>();
			if (val < 0) {
				throw BinderException("dictionary_size_limit must be greater than 0 or 0 to disable");
			}
			bind_data->dictionary_size_limit = val;
		} else if (option_name == "string_dictionary_page_size_limit") {
			auto val = option_values[0].GetValue<uint64_t>();
			if (val > PrimitiveColumnWriter::MAX_UNCOMPRESSED_DICT_PAGE_SIZE || val == 0) {
				throw BinderException(
				    "string_dictionary_page_size_limit cannot be 0 and must be less than or equal to %llu",
				    PrimitiveColumnWriter::MAX_UNCOMPRESSED_DICT_PAGE_SIZE);
			}
			bind_data->string_dictionary_page_size_limit = val;
		} else if (option_name == "data_page_size_limit") {
			auto val = option_values[0].GetValue<uint64_t>();
			if (val > PrimitiveColumnWriter::MAX_UNCOMPRESSED_PAGE_SIZE || val == 0) {
				throw BinderException("data_page_size_limit cannot be 0 and must be less than or equal to %llu",
				                      PrimitiveColumnWriter::MAX_UNCOMPRESSED_PAGE_SIZE);
			}
			bind_data->data_page_size_limit = val;
		} else if (option_name == "write_bloom_filter") {
			bind_data->enable_bloom_filters = BooleanValue::Get(option_values[0].DefaultCastAs(LogicalType::BOOLEAN));
		} else if (option_name == "bloom_filter_fallback_max_bytes") {
			bind_data->bloom_filter_fallback_max_bytes = ParseBytesArg(option_name, option_values[0]);
		} else if (option_name == "write_page_index") {
			bind_data->write_page_index = BooleanValue::Get(option_values[0].DefaultCastAs(LogicalType::BOOLEAN));
		} else if (option_name == "bloom_filter_false_positive_ratio") {
			auto val = option_values[0].GetValue<double>();
			if (!(val > 0 && val <= 1)) {
				throw BinderException("bloom_filter_false_positive_ratio must be greater than 0 and at most 1");
			}
			bind_data->bloom_filter_false_positive_ratio = val;
		} else if (option_name == "compression_level") {
			const auto val = option_values[0].GetValue<int64_t>();
			if (val < ZStdFileSystem::MinimumCompressionLevel() || val > ZStdFileSystem::MaximumCompressionLevel()) {
				throw BinderException("Compression level must be between %lld and %lld",
				                      ZStdFileSystem::MinimumCompressionLevel(),
				                      ZStdFileSystem::MaximumCompressionLevel());
			}
			bind_data->compression_level = val;
			compression_level_set = true;
		} else if (option_name == "parquet_version") {
			const auto roption = StringUtil::Upper(option_values[0].ToString());
			if (roption == "V1") {
				bind_data->parquet_version = ParquetVersion::V1;
			} else if (roption == "V2") {
				bind_data->parquet_version = ParquetVersion::V2;
			} else {
				throw BinderException("Expected parquet_version 'V1' or 'V2'");
			}
		} else if (option_name == "geoparquet_version") {
			const auto roption = StringUtil::Upper(option_values[0].ToString());
			if (roption == "NONE") {
				bind_data->geoparquet_version = GeoParquetVersion::NONE;
			} else if (roption == "V1") {
				bind_data->geoparquet_version = GeoParquetVersion::V1;
			} else if (roption == "V2") {
				bind_data->geoparquet_version = GeoParquetVersion::V2;
			} else if (roption == "BOTH") {
				bind_data->geoparquet_version = GeoParquetVersion::BOTH;
			} else {
				throw BinderException("Expected geoparquet_version 'NONE', 'V1' or 'BOTH'");
			}
		} else if (option_name == "write_timestamp_as_int96") {
			bind_data->write_timestamp_as_int96 =
			    BooleanValue::Get(option_values[0].DefaultCastAs(LogicalType::BOOLEAN));
		} else if (option_name == "timestamp_is_adjusted_to_utc") {
			bind_data->timestamp_is_adjusted_to_utc =
			    EnumUtil::FromString<TimeStampIsAdjustedToUTC>(StringUtil::Upper(option_values[0].ToString()));
		} else {
			throw InternalException("Unrecognized option for PARQUET: %s", option_name.c_str());
		}
	}

	if (compression_level_set && bind_data->codec != CompressionCodec::ZSTD) {
		throw BinderException("Compression level is only supported for the ZSTD compression codec");
	}

	bind_data->sql_types = sql_types;
	bind_data->column_names = IdentifiersToStrings(names);

	return std::move(bind_data);
}

static void ParquetCopyToPropagateStatistics(CopyToPropagateStatsInput &input) {
	auto &parquet_bind = input.bind_data.Cast<ParquetWriteBindData>();
	auto count = parquet_bind.sql_types.size();
	if (input.column_stats.size() != count) {
		return;
	}
	parquet_bind.not_null_columns.assign(count, false);
	for (idx_t i = 0; i < count; i++) {
		if (input.column_stats[i] && !input.column_stats[i]->CanHaveNull()) {
			parquet_bind.not_null_columns[i] = true;
		}
	}
}

static unique_ptr<GlobalFunctionData> ParquetWriteInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                                   const string &file_path) {
	auto global_state = make_uniq<ParquetWriteGlobalState>();
	auto &parquet_bind = bind_data.Cast<ParquetWriteBindData>();

	auto &fs = FileSystem::GetFileSystem(context);

	ParquetWriterOptions options;
	options.file_name = file_path;
	options.sql_types = parquet_bind.sql_types;
	options.column_names = parquet_bind.column_names;
	options.codec = parquet_bind.codec, options.field_ids = parquet_bind.field_ids.Copy();
	options.shredding_types = parquet_bind.shredding_types.Copy();
	options.encryption_config = parquet_bind.encryption_config;
	options.dictionary_size_limit = parquet_bind.dictionary_size_limit,
	options.string_dictionary_page_size_limit = parquet_bind.string_dictionary_page_size_limit;
	// pages are what the page index lets readers skip: with the index, pages follow the 1 MiB default of the other
	// writers; without it the whole column chunk stays one page, as before
	options.data_page_size_limit =
	    parquet_bind.data_page_size_limit != 0
	        ? parquet_bind.data_page_size_limit
	        : (parquet_bind.write_page_index ? PrimitiveColumnWriter::DEFAULT_DATA_PAGE_SIZE_LIMIT
	                                         : PrimitiveColumnWriter::MAX_UNCOMPRESSED_PAGE_SIZE);
	options.enable_bloom_filters = parquet_bind.enable_bloom_filters,
	options.write_page_index = parquet_bind.write_page_index;
	options.bloom_filter_false_positive_ratio = parquet_bind.bloom_filter_false_positive_ratio;
	options.bloom_filter_fallback_max_bytes = parquet_bind.bloom_filter_fallback_max_bytes;
	options.compression_level = parquet_bind.compression_level;
	options.parquet_version = parquet_bind.parquet_version,
	options.geoparquet_version = parquet_bind.geoparquet_version;
	options.write_timestamp_as_int96 = parquet_bind.write_timestamp_as_int96,
	options.timestamp_is_adjusted_to_utc = parquet_bind.timestamp_is_adjusted_to_utc;
	options.not_null_columns = parquet_bind.not_null_columns;

	auto flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
	// checking a remote path costs a request, so a remote output is opened like one that may exist
	if (!fs.IsRemoteFile(file_path) && !fs.FileExists(file_path) && !fs.IsPipe(file_path)) {
		flags |= FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
	}
	options.open_flags = flags;
	global_state->writer = make_uniq<ParquetWriter>(context, fs, std::move(options), parquet_bind.kv_metadata);
	return std::move(global_state);
}

static void ParquetWriteGetWrittenStatistics(ClientContext &context, FunctionData &bind_data,
                                             GlobalFunctionData &gstate, CopyFunctionFileStatistics &statistics) {
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	global_state.writer->SetWrittenStatistics(statistics);
}

static void ParquetWriteSink(ExecutionContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate,
                             LocalFunctionData &lstate, DataChunk &input) {
	auto &bind_data = bind_data_p.Cast<ParquetWriteBindData>();
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	auto &local_state = lstate.Cast<ParquetWriteLocalState>();

	// append data to the local (buffered) chunk collection
	local_state.buffer.Append(local_state.append_state, input);

	if (local_state.buffer.Count() >= bind_data.row_group_size ||
	    local_state.buffer.SizeInBytes() >= bind_data.row_group_size_bytes) {
		// if the chunk collection exceeds a certain size (rows/bytes) we flush it to the parquet file
		local_state.append_state.current_chunk_state.handles.clear();
		global_state.writer->Flush(local_state.buffer, local_state.transform_data);
		local_state.buffer.InitializeAppend(local_state.append_state);
	}
}

static void ParquetWriteCombine(ExecutionContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate,
                                LocalFunctionData &lstate) {
	auto &bind_data = bind_data_p.Cast<ParquetWriteBindData>();
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	auto &local_state = lstate.Cast<ParquetWriteLocalState>();

	if (local_state.buffer.Count() >= bind_data.row_group_size / 2 ||
	    local_state.buffer.SizeInBytes() >= bind_data.row_group_size_bytes / 2) {
		// local state buffer is more than half of the row_group_size(_bytes), just flush it
		global_state.writer->Flush(local_state.buffer, local_state.transform_data);
		return;
	}

	unique_lock<mutex> guard(global_state.lock);
	if (global_state.combine_buffer) {
		// There is still some data, combine it
		global_state.combine_buffer->Combine(local_state.buffer);
		if (global_state.combine_buffer->Count() >= bind_data.row_group_size / 2 ||
		    global_state.combine_buffer->SizeInBytes() >= bind_data.row_group_size_bytes / 2) {
			// After combining, the combine buffer is more than half of the row_group_size(_bytes), so we flush
			auto owned_combine_buffer = std::move(global_state.combine_buffer);
			guard.unlock();
			// Lock free, of course
			global_state.writer->Flush(*owned_combine_buffer, local_state.transform_data);
		}
		return;
	}

	global_state.combine_buffer = make_uniq<ColumnDataCollection>(context.client, local_state.buffer.Types());
	global_state.combine_buffer->Combine(local_state.buffer);
}

static void ParquetWriteFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	// flush the combine buffer (if it's there)
	if (global_state.combine_buffer) {
		global_state.writer->Flush(*global_state.combine_buffer, global_state.transform_data);
	}

	// finalize: write any additional metadata to the file here
	global_state.writer->Finalize();
}

static unique_ptr<LocalFunctionData> ParquetWriteInitializeLocal(ExecutionContext &context, FunctionData &bind_data_p) {
	auto &bind_data = bind_data_p.Cast<ParquetWriteBindData>();
	return make_uniq<ParquetWriteLocalState>(context.client, bind_data.sql_types);
}

// LCOV_EXCL_START

// FIXME: Have these be generated instead
template <>
const char *EnumUtil::ToChars<duckdb_parquet::CompressionCodec::type>(duckdb_parquet::CompressionCodec::type value) {
	switch (value) {
	case CompressionCodec::UNCOMPRESSED:
		return "UNCOMPRESSED";
		break;
	case CompressionCodec::SNAPPY:
		return "SNAPPY";
		break;
	case CompressionCodec::GZIP:
		return "GZIP";
		break;
	case CompressionCodec::LZO:
		return "LZO";
		break;
	case CompressionCodec::BROTLI:
		return "BROTLI";
		break;
	case CompressionCodec::LZ4:
		return "LZ4";
		break;
	case CompressionCodec::LZ4_RAW:
		return "LZ4_RAW";
		break;
	case CompressionCodec::ZSTD:
		return "ZSTD";
		break;
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
duckdb_parquet::CompressionCodec::type EnumUtil::FromString<duckdb_parquet::CompressionCodec::type>(const char *value) {
	if (StringUtil::Equals(value, "UNCOMPRESSED")) {
		return CompressionCodec::UNCOMPRESSED;
	}
	if (StringUtil::Equals(value, "SNAPPY")) {
		return CompressionCodec::SNAPPY;
	}
	if (StringUtil::Equals(value, "GZIP")) {
		return CompressionCodec::GZIP;
	}
	if (StringUtil::Equals(value, "LZO")) {
		return CompressionCodec::LZO;
	}
	if (StringUtil::Equals(value, "BROTLI")) {
		return CompressionCodec::BROTLI;
	}
	if (StringUtil::Equals(value, "LZ4")) {
		return CompressionCodec::LZ4;
	}
	if (StringUtil::Equals(value, "LZ4_RAW")) {
		return CompressionCodec::LZ4_RAW;
	}
	if (StringUtil::Equals(value, "ZSTD")) {
		return CompressionCodec::ZSTD;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *EnumUtil::ToChars<ParquetVersion>(ParquetVersion value) {
	switch (value) {
	case ParquetVersion::V1:
		return "V1";
	case ParquetVersion::V2:
		return "V2";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
ParquetVersion EnumUtil::FromString<ParquetVersion>(const char *value) {
	if (StringUtil::Equals(value, "V1")) {
		return ParquetVersion::V1;
	}
	if (StringUtil::Equals(value, "V2")) {
		return ParquetVersion::V2;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *EnumUtil::ToChars<ParquetPrefetchStrategyOption>(ParquetPrefetchStrategyOption value) {
	switch (value) {
	case ParquetPrefetchStrategyOption::AUTO:
		return "AUTO";
	case ParquetPrefetchStrategyOption::WHOLE_GROUP:
		return "WHOLE_GROUP";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
ParquetPrefetchStrategyOption EnumUtil::FromString<ParquetPrefetchStrategyOption>(const char *value) {
	if (StringUtil::Equals(value, "AUTO")) {
		return ParquetPrefetchStrategyOption::AUTO;
	}
	if (StringUtil::Equals(value, "WHOLE_GROUP")) {
		return ParquetPrefetchStrategyOption::WHOLE_GROUP;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *EnumUtil::ToChars<ParquetInt96AsOption>(ParquetInt96AsOption value) {
	switch (value) {
	case ParquetInt96AsOption::TIMESTAMP:
		return "TIMESTAMP";
	case ParquetInt96AsOption::TIMESTAMP_NS:
		return "TIMESTAMP_NS";
	case ParquetInt96AsOption::STRUCT:
		return "STRUCT";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
ParquetInt96AsOption EnumUtil::FromString<ParquetInt96AsOption>(const char *value) {
	if (StringUtil::Equals(value, "TIMESTAMP")) {
		return ParquetInt96AsOption::TIMESTAMP;
	}
	if (StringUtil::Equals(value, "TIMESTAMP_NS")) {
		return ParquetInt96AsOption::TIMESTAMP_NS;
	}
	if (StringUtil::Equals(value, "STRUCT")) {
		return ParquetInt96AsOption::STRUCT;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *
EnumUtil::ToChars<StringColumnReader::Utf8ValidationOption>(StringColumnReader::Utf8ValidationOption value) {
	switch (value) {
	case StringColumnReader::Utf8ValidationOption::STRICT_UTF8:
		return "STRICT";
	case StringColumnReader::Utf8ValidationOption::REPLACE_UTF8:
		return "REPLACE";
	case StringColumnReader::Utf8ValidationOption::IGNORE_UTF8:
		return "IGNORE";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
StringColumnReader::Utf8ValidationOption
EnumUtil::FromString<StringColumnReader::Utf8ValidationOption>(const char *value) {
	if (StringUtil::Equals(value, "STRICT")) {
		return StringColumnReader::Utf8ValidationOption::STRICT_UTF8;
	}
	if (StringUtil::Equals(value, "REPLACE")) {
		return StringColumnReader::Utf8ValidationOption::REPLACE_UTF8;
	}
	if (StringUtil::Equals(value, "IGNORE")) {
		return StringColumnReader::Utf8ValidationOption::IGNORE_UTF8;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *EnumUtil::ToChars<ParquetReaderProjectionExpressionType>(ParquetReaderProjectionExpressionType value) {
	switch (value) {
	case ParquetReaderProjectionExpressionType::BYTE_LENGTH:
		return "BYTE_LENGTH";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
ParquetReaderProjectionExpressionType EnumUtil::FromString<ParquetReaderProjectionExpressionType>(const char *value) {
	if (StringUtil::Equals(value, "BYTE_LENGTH")) {
		return ParquetReaderProjectionExpressionType::BYTE_LENGTH;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *EnumUtil::ToChars<GeoParquetVersion>(GeoParquetVersion value) {
	switch (value) {
	case GeoParquetVersion::NONE:
		return "NONE";
	case GeoParquetVersion::V1:
		return "V1";
	case GeoParquetVersion::V2:
		return "V2";
	case GeoParquetVersion::BOTH:
		return "BOTH";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
GeoParquetVersion EnumUtil::FromString<GeoParquetVersion>(const char *value) {
	if (StringUtil::Equals(value, "NONE")) {
		return GeoParquetVersion::NONE;
	}
	if (StringUtil::Equals(value, "V1")) {
		return GeoParquetVersion::V1;
	}
	if (StringUtil::Equals(value, "V2")) {
		return GeoParquetVersion::V2;
	}
	if (StringUtil::Equals(value, "BOTH")) {
		return GeoParquetVersion::BOTH;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

template <>
const char *EnumUtil::ToChars<TimeStampIsAdjustedToUTC>(TimeStampIsAdjustedToUTC value) {
	switch (value) {
	case TimeStampIsAdjustedToUTC::AUTO:
		return "AUTO";
	case TimeStampIsAdjustedToUTC::ALWAYS_TRUE:
		return "TRUE";
	case TimeStampIsAdjustedToUTC::ALWAYS_FALSE:
		return "FALSE";
	default:
		throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
	}
}

template <>
TimeStampIsAdjustedToUTC EnumUtil::FromString<TimeStampIsAdjustedToUTC>(const char *value) {
	if (StringUtil::Equals(value, "AUTO")) {
		return TimeStampIsAdjustedToUTC::AUTO;
	}
	if (StringUtil::Equals(value, "TRUE")) {
		return TimeStampIsAdjustedToUTC::ALWAYS_TRUE;
	}
	if (StringUtil::Equals(value, "FALSE")) {
		return TimeStampIsAdjustedToUTC::ALWAYS_FALSE;
	}
	throw NotImplementedException(StringUtil::Format("Enum value: '%s' not implemented", value));
}

static optional_idx SerializeCompressionLevel(const int64_t compression_level) {
	return compression_level < 0 ? NumericLimits<idx_t>::Maximum() - NumericCast<idx_t>(AbsValue(compression_level))
	                             : NumericCast<idx_t>(compression_level);
}

static int64_t DeserializeCompressionLevel(const optional_idx compression_level) {
	// Was originally an optional_idx, now int64_t, so we still serialize as such
	if (!compression_level.IsValid()) {
		return ZStdFileSystem::DefaultCompressionLevel();
	}
	if (compression_level.GetIndex() > NumericCast<idx_t>(ZStdFileSystem::MaximumCompressionLevel())) {
		// restore the negative compression level
		return -NumericCast<int64_t>(NumericLimits<idx_t>::Maximum() - compression_level.GetIndex());
	}
	return NumericCast<int64_t>(compression_level.GetIndex());
}

static void ParquetCopySerialize(Serializer &serializer, const FunctionData &bind_data_p,
                                 const CopyFunction &function) {
	auto &bind_data = bind_data_p.Cast<ParquetWriteBindData>();
	serializer.WriteProperty(100, "sql_types", bind_data.sql_types);
	serializer.WriteProperty(101, "column_names", bind_data.column_names);
	serializer.WriteProperty(102, "codec", bind_data.codec);

	// 103 was row_group_size, but was deleted
	// 104 was row_group_size_bytes, but was deleted

	serializer.WriteProperty(105, "kv_metadata", bind_data.kv_metadata);
	serializer.WriteProperty(106, "field_ids", bind_data.field_ids);
	serializer.WritePropertyWithDefault<shared_ptr<ParquetEncryptionConfig>>(107, "encryption_config",
	                                                                         bind_data.encryption_config, nullptr);

	// 108 was dictionary_compression_ratio_threshold, but was deleted

	// To avoid doubly defining the default values in both ParquetWriteBindData and here,
	// and possibly making a mistake, we just get the values from ParquetWriteBindData.
	// We have to std::move them, otherwise MSVC will complain that it's not a "const T &&"
	const auto compression_level = SerializeCompressionLevel(bind_data.compression_level);
	D_ASSERT(DeserializeCompressionLevel(compression_level) == bind_data.compression_level);
	ParquetWriteBindData default_value;
	serializer.WritePropertyWithDefault(109, "compression_level", compression_level);

	// 110 was row_groups_per_file, but was deleted

	serializer.WritePropertyWithDefault(112, "dictionary_size_limit", bind_data.dictionary_size_limit,
	                                    default_value.dictionary_size_limit);
	serializer.WritePropertyWithDefault(113, "bloom_filter_false_positive_ratio",
	                                    bind_data.bloom_filter_false_positive_ratio,
	                                    default_value.bloom_filter_false_positive_ratio);
	serializer.WritePropertyWithDefault(114, "parquet_version", bind_data.parquet_version,
	                                    default_value.parquet_version);
	serializer.WritePropertyWithDefault(115, "string_dictionary_page_size_limit",
	                                    bind_data.string_dictionary_page_size_limit,
	                                    default_value.string_dictionary_page_size_limit);
	serializer.WritePropertyWithDefault(116, "geoparquet_version", bind_data.geoparquet_version,
	                                    default_value.geoparquet_version);
	serializer.WritePropertyWithDefault<ShreddingType>(117, "shredding_types", bind_data.shredding_types,
	                                                   default_value.shredding_types);
	serializer.WritePropertyWithDefault(118, "timestamp_is_adjusted_to_utc", bind_data.timestamp_is_adjusted_to_utc,
	                                    default_value.timestamp_is_adjusted_to_utc);
	serializer.WritePropertyWithDefault(119, "write_timestamp_as_int96", bind_data.write_timestamp_as_int96,
	                                    default_value.write_timestamp_as_int96);
	serializer.WritePropertyWithDefault<vector<bool>>(120, "not_null_columns", bind_data.not_null_columns,
	                                                  default_value.not_null_columns);
	serializer.WritePropertyWithDefault(121, "data_page_size_limit", bind_data.data_page_size_limit,
	                                    default_value.data_page_size_limit);
	serializer.WritePropertyWithDefault(122, "write_page_index", bind_data.write_page_index,
	                                    default_value.write_page_index);
	serializer.WritePropertyWithDefault(123, "enable_bloom_filters", bind_data.enable_bloom_filters,
	                                    default_value.enable_bloom_filters);
	serializer.WritePropertyWithDefault(124, "bloom_filter_fallback_max_bytes",
	                                    bind_data.bloom_filter_fallback_max_bytes,
	                                    default_value.bloom_filter_fallback_max_bytes);
}

static unique_ptr<FunctionData> ParquetCopyDeserialize(Deserializer &deserializer, CopyFunction &function) {
	auto data = make_uniq<ParquetWriteBindData>();
	data->sql_types = deserializer.ReadProperty<vector<LogicalType>>(100, "sql_types");
	data->column_names = deserializer.ReadProperty<vector<string>>(101, "column_names");
	data->codec = deserializer.ReadProperty<duckdb_parquet::CompressionCodec::type>(102, "codec");
	deserializer.ReadDeletedProperty<idx_t>(103, "row_group_size");
	deserializer.ReadDeletedProperty<idx_t>(104, "row_group_size_bytes");
	data->kv_metadata = deserializer.ReadProperty<vector<pair<string, string>>>(105, "kv_metadata");
	data->field_ids = deserializer.ReadProperty<ChildFieldIDs>(106, "field_ids");
	deserializer.ReadPropertyWithExplicitDefault<shared_ptr<ParquetEncryptionConfig>>(
	    107, "encryption_config", data->encryption_config, std::move(ParquetWriteBindData().encryption_config));
	deserializer.ReadDeletedProperty<double>(108, "dictionary_compression_ratio_threshold");

	optional_idx compression_level;
	deserializer.ReadPropertyWithDefault<optional_idx>(109, "compression_level", compression_level);
	data->compression_level = DeserializeCompressionLevel(compression_level);
	D_ASSERT(SerializeCompressionLevel(data->compression_level) == compression_level);
	ParquetWriteBindData default_value;
	deserializer.ReadDeletedProperty<optional_idx>(110, "row_groups_per_file");
	deserializer.ReadDeletedProperty<bool>(111, "debug_use_openssl");
	data->dictionary_size_limit =
	    deserializer.ReadPropertyWithExplicitDefault<optional_idx>(112, "dictionary_size_limit", optional_idx());
	data->bloom_filter_false_positive_ratio = deserializer.ReadPropertyWithExplicitDefault<double>(
	    113, "bloom_filter_false_positive_ratio", default_value.bloom_filter_false_positive_ratio);
	data->parquet_version =
	    deserializer.ReadPropertyWithExplicitDefault(114, "parquet_version", default_value.parquet_version);
	data->string_dictionary_page_size_limit = deserializer.ReadPropertyWithExplicitDefault(
	    115, "string_dictionary_page_size_limit", default_value.string_dictionary_page_size_limit);
	data->geoparquet_version =
	    deserializer.ReadPropertyWithExplicitDefault(116, "geoparquet_version", default_value.geoparquet_version);
	data->shredding_types =
	    deserializer.ReadPropertyWithExplicitDefault<ShreddingType>(117, "shredding_types", ShreddingType());
	data->timestamp_is_adjusted_to_utc = deserializer.ReadPropertyWithExplicitDefault(
	    118, "timestamp_is_adjusted_to_utc", default_value.timestamp_is_adjusted_to_utc);
	data->write_timestamp_as_int96 = deserializer.ReadPropertyWithExplicitDefault(
	    119, "write_timestamp_as_int96", default_value.write_timestamp_as_int96);
	data->not_null_columns =
	    deserializer.ReadPropertyWithExplicitDefault<vector<bool>>(120, "not_null_columns", vector<bool>());
	data->data_page_size_limit =
	    deserializer.ReadPropertyWithExplicitDefault(121, "data_page_size_limit", default_value.data_page_size_limit);
	data->write_page_index =
	    deserializer.ReadPropertyWithExplicitDefault(122, "write_page_index", default_value.write_page_index);
	data->enable_bloom_filters =
	    deserializer.ReadPropertyWithExplicitDefault(123, "enable_bloom_filters", default_value.enable_bloom_filters);
	data->bloom_filter_fallback_max_bytes = deserializer.ReadPropertyWithExplicitDefault(
	    124, "bloom_filter_fallback_max_bytes", default_value.bloom_filter_fallback_max_bytes);

	return std::move(data);
}
// LCOV_EXCL_STOP

//===--------------------------------------------------------------------===//
// Execution Mode
//===--------------------------------------------------------------------===//
static CopyFunctionExecutionMode ParquetWriteExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	if (!preserve_insertion_order) {
		return CopyFunctionExecutionMode::PARALLEL_COPY_TO_FILE;
	}
	if (supports_batch_index) {
		return CopyFunctionExecutionMode::BATCH_COPY_TO_FILE;
	}
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}
//===--------------------------------------------------------------------===//
// Initialize Logger
//===--------------------------------------------------------------------===//
static void ParquetWriteInitializeOperator(GlobalFunctionData &gstate, const PhysicalOperator &op) {
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	global_state.op = &op;
}
//===--------------------------------------------------------------------===//
// Prepare Batch
//===--------------------------------------------------------------------===//
struct ParquetWriteBatchData : public PreparedBatchData {
	explicit ParquetWriteBatchData(Allocator &allocator) : prepared_row_group(allocator) {
	}

	PreparedRowGroup prepared_row_group;
};

static unique_ptr<PreparedBatchData> ParquetWritePrepareBatch(ClientContext &context, FunctionData &bind_data,
                                                              GlobalFunctionData &gstate,
                                                              unique_ptr<ColumnDataCollection> collection) {
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	auto result = make_uniq<ParquetWriteBatchData>(BufferAllocator::Get(context));
	unique_ptr<ParquetWriteTransformData> transform_data;
	global_state.writer->PrepareRowGroup(*collection, result->prepared_row_group, transform_data);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Flush Batch
//===--------------------------------------------------------------------===//
static void ParquetWriteFlushBatch(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                   PreparedBatchData &batch_p) {
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	auto &batch = batch_p.Cast<ParquetWriteBatchData>();
	global_state.writer->FlushRowGroup(batch.prepared_row_group);
}

//===--------------------------------------------------------------------===//
// Desired Batch Size
//===--------------------------------------------------------------------===//
static optional_idx ParquetWriteDesiredBatchSize(ClientContext &context, FunctionData &bind_data_p) {
	auto &bind_data = bind_data_p.Cast<ParquetWriteBindData>();
	return bind_data.row_group_size;
}

//===--------------------------------------------------------------------===//
// File Size Bytes
//===--------------------------------------------------------------------===//
static idx_t ParquetWriteFileSizeBytes(GlobalFunctionData &gstate) {
	auto &global_state = gstate.Cast<ParquetWriteGlobalState>();
	return global_state.writer->FileSize();
}

//===--------------------------------------------------------------------===//
// Scan Replacement
//===--------------------------------------------------------------------===//
static unique_ptr<TableRef> ParquetScanReplacement(ClientContext &context, ReplacementScanInput &input,
                                                   optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"parquet"})) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ConstantExpression::String(table_name));
	table_function->function = make_uniq<FunctionExpression>("parquet_scan", std::move(children));

	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = Identifier(fs.ExtractBaseName(table_name));
	}

	return std::move(table_function);
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
// Helper predicates for ParquetWriteSelect
static bool IsTypeNotSupported(const LogicalType &type) {
	if (type.IsNested()) {
		return false;
	}
	return !ParquetWriter::TryGetParquetType(type);
}

static bool IsTypeLossy(const LogicalType &type) {
	return type.id() == LogicalTypeId::HUGEINT || type.id() == LogicalTypeId::UHUGEINT;
}

static bool IsExtensionGeometryType(const LogicalType &type, ClientContext &context) {
	if (type.id() != LogicalTypeId::BLOB) {
		return false;
	}
	if (!type.HasAlias()) {
		return false;
	}
	if (type.GetAlias() != "GEOMETRY") {
		return false;
	}
	return GeoParquetFileMetadata::IsGeoParquetConversionEnabled(context);
}

static vector<unique_ptr<Expression>> ParquetWriteSelect(CopyToSelectInput &input) {
	auto &context = input.context;

	vector<unique_ptr<Expression>> result;

	bool any_change = false;

	for (auto &expr : input.select_list) {
		const auto &type = expr->GetReturnType();
		const auto &name = expr->GetAlias();

		// Spatial types need to be encoded into WKB when writing GeoParquet.
		// But dont perform this conversion if this is a EXPORT DATABASE statement
		if (input.copy_to_type == CopyToType::COPY_TO_FILE && IsExtensionGeometryType(type, context)) {
			// Cast the column to GEOMETRY
			auto cast_expr =
			    BoundCastExpression::AddCastToType(context, std::move(expr), LogicalType::GEOMETRY(), false);
			cast_expr->SetAlias(name);
			result.push_back(std::move(cast_expr));
			any_change = true;
		}
		// If this is an EXPORT DATABASE statement, we dont want to write "lossy" types, instead cast them to VARCHAR
		else if (input.copy_to_type == CopyToType::EXPORT_DATABASE && TypeVisitor::Contains(type, IsTypeLossy)) {
			// Replace all lossy types with VARCHAR
			auto new_type = TypeVisitor::VisitReplace(
			    type, [](const LogicalType &ty) -> LogicalType { return IsTypeLossy(ty) ? LogicalType::VARCHAR : ty; });

			// Cast the column to the new type
			auto cast_expr = BoundCastExpression::AddCastToType(context, std::move(expr), new_type, false);
			cast_expr->SetAlias(name);
			result.push_back(std::move(cast_expr));
			any_change = true;
		}
		// Else look if there is any unsupported type
		else if (TypeVisitor::Contains(type, IsTypeNotSupported)) {
			// If there is at least one unsupported type, replace all unsupported types with varchar
			// and perform a CAST
			auto new_type = TypeVisitor::VisitReplace(type, [](const LogicalType &ty) -> LogicalType {
				return IsTypeNotSupported(ty) ? LogicalType::VARCHAR : ty;
			});

			auto cast_expr = BoundCastExpression::AddCastToType(context, std::move(expr), new_type, false);
			cast_expr->SetAlias(name);
			result.push_back(std::move(cast_expr));
			any_change = true;
		}
		// Otherwise, just reference the input column
		else {
			result.push_back(std::move(expr));
		}
	}

	// If any change was made, return the new expressions
	// otherwise, return an empty vector to indicate no change and avoid pushing another projection on to the plan
	if (any_change) {
		return result;
	}
	return {};
}

//! Bind a COPY ... FROM a parquet file - the reader is the same one read_parquet is built on
static unique_ptr<FunctionData> ParquetCopyFromBind(ClientContext &context, CopyFromFunctionBindInput &input,
                                                    vector<Identifier> &expected_names,
                                                    vector<LogicalType> &expected_types) {
	// write options (e.g. written by EXPORT DATABASE) have no effect on reading - the codec is read from the file
	auto info = input.info.Copy();
	info->options.erase("codec");
	info->options.erase("row_group_size");
	CopyFromFunctionBindInput read_input(*info, input.tf);
	return TableFunctionMultiFileWrapper::MultiFileBindCopyWith(context, read_input, expected_names, expected_types,
	                                                            ParquetScanFunction::GetSingleFileFunction(),
	                                                            ParquetScanFunction::GetMultiFileSettings());
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db_instance = loader.GetDatabaseInstance();
	auto &fs = db_instance.GetFileSystem();
	fs.RegisterCompressionFilesystem(make_uniq<ZStdFileSystem>());

	auto scan_fun = MultiFileReader::CreateFunctionSet(ParquetScanFunction::GetMultiFileFunction("read_parquet"));
	scan_fun.SetName("read_parquet");
	loader.RegisterFunction(scan_fun);
	scan_fun.SetName("parquet_scan");
	loader.RegisterFunction(scan_fun);

	// the single-file parquet reader that the multi-file reader above is built on
	TableFunctionSet single_file_set("read_single_parquet_file");
	single_file_set.AddFunction(ParquetScanFunction::GetSingleFileFunction());
	loader.RegisterFunction(std::move(single_file_set));

	// parquet_metadata
	ParquetMetaDataFunction meta_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(meta_fun));

	// parquet_schema
	ParquetSchemaFunction schema_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(schema_fun));

	// parquet_key_value_metadata
	ParquetKeyValueMetadataFunction kv_meta_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(kv_meta_fun));

	// parquet_file_metadata
	ParquetFileMetadataFunction file_meta_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(file_meta_fun));

	// parquet_bloom_probe
	ParquetBloomProbeFunction bloom_probe_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(bloom_probe_fun));

	// parquet_full_metadata
	ParquetFullMetadataFunction full_meta_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(full_meta_fun));

	// parquet_page_index
	ParquetPageIndexFunction page_index_fun;
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(page_index_fun));

	// variant_to_parquet_variant
	loader.RegisterType("PARQUET_VARIANT", VariantColumnWriter::GetParquetVariantType());
	loader.RegisterFunction(VariantColumnWriter::GetTransformFunction());

	// bytes_to_variant
	loader.RegisterFunction(VariantColumnWriter::GetBytesToVariantFunction());

	CopyFunction function("parquet");
	function.copy_to_select = ParquetWriteSelect;
	function.copy_to_bind = ParquetWriteBind;
	function.copy_to_propagate_statistics = ParquetCopyToPropagateStatistics;
	function.copy_options = ParquetListCopyOptions;
	function.copy_to_initialize_global = ParquetWriteInitializeGlobal;
	function.copy_to_initialize_local = ParquetWriteInitializeLocal;
	function.copy_to_get_written_statistics = ParquetWriteGetWrittenStatistics;
	function.copy_to_sink = ParquetWriteSink;
	function.copy_to_combine = ParquetWriteCombine;
	function.copy_to_finalize = ParquetWriteFinalize;
	function.execution_mode = ParquetWriteExecutionMode;
	function.initialize_operator = ParquetWriteInitializeOperator;
	function.copy_from_bind = ParquetCopyFromBind;
	function.copy_from_function = *scan_fun.functions[0];

	function.prepare_batch = ParquetWritePrepareBatch;
	function.flush_batch = ParquetWriteFlushBatch;
	function.file_size_bytes = ParquetWriteFileSizeBytes;

	function.desired_batch_size = ParquetWriteDesiredBatchSize;

	function.serialize = ParquetCopySerialize;
	function.deserialize = ParquetCopyDeserialize;

	function.extension = "parquet";
	loader.RegisterFunction(function);

	// parquet_key
	auto parquet_key_fun = PragmaFunction::PragmaCall("add_parquet_key", ParquetCrypto::AddKey,
	                                                  FunctionSignature()
	                                                      .AddPositionalOnly("key_name", LogicalType::VARCHAR)
	                                                      .AddPositionalOnly("key", LogicalType::VARCHAR));
	loader.RegisterFunction(parquet_key_fun);

	auto &config = DBConfig::GetConfig(db_instance);
	config.replacement_scans.emplace_back(ParquetScanReplacement);
	config.AddExtensionOption("binary_as_string", "In Parquet files, interpret binary data as a string.",
	                          LogicalType::BOOLEAN, Value(false));
	config.AddExtensionOption("disable_parquet_prefetching", "Disable the prefetching mechanism in Parquet",
	                          LogicalType::BOOLEAN, Value(false));
	config.AddExtensionOption("prefetch_all_parquet_files",
	                          "(deprecated) Parquet files are now always prefetched, this setting has no effect",
	                          LogicalType::BOOLEAN, Value(false));
	config.AddExtensionOption(
	    "parquet_prefetch_column_gap",
	    "Byte gap under which Parquet prefetch I/O ranges are coalesced (NULL lets the cost model adapt it)",
	    LogicalType::UBIGINT, Value(LogicalType::UBIGINT));
	config.AddExtensionOption("parquet_metadata_cache",
	                          "Cache Parquet metadata - useful when reading the same files multiple times",
	                          LogicalType::BOOLEAN, Value(false));
	config.AddExtensionOption("parquet_use_page_index",
	                          "Use the page index (ColumnIndex and OffsetIndex) of Parquet files to skip the pages "
	                          "that cannot contain rows matching the filters",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption(
	    "enable_geoparquet_conversion",
	    "Attempt to decode/encode geometry data in/as GeoParquet files if the spatial extension is present.",
	    LogicalType::BOOLEAN, Value::BOOLEAN(true));
}

// LCOV_EXCL_START
void ParquetExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ParquetExtension::Name() {
	return "parquet";
}

std::string ParquetExtension::Version() const {
#ifdef EXT_VERSION_PARQUET
	return EXT_VERSION_PARQUET;
#else
	return "";
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(parquet, loader) { // NOLINT
	duckdb::LoadInternal(loader);
}
}
