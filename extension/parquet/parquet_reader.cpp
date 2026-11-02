#include "parquet_reader.hpp"

#include <string.h>
#include <unordered_map>
#include <vector>

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parallel/callback_async_task.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "parquet_types.h"
#include "column_reader.hpp"
#include "reader/byte_array_length_column_reader.hpp"
#include "reader/expression_column_reader.hpp"
#include "parquet_geometry.hpp"
#include "parquet_int96.hpp"
#include "reader/list_column_reader.hpp"
#include "parquet_crypto.hpp"
#include "parquet_file_metadata_cache.hpp"
#include "parquet_statistics.hpp"
#include "reader/row_number_column_reader.hpp"
#include "reader/variant_column_reader.hpp"
#include "reader/struct_column_reader.hpp"
#include "reader/string_column_reader.hpp"
#include "thrift_tools.hpp"
#include "parquet_prefetch_cost_model.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/common/multi_file/multi_file_adaptive_filter_cache.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/scalar/struct_utils.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/original/std/memory.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "thrift/TBase.h"
#include "thrift/protocol/TCompactProtocol.h"
#include "thrift/transport/TTransport.h"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::UNIT_MS) == LogicalTypeId::TIMESTAMP_TZ);
static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::UNIT_MICROS) == LogicalTypeId::TIMESTAMP_TZ);
static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::UNIT_NS) == LogicalTypeId::TIMESTAMP_TZ_NS);

static_assert(ParquetTimeTzLogicalType(ParquetExtraTypeInfo::UNIT_MS) == LogicalTypeId::TIME_TZ);
static_assert(ParquetTimeTzLogicalType(ParquetExtraTypeInfo::UNIT_MICROS) == LogicalTypeId::TIME_TZ);
static_assert(ParquetTimeTzLogicalType(ParquetExtraTypeInfo::UNIT_NS) == LogicalTypeId::TIME_TZ);

const char *ParquetPrefetchStrategyToString(ParquetPrefetchStrategy strategy) {
	switch (strategy) {
	case ParquetPrefetchStrategy::WHOLE_GROUP:
		return "whole_group";
	case ParquetPrefetchStrategy::PREFETCH_FILTERS:
		return "prefetch_filters";
	case ParquetPrefetchStrategy::COLUMN_WISE_EAGER:
		return "column_wise_eager";
	case ParquetPrefetchStrategy::PREFETCH_FILTERS_PAGES:
		return "prefetch_filters_pages";
	case ParquetPrefetchStrategy::COLUMN_WISE_EAGER_PAGES:
		return "column_wise_eager_pages";
	case ParquetPrefetchStrategy::NONE:
		return "none";
	default:
		throw InternalException("Invalid ParquetPrefetchStrategy for ParquetPrefetchStrategyToString()");
	}
}

ParquetPrefetchStrategyOption ParquetPrefetchStrategyOptionFromString(const string &value) {
	auto lower = StringUtil::Lower(value);
	if (lower == "auto") {
		return ParquetPrefetchStrategyOption::AUTO;
	}
	if (lower == "whole_group") {
		return ParquetPrefetchStrategyOption::WHOLE_GROUP;
	}
	throw BinderException("Unrecognized prefetch_strategy '%s' (supported: 'auto', 'whole_group')", value);
}

ParquetInt96AsOption ParquetInt96AsOptionFromString(const string &value) {
	auto lower = StringUtil::Lower(value);
	if (lower == "timestamp") {
		return ParquetInt96AsOption::TIMESTAMP;
	}
	if (lower == "timestamp_ns") {
		return ParquetInt96AsOption::TIMESTAMP_NS;
	}
	if (lower == "struct") {
		return ParquetInt96AsOption::STRUCT;
	}
	throw BinderException("Unrecognized int96_as '%s' (supported: 'timestamp', 'timestamp_ns', 'struct')", value);
}

static idx_t ParquetColumnChunkFileOffset(const duckdb_parquet::ColumnChunk &chunk) {
	idx_t offset = NumericCast<idx_t>(chunk.meta_data.data_page_offset);
	if (chunk.meta_data.__isset.dictionary_page_offset) {
		offset = MinValue<idx_t>(offset, NumericCast<idx_t>(chunk.meta_data.dictionary_page_offset));
	}
	if (chunk.meta_data.__isset.index_page_offset) {
		offset = MinValue<idx_t>(offset, NumericCast<idx_t>(chunk.meta_data.index_page_offset));
	}
	return offset;
}

void ParquetLoggerPrefetchMetrics::GeneratePrefetchGroup(ThriftFileTransport &trans,
                                                         vector<ParquetPrefetchColumn> &requested_columns,
                                                         const duckdb_parquet::ColumnChunkList &all_chunks) {
	if (requested_columns.empty()) {
		return;
	}

	// Let's use ReadHead to bucket registrations so we are use we are getting whatever the ThriftFileTransport does
	vector<reference<ReadHead>> bucket_order;
	reference_map_t<ReadHead, vector<ParquetPrefetchColumn>> buckets;
	set<idx_t> registered_offsets;
	for (auto &requested_column : requested_columns) {
		registered_offsets.insert(requested_column.offset);
		auto rh_ptr = trans.GetReadHead(requested_column.offset);
		if (!rh_ptr) {
			continue;
		}
		auto &rh = *rh_ptr;
		auto inserted = buckets.emplace(rh, vector<ParquetPrefetchColumn> {});
		if (inserted.second) {
			bucket_order.emplace_back(rh);
		}
		inserted.first->second.push_back(requested_column);
	}

	for (auto &chunk : all_chunks) {
		// We gotta figure out if we had any hitchhiker
		idx_t chunk_start = ParquetColumnChunkFileOffset(chunk);
		if (registered_offsets.count(chunk_start) > 0) {
			continue;
		}
		idx_t chunk_end = chunk_start + NumericCast<idx_t>(chunk.meta_data.total_compressed_size);
		// Figure out what physical I/O this file offset lives in
		auto rh_ptr = trans.GetReadHead(chunk_start);
		if (!rh_ptr || chunk_end > rh_ptr->GetEnd()) {
			continue;
		}
		auto &rh = *rh_ptr;
		// Hitchhiker only counts if the ReadHead was created for one of our registrations.
		auto bucket_it = buckets.find(rh);
		if (bucket_it == buckets.end()) {
			continue;
		}
		auto &path = chunk.meta_data.path_in_schema;
		string name =
		    path.empty()
		        ? string()
		        : StringUtil::Join(path, ".", [](const duckdb_parquet::ParquetString &part) { return string(part); });
		// we tag hitchhikers with *column_name*
		bucket_it->second.emplace_back("*" + std::move(name) + "*", chunk_start);
	}

	// Issue one log group per ReadHead, on the order they were registered
	for (auto &rh_ref : bucket_order) {
		auto &bucket = buckets[rh_ref];
		std::sort(bucket.begin(), bucket.end());
		vector<string> names;
		names.reserve(bucket.size());
		for (auto &col : bucket) {
			names.push_back(std::move(col.name));
		}
		prefetch_groups.push_back(std::move(names));
	}
	requested_columns.clear();
}

static void LogRowGroupPrefetch(ClientContext &context, const string &file_path, idx_t row_group_id,
                                ParquetReaderScanState &state) {
	const bool fully_filtered = !state.prefetch_metrics.had_match;
	auto &filters_used = state.prefetch_metrics.logger.filters_used;
	vector<string> minimal_filters;
	minimal_filters.reserve(filters_used.size());
	for (idx_t i = 0; i < state.scan_filters.size(); i++) {
		if (i < filters_used.size() && filters_used[i]) {
			auto &scan_filter = state.scan_filters[i];
			MultiFileLocalIndex local_idx(scan_filter.filter_idx);
			minimal_filters.push_back(state.GetColumnReader(local_idx).Schema().name);
		}
	}
	DUCKDB_LOG(context, ParquetPrefetchLogType, file_path, row_group_id, fully_filtered,
	           ParquetPrefetchStrategyToString(state.prefetch_metrics.logger.strategy),
	           state.prefetch_metrics.logger.prefetch_groups, minimal_filters,
	           state.prefetch_metrics.logger.accepted_column_gap);
}

using duckdb_parquet::ColumnChunk;
using duckdb_parquet::ConvertedType;
using duckdb_parquet::FieldRepetitionType;
using duckdb_parquet::FileCryptoMetaData;
using duckdb_parquet::FileMetaData;
using ParquetRowGroup = duckdb_parquet::RowGroup;
using duckdb_parquet::SchemaElement;
using duckdb_parquet::Statistics;
using duckdb_parquet::Type;

static unique_ptr<duckdb_apache::thrift::protocol::TProtocol>
CreateThriftFileProtocol(QueryContext context, CachingFileHandle &file_handle, bool prefetch_mode,
                         uint64_t accepted_column_gap = ReadHeadComparator::DEFAULT_ACCEPTED_COLUMN_GAP) {
	auto transport =
	    duckdb_base_std::make_shared<ThriftFileTransport>(context, file_handle, prefetch_mode, accepted_column_gap);
	// every list element takes at least one byte, so a list cannot hold more elements than the file has bytes
	auto max_list_size = MinValue<idx_t>(transport->GetSize(), NumericLimits<int32_t>::Maximum());
	auto container_limit = NumericCast<int32_t>(max_list_size);
	return make_uniq<duckdb_apache::thrift::protocol::TCompactProtocolT<ThriftFileTransport>>(std::move(transport), 0,
	                                                                                          container_limit);
}

static bool ShouldAndCanPrefetch(ClientContext &context, CachingFileHandle &file_handle) {
	bool disable_prefetch = false;
	context.TryGetCurrentSetting("disable_parquet_prefetching", disable_prefetch);
	// local files also prefetch by default, the async I/O overlaps with decoding
	return file_handle.CanSeek() && !disable_prefetch;
}

//! Coalescing gap for the scan's prefetch I/O, either pinned through a setting or chosen by the cost model
static uint64_t DetermineAcceptedColumnGap(ClientContext &context, ParquetReaderScanState &state) {
	uint64_t gap = ReadHeadComparator::DEFAULT_ACCEPTED_COLUMN_GAP;
	if (!state.prefetch_mode) {
		return gap;
	}
	Value pinned_gap;
	context.TryGetCurrentSetting("parquet_prefetch_column_gap", pinned_gap);
	if (!pinned_gap.IsNull()) {
		return MinValue<uint64_t>(pinned_gap.GetValue<uint64_t>(), PrefetchCostModel::GAP_MAX);
	}
	// remote files measure their requests in the file system, local files in the caching file handle
	NetworkThroughputEstimate estimate;
	if (state.file_handle->TryGetNetworkThroughput(estimate)) {
		state.cost_model_state.RefineFromEstimate(estimate);
	}
	state.cost_model_state.TryGetColumnGapSize(gap);
	return gap;
}

static void ParseParquetFooter(const_data_ptr_t buffer, const string &file_path, idx_t file_size,
                               const shared_ptr<const ParquetEncryptionConfig> &encryption_config, uint32_t &footer_len,
                               bool &footer_encrypted) {
	if (memcmp(buffer + 4, "PAR1", 4) == 0) {
		footer_encrypted = false;
		if (encryption_config) {
			throw InvalidInputException("File '%s' is not encrypted, but 'encryption_config' was set", file_path);
		}
	} else if (memcmp(buffer + 4, "PARE", 4) == 0) {
		footer_encrypted = true;
		if (!encryption_config) {
			throw InvalidInputException("File '%s' is encrypted, but 'encryption_config' was not set", file_path);
		}
	} else {
		throw InvalidInputException("No magic bytes found at end of file '%s'", file_path);
	}

	// read four-byte footer length from just before the end magic bytes
	footer_len = Load<uint32_t>(buffer);
	if (footer_len == 0 || file_size < 12 + footer_len) {
		throw InvalidInputException("Footer length error in file '%s'", file_path);
	}
}

static shared_ptr<ParquetFileMetadataCache>
LoadMetadata(ClientContext &context, Allocator &allocator, CachingFileHandle &file_handle,
             const shared_ptr<const ParquetEncryptionConfig> &encryption_config,
             shared_ptr<EncryptionUtil> &encryption_util, optional_idx footer_size) {
	auto file_proto = CreateThriftFileProtocol(context, file_handle, false);
	auto &transport = reinterpret_cast<ThriftFileTransport &>(*file_proto->getTransport());
	auto file_size = transport.GetSize();
	if (file_size < 12) {
		throw InvalidInputException("File '%s' too small to be a Parquet file", file_handle.GetPath());
	}

	bool footer_encrypted;
	uint32_t footer_len;
	// footer size is not provided - read it from the back
	if (!footer_size.IsValid()) {
		// We have to do two reads here:
		// 1. The 8 bytes from the back to check if it's a Parquet file and the footer size
		// 2. The footer (after getting the size)
		// For local reads this doesn't matter much, but for remote reads this means two round trips,
		// which is especially bad for small Parquet files where the read cost is mostly round trips.
		// So, we prefetch more, to hopefully save a round trip.
		static constexpr idx_t ESTIMATED_FOOTER_RATIO = 1000; // Estimate 1/1000th of the file to be footer
		static constexpr idx_t MIN_PREFETCH_SIZE = 16384;     // Prefetch at least this many bytes
		static constexpr idx_t MAX_PREFETCH_SIZE = 262144;    // Prefetch at most this many bytes
		idx_t prefetch_size = 8;
		if (ShouldAndCanPrefetch(context, file_handle)) {
			prefetch_size = ClampValue(file_size / ESTIMATED_FOOTER_RATIO, MIN_PREFETCH_SIZE, MAX_PREFETCH_SIZE);
			prefetch_size = MinValue(NextPowerOfTwo(prefetch_size), file_size);
		}

		ResizeableBuffer buf;
		buf.resize(allocator, 8);
		buf.zero();

		transport.Prefetch(file_size - prefetch_size, prefetch_size);
		transport.SetLocation(file_size - 8);
		transport.read(buf.ptr, 8);

		ParseParquetFooter(buf.ptr, file_handle.GetPath(), file_size, encryption_config, footer_len, footer_encrypted);

		auto metadata_pos = file_size - (footer_len + 8);
		transport.SetLocation(metadata_pos);
		if (footer_len > prefetch_size - 8) {
			transport.Prefetch(metadata_pos, footer_len);
		}
	} else {
		auto provided_footer_len = footer_size.GetIndex();
		if (provided_footer_len == 0 || provided_footer_len > NumericLimits<uint32_t>::Maximum() ||
		    file_size < 12 + provided_footer_len) {
			throw InvalidInputException("Invalid footer length provided for file '%s'", file_handle.GetPath());
		}
		footer_len = UnsafeNumericCast<uint32_t>(provided_footer_len);

		idx_t total_footer_len = footer_len + 8;
		auto metadata_pos = file_size - total_footer_len;
		transport.SetLocation(metadata_pos);
		transport.Prefetch(metadata_pos, total_footer_len);

		auto read_head = transport.GetReadHead(metadata_pos);
		auto data_ptr = read_head->buffer_ptr;

		uint32_t read_footer_len;
		ParseParquetFooter(data_ptr + footer_len, file_handle.GetPath(), file_size, encryption_config, read_footer_len,
		                   footer_encrypted);
		if (read_footer_len != footer_len) {
			throw InvalidInputException("Parquet footer length stored in file is not equal to footer length provided");
		}
	}

	// CreateThriftFileProtocol bounded the container size limit by the whole file, since footer_len was not yet
	// known at that point. The actual footer (crypto metadata and/or file metadata) can only ever occupy footer_len
	// bytes, which is typically far smaller than the file - tighten the limit now to that true byte budget, so a
	// declared list/map element count can no longer be inflated using unrelated bytes elsewhere in the file.
	auto &compact_proto =
	    reinterpret_cast<duckdb_apache::thrift::protocol::TCompactProtocolT<ThriftFileTransport> &>(*file_proto);
	auto limit = MinValue<idx_t>(footer_len, NumericLimits<int32_t>::Maximum());
	compact_proto.setContainerSizeLimit(NumericCast<int32_t>(limit));

	auto metadata = make_uniq<ParquetMetadata>(Allocator::DefaultAllocator());
	auto &file_metadata = metadata->file_metadata;
	unique_ptr<FileCryptoMetaData> crypto_metadata;
	string encryption_key_hash;

	if (footer_encrypted) {
		// Get the encryption util
		// The parquet reader only reads data, so we set util to true
		encryption_util = context.db->GetEncryptionUtil(true);
		crypto_metadata = make_uniq<FileCryptoMetaData>(metadata->arena);
		crypto_metadata->read(file_proto.get());

		if (crypto_metadata->encryption_algorithm.__isset.AES_GCM_CTR_V1) {
			throw InvalidInputException("File '%s' is encrypted with AES_GCM_CTR_V1, but only AES_GCM_V1 is supported",
			                            file_handle.GetPath());
		}
		auto file_aad = crypto_metadata->encryption_algorithm.AES_GCM_V1.aad_file_unique;
		CryptoMetaData aad_crypto_metadata = CryptoMetaData(allocator);

		if (!file_aad.empty()) {
			aad_crypto_metadata.Initialize(file_aad);
			aad_crypto_metadata.SetModule(ParquetCrypto::FOOTER);
		}
		ParquetCrypto::GenerateAdditionalAuthenticatedData(allocator, aad_crypto_metadata);
		ParquetCrypto::Read(file_metadata, *file_proto, encryption_config->GetFooterKey(), *encryption_util,
		                    aad_crypto_metadata);
		auto hash_util = context.db->GetMbedTLSUtil(false);
		encryption_key_hash = ParquetFileMetadataCache::CreateEncryptionKeyHash(*encryption_config, *hash_util);
	} else {
		file_metadata.read(file_proto.get());
	}

	if (file_metadata.num_rows < 0) {
		throw InvalidInputException("Failed to read Parquet file \"%s\": has negative number of rows",
		                            file_handle.GetPath());
	}

	// Try to read the GeoParquet metadata (if present)
	auto geo_metadata = GeoParquetFileMetadata::TryRead(file_metadata, context);
	return make_shared_ptr<ParquetFileMetadataCache>(std::move(metadata), file_handle, std::move(geo_metadata),
	                                                 std::move(crypto_metadata), std::move(encryption_key_hash),
	                                                 footer_len);
}

static bool CanUseParquetMetadataStatistics(ClientContext &context,
                                            const shared_ptr<ParquetFileMetadataCache> &metadata,
                                            const ParquetOptions &parquet_options) {
	string encryption_key_hash;
	optional_ptr<const string> encryption_key_hash_ptr;
	if (metadata->IsEncrypted() && parquet_options.encryption_config) {
		auto hash_util = context.db->GetMbedTLSUtil(false);
		encryption_key_hash =
		    ParquetFileMetadataCache::CreateEncryptionKeyHash(*parquet_options.encryption_config, *hash_util);
		encryption_key_hash_ptr = encryption_key_hash;
	}
	return metadata->CanUseMetadataStatistics(parquet_options.encryption_config, encryption_key_hash_ptr);
}

LogicalType ParquetReader::DeriveLogicalType(const SchemaElement &s_ele, ParquetColumnSchema &schema) const {
	return DeriveLogicalType(s_ele, parquet_options, schema);
}

LogicalType ParquetReader::DeriveLogicalType(const SchemaElement &s_ele, const ParquetOptions &parquet_options,
                                             ParquetColumnSchema &schema) {
	// inner node
	if (s_ele.type == Type::FIXED_LEN_BYTE_ARRAY && !s_ele.__isset.type_length) {
		throw IOException("FIXED_LEN_BYTE_ARRAY requires length to be set");
	}
	if (s_ele.__isset.type_length) {
		if (s_ele.type_length < 0) {
			throw IOException("Invalid negative type_length %d for column \"%s\"", s_ele.type_length,
			                  string(s_ele.name));
		}
		schema.type_length = NumericCast<uint32_t>(s_ele.type_length);
	}
	schema.parquet_type = s_ele.type;
	if (s_ele.__isset.logicalType) {
		if (s_ele.logicalType.__isset.UNKNOWN) {
			return LogicalType::SQLNULL;
		} else if (s_ele.logicalType.__isset.UUID) {
			if (s_ele.type == Type::FIXED_LEN_BYTE_ARRAY) {
				return LogicalType::UUID;
			}
		} else if (s_ele.logicalType.__isset.FLOAT16) {
			if (s_ele.type == Type::FIXED_LEN_BYTE_ARRAY && s_ele.type_length == 2) {
				schema.type_info = ParquetExtraTypeInfo::FLOAT16;
				return LogicalType::FLOAT;
			}
		} else if (s_ele.logicalType.__isset.TIMESTAMP) {
			if (s_ele.logicalType.TIMESTAMP.unit.__isset.MILLIS) {
				schema.type_info = ParquetExtraTypeInfo::UNIT_MS;
			} else if (s_ele.logicalType.TIMESTAMP.unit.__isset.MICROS) {
				schema.type_info = ParquetExtraTypeInfo::UNIT_MICROS;
			} else if (s_ele.logicalType.TIMESTAMP.unit.__isset.NANOS) {
				schema.type_info = ParquetExtraTypeInfo::UNIT_NS;
			} else {
				throw NotImplementedException("Unimplemented TIMESTAMP encoding - missing UNIT");
			}
			if (s_ele.logicalType.TIMESTAMP.isAdjustedToUTC) {
				return LogicalType(ParquetTimestampTzLogicalType(schema.type_info));
			}
			return LogicalType(ParquetTimestampLogicalType(schema.type_info));
		} else if (s_ele.logicalType.__isset.TIME) {
			if (s_ele.logicalType.TIME.unit.__isset.MILLIS) {
				schema.type_info = ParquetExtraTypeInfo::UNIT_MS;
			} else if (s_ele.logicalType.TIME.unit.__isset.MICROS) {
				schema.type_info = ParquetExtraTypeInfo::UNIT_MICROS;
			} else if (s_ele.logicalType.TIME.unit.__isset.NANOS) {
				schema.type_info = ParquetExtraTypeInfo::UNIT_NS;
			} else {
				throw NotImplementedException("Unimplemented TIME encoding - missing UNIT");
			}
			if (s_ele.logicalType.TIME.isAdjustedToUTC) {
				return LogicalType(ParquetTimeTzLogicalType(schema.type_info));
			}
			return LogicalType(ParquetTimeLogicalType(schema.type_info));
		}
	}
	if (s_ele.__isset.converted_type) {
		// Legacy NULL type, does no longer exist, but files are still around of course
		if (static_cast<uint8_t>(s_ele.converted_type) == 24) {
			return LogicalTypeId::SQLNULL;
		}
		switch (s_ele.converted_type) {
		case ConvertedType::INT_8:
			if (s_ele.type == Type::INT32) {
				return LogicalType::TINYINT;
			} else {
				throw IOException("INT8 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::INT_16:
			if (s_ele.type == Type::INT32) {
				return LogicalType::SMALLINT;
			} else {
				throw IOException("INT16 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::INT_32:
			if (s_ele.type == Type::INT32) {
				return LogicalType::INTEGER;
			} else {
				throw IOException("INT32 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::INT_64:
			if (s_ele.type == Type::INT64) {
				return LogicalType::BIGINT;
			} else {
				throw IOException("INT64 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::UINT_8:
			if (s_ele.type == Type::INT32) {
				return LogicalType::UTINYINT;
			} else {
				throw IOException("UINT8 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::UINT_16:
			if (s_ele.type == Type::INT32) {
				return LogicalType::USMALLINT;
			} else {
				throw IOException("UINT16 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::UINT_32:
			if (s_ele.type == Type::INT32) {
				return LogicalType::UINTEGER;
			} else {
				throw IOException("UINT32 converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::UINT_64:
			if (s_ele.type == Type::INT64) {
				return LogicalType::UBIGINT;
			} else {
				throw IOException("UINT64 converted type can only be set for value of Type::INT64");
			}
		case ConvertedType::DATE:
			if (s_ele.type == Type::INT32) {
				return LogicalType::DATE;
			} else {
				throw IOException("DATE converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::TIMESTAMP_MICROS:
			schema.type_info = ParquetExtraTypeInfo::UNIT_MICROS;
			if (s_ele.type == Type::INT64) {
				return LogicalType(ParquetTimestampLogicalType(schema.type_info));
			} else {
				throw IOException("TIMESTAMP converted type can only be set for value of Type::INT64");
			}
		case ConvertedType::TIMESTAMP_MILLIS:
			schema.type_info = ParquetExtraTypeInfo::UNIT_MS;
			if (s_ele.type == Type::INT64) {
				return LogicalType(ParquetTimestampLogicalType(schema.type_info));
			} else {
				throw IOException("TIMESTAMP converted type can only be set for value of Type::INT64");
			}
		case ConvertedType::DECIMAL:
			if (!s_ele.__isset.precision || !s_ele.__isset.scale) {
				throw IOException("DECIMAL requires a length and scale specifier!");
			}
			if (s_ele.precision < 1 || s_ele.scale < 0 || s_ele.scale > s_ele.precision) {
				throw IOException("Invalid DECIMAL precision %d and scale %d", s_ele.precision, s_ele.scale);
			}
			schema.type_scale = NumericCast<uint32_t>(s_ele.scale);
			if (s_ele.precision > DecimalType::MaxWidth()) {
				schema.type_info = ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY;
				return LogicalType::DOUBLE;
			}
			switch (s_ele.type) {
			case Type::BYTE_ARRAY:
			case Type::FIXED_LEN_BYTE_ARRAY:
				schema.type_info = ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY;
				break;
			case Type::INT32:
				schema.type_info = ParquetExtraTypeInfo::DECIMAL_INT32;
				break;
			case Type::INT64:
				schema.type_info = ParquetExtraTypeInfo::DECIMAL_INT64;
				break;
			default:
				throw IOException(
				    "DECIMAL converted type can only be set for value of Type::(FIXED_LEN_)BYTE_ARRAY/INT32/INT64");
			}
			return LogicalType::DECIMAL(s_ele.precision, s_ele.scale);
		case ConvertedType::UTF8:
		case ConvertedType::ENUM:
			switch (s_ele.type) {
			case Type::BYTE_ARRAY:
			case Type::FIXED_LEN_BYTE_ARRAY:
				return LogicalType::VARCHAR;
			default:
				throw IOException("UTF8 converted type can only be set for Type::(FIXED_LEN_)BYTE_ARRAY");
			}
		case ConvertedType::TIME_MILLIS:
			schema.type_info = ParquetExtraTypeInfo::UNIT_MS;
			if (s_ele.type == Type::INT32) {
				return LogicalType(ParquetTimeLogicalType(schema.type_info));
			} else {
				throw IOException("TIME_MILLIS converted type can only be set for value of Type::INT32");
			}
		case ConvertedType::TIME_MICROS:
			schema.type_info = ParquetExtraTypeInfo::UNIT_MICROS;
			if (s_ele.type == Type::INT64) {
				return LogicalType(ParquetTimeLogicalType(schema.type_info));
			} else {
				throw IOException("TIME_MICROS converted type can only be set for value of Type::INT64");
			}
		case ConvertedType::INTERVAL:
			if (s_ele.type != Type::FIXED_LEN_BYTE_ARRAY || s_ele.type_length != 12) {
				throw DataCorruptionException(
				    "INTERVAL converted type can only be set for value of Type::FIXED_LEN_BYTE_ARRAY of length 12");
			}
			return LogicalType::INTERVAL;
		case ConvertedType::JSON:
			return LogicalType::JSON();
		case ConvertedType::MAP:
		case ConvertedType::MAP_KEY_VALUE:
		case ConvertedType::LIST:
		case ConvertedType::BSON:
		default:
			throw IOException("Unsupported converted type (%d)", (int32_t)s_ele.converted_type);
		}
	} else {
		// no converted type set
		// use default type for each physical type
		switch (s_ele.type) {
		case Type::BOOLEAN:
			return LogicalType::BOOLEAN;
		case Type::INT32:
			return LogicalType::INTEGER;
		case Type::INT64:
			return LogicalType::BIGINT;
		case Type::INT96: // always a timestamp it would seem
			schema.type_info = ParquetExtraTypeInfo::IMPALA_TIMESTAMP;
			switch (parquet_options.int96_as) {
			case ParquetInt96AsOption::TIMESTAMP:
				return LogicalType::TIMESTAMP;
			case ParquetInt96AsOption::TIMESTAMP_NS:
				return LogicalType::TIMESTAMP_NS;
			case ParquetInt96AsOption::STRUCT:
				return LogicalType::STRUCT({{"date", LogicalType::DATE}, {"time", LogicalType::TIME_NS}});
			default:
				throw InternalException("Unrecognized int96_as option");
			}
		case Type::FLOAT:
			return LogicalType::FLOAT;
		case Type::DOUBLE:
			return LogicalType::DOUBLE;
		case Type::BYTE_ARRAY:
		case Type::FIXED_LEN_BYTE_ARRAY:
			if (parquet_options.binary_as_string) {
				return LogicalType::VARCHAR;
			}
			return LogicalType::BLOB;
		default:
			return LogicalType::INVALID;
		}
	}
}

ParquetColumnSchema ParquetReader::ParseColumnSchema(const SchemaElement &s_ele, idx_t max_define, idx_t max_repeat,
                                                     idx_t schema_index, idx_t column_index,
                                                     ParquetColumnSchemaType type) {
	return ParquetColumnSchema::FromSchemaElement(s_ele, max_define, max_repeat, schema_index, column_index, type,
	                                              parquet_options);
}

static unique_ptr<BaseStatistics> ReadColumnStatistics(const FileMetaData &file_meta_data,
                                                       const ParquetColumnSchema &column,
                                                       const ParquetOptions &parquet_options) {
	unique_ptr<BaseStatistics> column_stats;

	for (idx_t row_group_idx = 0; row_group_idx < file_meta_data.row_groups.size(); row_group_idx++) {
		auto &row_group = file_meta_data.row_groups[row_group_idx];
		auto chunk_stats = column.Stats(file_meta_data, parquet_options, row_group_idx, row_group.columns);
		if (!chunk_stats) {
			return nullptr;
		}
		if (!column_stats) {
			column_stats = std::move(chunk_stats);
		} else {
			column_stats->Merge(*chunk_stats);
		}
	}
	return column_stats;
}

static unique_ptr<BaseStatistics> ReadStatisticsInternal(const FileMetaData &file_meta_data,
                                                         const ParquetColumnSchema &root_schema,
                                                         const ParquetOptions &parquet_options,
                                                         const idx_t &file_col_idx) {
	return ReadColumnStatistics(file_meta_data, root_schema.children[file_col_idx], parquet_options);
}

unique_ptr<BaseStatistics> ParquetReader::ReadStatistics(const Identifier &name) {
	if (!can_use_metadata_statistics) {
		return nullptr;
	}
	idx_t file_col_idx;
	for (file_col_idx = 0; file_col_idx < columns.size(); file_col_idx++) {
		if (columns[file_col_idx].name == name) {
			break;
		}
	}
	if (file_col_idx == columns.size()) {
		return nullptr;
	}

	return ReadStatisticsInternal(*GetFileMetadata(), *root_schema, parquet_options, file_col_idx);
}

unique_ptr<BaseStatistics> ParquetReader::ReadStatistics(ClientContext &context, ParquetOptions parquet_options,
                                                         shared_ptr<ParquetFileMetadataCache> metadata,
                                                         const Identifier &name) {
	if (!CanUseParquetMetadataStatistics(context, metadata, parquet_options)) {
		return nullptr;
	}
	ParquetReader reader(context, std::move(parquet_options), std::move(metadata));
	return reader.ReadStatistics(name);
}

unique_ptr<BaseStatistics>
ParquetReader::ReadVirtualColumnStatistics(ClientContext &context, const ParquetOptions &parquet_options,
                                           const shared_ptr<ParquetFileMetadataCache> &metadata,
                                           column_t virtual_column_id) {
	if (virtual_column_id != MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER || !metadata ||
	    !CanUseParquetMetadataStatistics(context, metadata, parquet_options)) {
		return nullptr;
	}
	// the row numbers of every row group follow on from those of the row groups before it
	return ReadColumnStatistics(metadata->metadata->file_metadata, ParquetColumnSchema::FileRowNumber(),
	                            parquet_options);
}

unique_ptr<BaseStatistics> ParquetReader::GetVirtualColumnStatistics(ClientContext &context,
                                                                     column_t virtual_column_id) {
	return ReadVirtualColumnStatistics(context, parquet_options, metadata, virtual_column_id);
}

unique_ptr<BaseStatistics> ParquetReader::ReadStatistics(ClientContext &context, const ParquetUnionData &union_data,
                                                         const Identifier &name) {
	if (!CanUseParquetMetadataStatistics(context, union_data.metadata, union_data.options)) {
		return nullptr;
	}
	const auto &col_names = union_data.names;

	idx_t file_col_idx;
	for (file_col_idx = 0; file_col_idx < col_names.size(); file_col_idx++) {
		if (col_names[file_col_idx] == name) {
			break;
		}
	}
	if (file_col_idx == col_names.size()) {
		return nullptr;
	}

	return ReadStatisticsInternal(union_data.metadata->metadata->file_metadata, *union_data.root_schema,
	                              union_data.options, file_col_idx);
}

static bool IsFullyShredded(const BaseStatistics &variant_stats, const ColumnIndex &column_index) {
	D_ASSERT(column_index.IsPushdownExtract());
	return VariantStats::IsShredded(variant_stats, column_index);
}

optional_ptr<const BaseStatistics> ParquetReader::GetVariantStats(const ParquetColumnSchema &schema) const {
	D_ASSERT(schema.schema_type == ParquetColumnSchemaType::VARIANT);
	D_ASSERT(schema.schema_index.IsValid());
	const auto cache_key = schema.schema_index.GetIndex();

	{
		const annotated_lock_guard<annotated_mutex> guard(variant_stats_lock);
		auto entry = variant_stats_cache.find(cache_key);
		if (entry != variant_stats_cache.end()) {
			D_ASSERT(entry->second);
			return *entry->second;
		}
	}

	auto variant_stats = ReadColumnStatistics(*GetFileMetadata(), schema, parquet_options);
	if (!variant_stats) {
		return nullptr;
	}

	const annotated_lock_guard<annotated_mutex> guard(variant_stats_lock);
	auto res = variant_stats_cache.emplace(cache_key, std::move(variant_stats));
	return *res.first->second;
}

//! Whether the cast of the extracted field can be applied to its 'typed_value' instead of to the VARIANT
static bool TypedValueCastMatchesVariantCast(const ParquetColumnSchema &schema, const ColumnIndex &column_id) {
	D_ASSERT(schema.name == "typed_value");
	reference<const ParquetColumnSchema> typed_value(schema);
	reference<const ColumnIndex> path_iter(column_id);
	while (path_iter.get().HasChildren()) {
		auto &child = path_iter.get().GetChildIndexes()[0];
		auto child_column_index = typed_value.get().GetChildIndexByName(child.GetFieldName());
		if (!child_column_index.IsValid()) {
			return false;
		}
		auto &child_column = typed_value.get().GetChildByIndex(child_column_index.GetIndex());
		auto typed_value_index = child_column.GetChildIndexByName("typed_value");
		if (!typed_value_index.IsValid()) {
			return false;
		}
		typed_value = child_column.GetChildByIndex(typed_value_index.GetIndex());
		path_iter = child;
	}
	return VariantUtils::ShreddedCastMatchesVariantCast(typed_value.get().type, path_iter.get().GetType());
}

static ColumnIndex CreateVariantTypedValuePushdown(const ParquetColumnSchema &schema, const ColumnIndex &column_id) {
	D_ASSERT(schema.name == "typed_value");
	reference<const ParquetColumnSchema> typed_value(schema);

	reference<const ColumnIndex> path_iter(column_id);
	ColumnIndex result_index(0);
	result_index.SetType(schema.type);
	reference<ColumnIndex> result(result_index);
	while (path_iter.get().HasChildren()) {
		auto &child = path_iter.get().GetChildIndexes()[0];
		auto &field_name = child.GetFieldName();
		auto child_column_index = typed_value.get().GetChildIndexByName(field_name);
		if (!child_column_index.IsValid()) {
			throw InternalException("Can't locate the child by name '%s' in the VARIANT column", field_name);
		}
		auto &child_column = typed_value.get().GetChildByIndex(child_column_index.GetIndex());
		if (!StructType::IsStruct(child_column.type)) {
			throw InternalException("Extracted field for '%s' from 'typed_value', is not a struct (received: %s)",
			                        field_name, child_column.type.ToString());
		}
		auto typed_value_index = child_column.GetChildIndexByName("typed_value");
		if (!typed_value_index.IsValid()) {
			throw InternalException("Can't find 'typed_value' inside type %s", child_column.type.ToString());
		}
		auto &typed_value_column = child_column.GetChildByIndex(typed_value_index.GetIndex());

		//! <field_name>
		ColumnIndex index(child_column_index.GetIndex());
		index.SetType(child_column.type);
		result.get().AddChildIndex(std::move(index));
		result.get().SetPushdownExtract();
		result = result.get().GetChildIndexesMutable()[0];

		//! <field_name>.typed_value
		ColumnIndex nested_typed_value(typed_value_index.GetIndex());
		nested_typed_value.SetType(child.GetType());
		result.get().AddChildIndex(std::move(nested_typed_value));
		result.get().SetPushdownExtract();
		result = result.get().GetChildIndexesMutable()[0];

		typed_value = typed_value_column;
		path_iter = child;
	}
	result_index.SetPushdownExtract();
	return result_index;
}

static unique_ptr<ColumnReader> CreateInt96StructReader(ClientContext &context, const ParquetReader &reader,
                                                        const ParquetColumnSchema &schema,
                                                        optional_ptr<const ColumnIndex> pushdown_child) {
	// Read the raw 12-byte INT96 value as a BLOB - this is lossless over the full range INT96 can express
	// (0001-01-01 through 9999-12-31), unlike TIMESTAMP/TIMESTAMP_NS which clamp or truncate
	// The blob schema is a copy of the leaf schema with a different type; it is kept alive on the
	// expression reader's heap (the ColumnReader base class only holds a reference to it)
	if (pushdown_child) {
		// The scan only exposes a single child of the INT96-as-struct column (e.g. ts['date']).
		// Follow the same pattern as a pushed-down extract on a regular STRUCT column: wrap the leaf in an
		// EXPRESSION schema carrying the child's type so the reader reports the child type and produces no
		// (struct) row group statistics, which a filter on the child would otherwise be checked against.
		auto expr = CreateInt96AsStructChildExpression(context, pushdown_child->GetPrimaryIndex());
		if (expr->GetReturnType() != pushdown_child->GetType()) {
			expr = BoundCastExpression::AddCastToType(context, std::move(expr), pushdown_child->GetType());
		}
		auto expr_schema = make_uniq<ParquetColumnSchema>(
		    ParquetColumnSchema::FromParentSchema(schema, expr->GetReturnType(), ParquetColumnSchemaType::EXPRESSION));
		// FromParentSchema embeds a copy of the leaf schema as its first child - retype that copy to BLOB so the
		// embedded child is the raw INT96 leaf and stays alive through the expression schema's ownership
		expr_schema->children[0].type = LogicalType::BLOB;
		vector<unique_ptr<ColumnReader>> children;
		children.push_back(make_uniq<StringColumnReader>(reader, expr_schema->children[0]));
		return make_uniq<ExpressionColumnReader>(context, std::move(children), std::move(expr), std::move(expr_schema));
	}
	auto child_schema = make_uniq<ParquetColumnSchema>(schema);
	child_schema->type = LogicalType::BLOB;
	auto blob_reader = make_uniq<StringColumnReader>(reader, *child_schema);

	vector<unique_ptr<ColumnReader>> children;
	children.push_back(std::move(blob_reader));
	auto expr = CreateInt96AsStructExpression(context);
	auto result = make_uniq<ExpressionColumnReader>(context, std::move(children), std::move(expr), schema);
	result->owned_schema = std::move(child_schema);
	return std::move(result);
}

unique_ptr<ColumnReader> ParquetReader::CreateReaderRecursive(ClientContext &context, const ColumnIndex &column_id,
                                                              const ParquetColumnSchema &schema) const {
	auto &indexes = column_id.GetChildIndexes();

	switch (schema.schema_type) {
	case ParquetColumnSchemaType::FILE_ROW_NUMBER:
		return make_uniq<RowNumberColumnReader>(*this, schema);
	case ParquetColumnSchemaType::FILE_ROW_GROUP_NUMBER:
		return make_uniq<RowGroupColumnReader>(*this, schema);
	case ParquetColumnSchemaType::GEOMETRY: {
		return GeometryColumnReader::Create(*this, schema, context);
	}
	case ParquetColumnSchemaType::COLUMN: {
		if (schema.children.empty()) {
			if (schema.parquet_type == Type::INT96 && schema.type.id() == LogicalTypeId::STRUCT) {
				// int96_as='struct' exposes the raw INT96 value as STRUCT(date DATE, time TIME_NS)
				return CreateInt96StructReader(context, *this, schema,
				                               column_id.IsPushdownExtract() ? &indexes[0] : nullptr);
			}
			// leaf reader
			return ColumnReader::CreateReader(*this, schema);
		}
		vector<unique_ptr<ColumnReader>> children;
		children.resize(schema.children.size());

		if (indexes.empty()) {
			for (idx_t child_index = 0; child_index < schema.children.size(); child_index++) {
				children[child_index] =
				    CreateReaderRecursive(context, ColumnIndex(child_index), schema.children[child_index]);
			}
		} else {
			for (idx_t i = 0; i < indexes.size(); i++) {
				auto child_index = indexes[i].GetPrimaryIndex();
				children[child_index] = CreateReaderRecursive(context, indexes[i], schema.children[child_index]);
			}
		}
		switch (schema.type.id()) {
		case LogicalTypeId::LIST:
		case LogicalTypeId::MAP:
			D_ASSERT(children.size() == 1);
			return make_uniq<ListColumnReader>(*this, schema, std::move(children[0]));
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::TUPLE: {
			if (column_id.IsPushdownExtract()) {
				auto &child = indexes[0];
				auto child_index = child.GetPrimaryIndex();
				auto column_reader = std::move(children[child_index]);
				auto &child_type = column_reader->Type();
				if (!child.HasChildren() && child_type != child.GetType()) {
					auto input = make_uniq<BoundReferenceExpression>(child_type, 0ULL);
					auto cast_expression =
					    BoundCastExpression::AddCastToType(context, std::move(input), child.GetType());
					auto expr_schema = make_uniq<ParquetColumnSchema>(
					    ParquetColumnSchema::FromParentSchema(column_reader->Schema(), cast_expression->GetReturnType(),
					                                          ParquetColumnSchemaType::EXPRESSION));

					vector<unique_ptr<ColumnReader>> expression_children;
					expression_children.push_back(std::move(column_reader));
					auto expr_reader = make_uniq<ExpressionColumnReader>(
					    context, std::move(expression_children), std::move(cast_expression), std::move(expr_schema));
					return std::move(expr_reader);
				}
				return column_reader;
			}
			return make_uniq<StructColumnReader>(*this, schema, std::move(children));
		}
		default:
			throw InternalException("Unsupported schema type for schema with children");
		}
	}
	case ParquetColumnSchemaType::VARIANT: {
		if (schema.children.size() < 2) {
			throw InternalException("VARIANT schema type used for a non-variant type column");
		}
		vector<unique_ptr<ColumnReader>> children;
		children.resize(schema.children.size());
		if (schema.children.size() == 3 && column_id.IsPushdownExtract()) {
			//! VARIANT is shredded - it has a 'typed_value' column
			auto &typed_value_schema = schema.children[2];
			if (typed_value_schema.name != "typed_value") {
				throw InvalidInputException("Variant group can only contain 'value'/'typed_value', found '%s'",
				                            typed_value_schema.name);
			}
			auto variant_stats = GetVariantStats(schema);

			if (variant_stats && IsFullyShredded(*variant_stats, column_id) &&
			    TypedValueCastMatchesVariantCast(typed_value_schema, column_id)) {
				//! This field is present in 'typed_value' across all rowgroups
				//! So we can directly push a struct extract into 'typed_value' and ignore 'value'+'metadata'
				auto typed_value_index = CreateVariantTypedValuePushdown(typed_value_schema, column_id);
				return CreateReaderRecursive(context, typed_value_index, typed_value_schema);
			}
		}
		for (idx_t child_index = 0; child_index < schema.children.size(); child_index++) {
			children[child_index] =
			    CreateReaderRecursive(context, ColumnIndex(child_index), schema.children[child_index]);
		}
		// Create the VariantColumnReader with the column index, so we can perform the extract at Read
		auto column_reader = make_uniq<VariantColumnReader>(context, *this, schema, std::move(children), column_id);

		if (!column_id.IsPushdownExtract() || column_id.GetScanType().id() == LogicalTypeId::VARIANT) {
			return std::move(column_reader);
		}
		const auto &scan_type = column_id.GetScanType();
		auto input = make_uniq<BoundReferenceExpression>(LogicalType::VARIANT(), 0ULL);
		auto cast_expression = BoundCastExpression::AddCastToType(context, std::move(input), scan_type);
		auto expr_schema = make_uniq<ParquetColumnSchema>(ParquetColumnSchema::FromParentSchema(
		    column_reader->Schema(), cast_expression->GetReturnType(), ParquetColumnSchemaType::EXPRESSION));
		vector<unique_ptr<ColumnReader>> child_readers;
		child_readers.push_back(std::move(column_reader));
		return make_uniq<ExpressionColumnReader>(context, std::move(child_readers), std::move(cast_expression),
		                                         std::move(expr_schema));
	}
	default:
		throw InternalException("Unsupported ParquetColumnSchemaType");
	}
}

static bool IsGeometryType(const SchemaElement &s_ele, const ParquetFileMetadataCache &metadata, idx_t depth,
                           string &crs) {
	const auto is_blob = s_ele.__isset.type && s_ele.type == Type::BYTE_ARRAY;
	if (!is_blob) {
		return false;
	}

	// TODO: Handle CRS in the future
	const auto is_native_geom = s_ele.__isset.logicalType && s_ele.logicalType.__isset.GEOMETRY;
	if (is_native_geom) {
		if (s_ele.logicalType.GEOMETRY.__isset.crs) {
			crs = s_ele.logicalType.GEOMETRY.crs;
		}
		return true;
	}

	const auto is_native_geog = s_ele.__isset.logicalType && s_ele.logicalType.__isset.GEOGRAPHY;
	if (is_native_geog) {
		if (s_ele.logicalType.GEOGRAPHY.__isset.crs) {
			crs = s_ele.logicalType.GEOGRAPHY.crs;
		}
		return true;
	}

	// geoparquet types have to be at the root of the schema, and have to be present in the kv metadata.
	const auto is_at_root = depth == 1;
	const auto is_in_gpq_metadata = metadata.geo_metadata && metadata.geo_metadata->IsGeometryColumn(s_ele.name);
	// A leaf node has a type set (as per Parquet spec)
	const auto is_leaf = s_ele.__isset.type;
	const auto is_geoparquet_geom = is_at_root && is_in_gpq_metadata && is_leaf;

	if (is_geoparquet_geom) {
		auto meta_ptr = metadata.geo_metadata->GetColumnMeta(s_ele.name);
		if (meta_ptr) {
			crs = meta_ptr->projjson;
		}
		return true;
	}

	return false;
}

static bool IsVariantType(const SchemaElement &root, const vector<ParquetColumnSchema> &children) {
	if (children.size() < 2) {
		return false;
	}
	//! Names have to be 'metadata' and 'value' respectively
	//! But apparently some writers can mix the order, so we are more lenient
	if (children[0].name != "metadata" && children[1].name != "metadata") {
		return false;
	}
	if (children[0].name != "value" && children[1].name != "value") {
		return false;
	}

	//! Verify types
	if (children[0].parquet_type != duckdb_parquet::Type::BYTE_ARRAY) {
		return false;
	}
	if (children[1].parquet_type != duckdb_parquet::Type::BYTE_ARRAY) {
		return false;
	}
	if (children.size() == 3) {
		auto &typed_value = children[2];
		if (typed_value.name != "typed_value") {
			return false;
		}
		throw NotImplementedException("Shredded Variants are not supported yet");
	} else if (children.size() != 2) {
		return false;
	}
	return true;
}

//! The maximum nesting depth of a Parquet schema - deeper schemas would overflow the stack while being read
static constexpr idx_t MAX_PARQUET_SCHEMA_DEPTH = 500;

ParquetColumnSchema ParquetReader::ParseSchemaRecursive(idx_t depth, idx_t max_define, idx_t max_repeat,
                                                        idx_t &next_schema_idx, idx_t &next_file_idx,
                                                        ClientContext &context) {
	auto file_meta_data = GetFileMetadata();
	D_ASSERT(file_meta_data);
	if (depth > MAX_PARQUET_SCHEMA_DEPTH) {
		throw InvalidInputException(
		    "Malformed Parquet schema in file \"%s\": the schema is nested more than %llu levels "
		    "deep",
		    file.path, MAX_PARQUET_SCHEMA_DEPTH);
	}
	if (next_schema_idx >= file_meta_data->schema.size()) {
		throw InvalidInputException("Malformed Parquet schema in file \"%s\": invalid schema index %d", file.path,
		                            next_schema_idx);
	}
	auto &s_ele = file_meta_data->schema[next_schema_idx];
	auto this_idx = next_schema_idx;

	auto repetition_type = FieldRepetitionType::REQUIRED;
	if (s_ele.__isset.repetition_type && this_idx > 0) {
		repetition_type = s_ele.repetition_type;
	}
	if (repetition_type != FieldRepetitionType::REQUIRED) {
		max_define++;
	}
	if (repetition_type == FieldRepetitionType::REPEATED) {
		max_repeat++;
	}

	// Check for geometry type
	string crs;
	if (IsGeometryType(s_ele, *metadata, depth, crs)) {
		// Geometries in both GeoParquet and native parquet are stored as a WKB-encoded BLOB.
		// Because we don't just want to validate that the WKB encoding is correct, but also transform it into
		// little-endian if necessary, we cant just make use of the StringColumnReader without heavily modifying it.
		// Therefore, we create a dedicated GEOMETRY parquet column schema type, which wraps the underlying BLOB column.
		// This schema type gets instantiated as a ExpressionColumnReader on top of the standard Blob/String reader,
		// which performs the WKB validation/transformation using the `ST_GeomFromWKB` function of DuckDB.
		// This enables us to also support other geometry encodings (such as GeoArrow geometries) easier in the future.

		// Try to parse the CRS
		LogicalType target_type;

		auto lookup = CoordinateReferenceSystem::TryIdentify(context, crs);
		if (lookup) {
			target_type = LogicalType::GEOMETRY(*lookup);
		} else {
			target_type = LogicalType::GEOMETRY();
		}

		// Inner BLOB schema
		vector<ParquetColumnSchema> geometry_child;
		geometry_child.emplace_back(ParseColumnSchema(s_ele, max_define, max_repeat, this_idx, next_file_idx));

		// Wrap in geometry schema
		return ParquetColumnSchema::FromChildSchemas(s_ele.name, target_type, max_define, max_repeat, this_idx,
		                                             next_file_idx++, std::move(geometry_child),
		                                             ParquetColumnSchemaType::GEOMETRY);
	}

	// Determine if this is an inner node
	// According to Parquet spec: nodes without 'type' set are inner nodes (groups)
	// For backwards compatibility with non-standard files, also check the old condition
	bool is_inner_node = !s_ele.__isset.type || (s_ele.__isset.num_children && s_ele.num_children > 0);

	if (is_inner_node && s_ele.__isset.type) {
		// This case handles non-standard files where both type and num_children are set
		// Prioritize num_children if set and > 0
		is_inner_node = s_ele.__isset.num_children && s_ele.num_children > 0;
	}

	if (is_inner_node) { // inner node
		vector<ParquetColumnSchema> child_schemas;

		idx_t c_idx = 0;
		idx_t num_children = 0;
		if (s_ele.__isset.num_children && !TryCast::Operation(s_ele.num_children, num_children)) {
			throw InvalidInputException(
			    "Failed to read Parquet file \"%s\": schema element has negative number of children", file.path);
		}
		while (c_idx < num_children) {
			next_schema_idx++;

			auto child_schema =
			    ParseSchemaRecursive(depth + 1, max_define, max_repeat, next_schema_idx, next_file_idx, context);
			child_schemas.push_back(std::move(child_schema));
			c_idx++;
		}
		// rename child type entries if there are case-insensitive duplicates by appending _1, _2 etc.
		// behavior consistent with CSV reader fwiw
		case_insensitive_map_t<idx_t> name_collision_count;
		for (auto &child_schema : child_schemas) {
			auto &col_name = child_schema.name;
			// avoid duplicate header names
			while (name_collision_count.find(col_name) != name_collision_count.end()) {
				name_collision_count[col_name] += 1;
				col_name = col_name + "_" + to_string(name_collision_count[col_name]);
			}
			child_schema.name = col_name;
			name_collision_count[col_name] = 0;
		}

		bool is_repeated = repetition_type == FieldRepetitionType::REPEATED;
		const bool is_list = s_ele.__isset.converted_type && s_ele.converted_type == ConvertedType::LIST;
		const bool is_map = s_ele.__isset.converted_type && s_ele.converted_type == ConvertedType::MAP;
		bool is_map_kv = s_ele.__isset.converted_type && s_ele.converted_type == ConvertedType::MAP_KEY_VALUE;
		bool is_variant = s_ele.__isset.logicalType && s_ele.logicalType.__isset.VARIANT == true;
		if (!is_variant && parquet_options.variant_legacy_encoding && IsVariantType(s_ele, child_schemas)) {
			is_variant = true;
		}
		if (is_variant) {
			// the VARIANT reader reads the 'metadata' and 'value' columns as binary
			for (auto &child_schema : child_schemas) {
				if (child_schema.name != "metadata" && child_schema.name != "value") {
					continue;
				}
				// a logical type annotation (e.g. GEOMETRY) would make the column reader produce another type
				const auto is_binary =
				    child_schema.type.id() == LogicalTypeId::BLOB || child_schema.type.id() == LogicalTypeId::VARCHAR;
				if (!child_schema.children.empty() || child_schema.parquet_type != duckdb_parquet::Type::BYTE_ARRAY ||
				    !is_binary || child_schema.schema_type != ParquetColumnSchemaType::COLUMN) {
					throw DataCorruptionException(
					    "Failed to read Parquet file \"%s\": the '%s' column of VARIANT column \"%s\" "
					    "must be a binary BYTE_ARRAY column",
					    file.path, child_schema.name, string(s_ele.name));
				}
			}
		}

		if (!is_map_kv && this_idx > 0) {
			// check if the parent node of this is a map
			auto &p_ele = file_meta_data->schema[this_idx - 1];
			bool parent_is_map = p_ele.__isset.converted_type && p_ele.converted_type == ConvertedType::MAP;
			bool parent_has_children = p_ele.__isset.num_children && p_ele.num_children == 1;
			is_map_kv = parent_is_map && parent_has_children;
		}

		if (is_map_kv) {
			if (child_schemas.size() != 2) {
				throw IOException("MAP_KEY_VALUE requires two children");
			}
			if (!is_repeated) {
				throw IOException("MAP_KEY_VALUE needs to be repeated");
			}
			auto result_type = LogicalType::MAP(child_schemas[0].type, child_schemas[1].type);
			vector<ParquetColumnSchema> map_children;
			map_children.emplace_back(ParquetColumnSchema::FromChildSchemas(
			    s_ele.name, ListType::GetChildType(result_type), max_define - 1, max_repeat - 1, this_idx,
			    next_file_idx, std::move(child_schemas)));
			return ParquetColumnSchema::FromChildSchemas(s_ele.name, result_type, max_define, max_repeat, this_idx,
			                                             next_file_idx, std::move(map_children));
		}
		ParquetColumnSchema result;
		if (child_schemas.size() > 1 || (!is_list && !is_map && !is_repeated)) {
			child_list_t<LogicalType> struct_types;
			for (auto &child_schema : child_schemas) {
				struct_types.emplace_back(make_pair(child_schema.name, child_schema.type));
			}

			LogicalType result_type;
			if (is_variant) {
				result_type = LogicalType::VARIANT();
			} else {
				result_type = LogicalType::STRUCT(std::move(struct_types));
			}
			ParquetColumnSchemaType schema_type =
			    is_variant ? ParquetColumnSchemaType::VARIANT : ParquetColumnSchemaType::COLUMN;
			result = ParquetColumnSchema::FromChildSchemas(s_ele.name, result_type, max_define, max_repeat, this_idx,
			                                               next_file_idx, std::move(child_schemas), schema_type);
		} else if (child_schemas.empty()) {
			throw InvalidInputException("Failed to read Parquet file \"%s\": schema element has no children",
			                            file.path);
		} else {
			// if we have a struct with only a single type, pull up
			result = std::move(child_schemas[0]);
			result.name = s_ele.name;
		}
		if (is_repeated) {
			auto list_type = LogicalType::LIST(result.type);
			vector<ParquetColumnSchema> list_child = {std::move(result)};
			result = ParquetColumnSchema::FromChildSchemas(s_ele.name, list_type, max_define, max_repeat, this_idx,
			                                               next_file_idx, std::move(list_child));
		}
		result.parent_schema_index = this_idx;
		return result;
	} else { // leaf node
		if (!s_ele.__isset.type) {
			throw InvalidInputException(
			    "Node '%s' has neither num_children nor type set - this violates the Parquet spec (corrupted file)",
			    s_ele.name.c_str());
		}
		auto result = ParseColumnSchema(s_ele, max_define, max_repeat, this_idx, next_file_idx++);
		if (s_ele.repetition_type == FieldRepetitionType::REPEATED) {
			auto list_type = LogicalType::LIST(result.type);
			vector<ParquetColumnSchema> list_child = {std::move(result)};
			return ParquetColumnSchema::FromChildSchemas(s_ele.name, list_type, max_define, max_repeat, this_idx,
			                                             next_file_idx, std::move(list_child));
		}

		return result;
	}
}

static ParquetColumnSchema FileRowNumberSchema() {
	return ParquetColumnSchema::FileRowNumber();
}

static ParquetColumnSchema FileRowGroupNumberSchema() {
	return ParquetColumnSchema::FileRowGroupNumber();
}

unique_ptr<ParquetColumnSchema> ParquetReader::ParseSchema(ClientContext &context) {
	auto file_meta_data = GetFileMetadata();
	idx_t next_schema_idx = 0;
	idx_t next_file_idx = 0;

	if (file_meta_data->schema.empty()) {
		throw IOException("Failed to read Parquet file \"%s\": no schema elements found", file.path);
	}
	if (file_meta_data->schema[0].num_children == 0) {
		throw IOException("Failed to read Parquet file \"%s\": root schema element has no children", file.path);
	}
	auto root = ParseSchemaRecursive(0, 0, 0, next_schema_idx, next_file_idx, context);
	if (!StructType::IsStruct(root.type)) {
		throw InvalidInputException("Failed to read Parquet file \"%s\": Root element of Parquet file must be a struct",
		                            file.path);
	}
	if (next_schema_idx != file_meta_data->schema.size() - 1) {
		throw InvalidInputException("Failed to read Parquet file \"%s\": schema has elements that are not part of the "
		                            "schema tree",
		                            file.path);
	}
	if (!file_meta_data->row_groups.empty() && next_file_idx != file_meta_data->row_groups[0].columns.size()) {
		throw InvalidInputException("Failed to read Parquet file \"%s\": row group does not have enough columns",
		                            file.path);
	}
	return make_uniq<ParquetColumnSchema>(root);
}

MultiFileColumnDefinition ParquetReader::ParseColumnDefinition(const FileMetaData &file_meta_data,
                                                               ParquetColumnSchema &element) {
	MultiFileColumnDefinition result(element.name, element.type);
	if (element.schema_type == ParquetColumnSchemaType::FILE_ROW_NUMBER) {
		result.identifier = Value::INTEGER(MultiFileReader::ORDINAL_FIELD_ID);
		return result;
	}
	D_ASSERT(element.schema_index.IsValid());
	auto &column_schema = file_meta_data.schema[element.schema_index.GetIndex()];

	if (column_schema.__isset.field_id) {
		result.identifier = Value::INTEGER(column_schema.field_id);
	} else if (element.parent_schema_index.IsValid()) {
		auto &parent_column_schema = file_meta_data.schema[element.parent_schema_index.GetIndex()];
		if (parent_column_schema.__isset.field_id) {
			result.identifier = Value::INTEGER(parent_column_schema.field_id);
		}
	}
	// A GEOMETRY column is a leaf at the logical level - it only wraps an inner BLOB child internally so that the
	// reader can validate/transform the WKB. Exposing that child here would make the column definition diverge from
	// the (childless) global GEOMETRY column, breaking trivial column mapping and disabling row group pruning for
	// spatial predicates. Treat it as a leaf.
	if (element.schema_type != ParquetColumnSchemaType::GEOMETRY) {
		for (auto &child : element.children) {
			result.children.push_back(ParseColumnDefinition(file_meta_data, child));
		}
	}
	// An INT96 column exposed as a STRUCT (int96_as='struct') is a logical struct over a physical leaf - synthesize
	// the struct children here, mirroring MultiFileColumnDefinition::CreateFromNameAndType, so that the column
	// definition matches the global one (and the multi-file mapper can map the column trivially).
	if (element.schema_type == ParquetColumnSchemaType::COLUMN && element.parquet_type == Type::INT96 &&
	    element.type.id() == LogicalTypeId::STRUCT && parquet_options.int96_as == ParquetInt96AsOption::STRUCT) {
		result.children.clear();
		result.children.reserve(StructType::GetChildTypes(element.type).size());
		for (auto &child_entry : StructType::GetChildTypes(element.type)) {
			result.children.push_back(
			    MultiFileColumnDefinition::CreateFromNameAndType(child_entry.first, child_entry.second));
		}
	}
	return result;
}

void ParquetReader::InitializeSchema(ClientContext &context) {
	auto file_meta_data = GetFileMetadata();

	if (file_meta_data->__isset.encryption_algorithm) {
		if (file_meta_data->encryption_algorithm.__isset.AES_GCM_CTR_V1) {
			throw InvalidInputException("File '%s' is encrypted with AES_GCM_CTR_V1, but only AES_GCM_V1 is supported",
			                            GetFileName());
		}
	}
	// check if we like this schema
	if (file_meta_data->schema.size() < 2) {
		throw InvalidInputException("Failed to read Parquet file '%s': Need at least one non-root column in the file",
		                            GetFileName());
	}
	root_schema = ParseSchema(context);
	for (idx_t i = 0; i < root_schema->children.size(); i++) {
		auto &element = root_schema->children[i];
		columns.push_back(ParseColumnDefinition(*file_meta_data, element));
	}
}

void ParquetReader::AddVirtualColumn(column_t virtual_column_id) {
	if (virtual_column_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
		root_schema->children.push_back(FileRowNumberSchema());
	} else if (virtual_column_id == ParquetReader::COLUMN_IDENTIFIER_FILE_ROW_GROUP_NUMBER) {
		root_schema->children.push_back(FileRowGroupNumberSchema());
	} else {
		throw InternalException("Unsupported virtual column id %d for parquet reader", virtual_column_id);
	}
}

ParquetOptions::ParquetOptions(ClientContext &context) {
	context.TryGetCurrentSetting("binary_as_string", binary_as_string);
	context.TryGetCurrentSetting("debug_delta_only_variant_encoding_enabled", variant_legacy_encoding);
}

MultiFileColumnDefinition ParquetColumnDefinition::ToMultiFileColumnDefinition() const {
	MultiFileColumnDefinition result(name, type);
	result.identifier = identifier;
	result.default_expression = ConstantExpression::FromValue(default_value);
	result.children.reserve(children.size());
	for (auto &child : children) {
		result.children.emplace_back(child.ToMultiFileColumnDefinition());
	}
	return result;
}

ParquetReader::ParquetReader(ClientContext &context_p, OpenFileInfo file_p, ParquetOptions parquet_options_p,
                             shared_ptr<ParquetFileMetadataCache> metadata_p,
                             unordered_map<idx_t, ParquetReaderProjectionExpression> projection_expressions_p)
    : BaseFileReader(std::move(file_p)), fs(CachingFileSystem::Get(context_p)),
      allocator(BufferAllocator::Get(context_p)), parquet_options(std::move(parquet_options_p)),
      projection_expressions(std::move(projection_expressions_p)) {
	auto flags = FileFlags::FILE_FLAGS_READ;
	flags.SetRequestSizing(RequestSizing::BY_READER);
	file_handle = fs.OpenFile(context_p, file, flags);
	if (!file_handle->CanSeek()) {
		throw NotImplementedException(
		    "Reading parquet files from a FIFO stream is not supported and cannot be efficiently supported since "
		    "metadata is located at the end of the file. Write the stream to disk first and read from there instead.");
	}
	// read the extended file open info (if any)
	optional_idx footer_size;
	if (file.extended_info) {
		auto &extended_info = *file.extended_info;
		string encryption_key;
		if (extended_info.TryGetOption("encryption_key", encryption_key)) {
			parquet_options.encryption_config = make_shared_ptr<ParquetEncryptionConfig>(std::move(encryption_key));
		}
		idx_t footer_size_option;
		if (extended_info.TryGetOption("footer_size", footer_size_option)) {
			if (footer_size_option > NumericLimits<uint32_t>::Maximum()) {
				throw InvalidInputException("Invalid footer length provided for file '%s'", file.path);
			}
			footer_size = footer_size_option;
		}
	}

	// If metadata cached is disabled
	// or if this file has cached metadata
	// or if the cached version already expired
	if (!metadata_p) {
		if (!MetadataCacheEnabled(context_p)) {
			metadata = LoadMetadata(context_p, allocator, *file_handle, parquet_options.encryption_config,
			                        encryption_util, footer_size);
		} else {
			metadata = ObjectCache::GetObjectCache(context_p).GetWithTypePrefix<ParquetFileMetadataCache>(file.path);
			if (!metadata || !metadata->IsValid(*file_handle)) {
				metadata = LoadMetadata(context_p, allocator, *file_handle, parquet_options.encryption_config,
				                        encryption_util, footer_size);
				ObjectCache::GetObjectCache(context_p).PutWithTypePrefix<ParquetFileMetadataCache>(file.path, metadata);
			}
		}
	} else {
		metadata = std::move(metadata_p);
	}
	can_use_metadata_statistics = CanUseParquetMetadataStatistics(context_p, metadata, parquet_options);
	if (metadata->IsEncrypted()) {
		if (!parquet_options.encryption_config) {
			throw InvalidInputException("File '%s' is encrypted, but 'encryption_config' was not set",
			                            file_handle->GetPath());
		}
		if (!can_use_metadata_statistics) {
			throw InvalidInputException("Computed AES tag differs from read AES tag, are you using the right key?");
		}
		if (!encryption_util) {
			encryption_util = context_p.db->GetEncryptionUtil(true);
		}
	} else if (parquet_options.encryption_config) {
		throw InvalidInputException("File '%s' is not encrypted, but 'encryption_config' was set",
		                            file_handle->GetPath());
	}
	interval_bloom_filter_version = ParquetStatisticsUtils::GetIntervalBloomFilterVersion(*GetFileMetadata());
	InitializeSchema(context_p);
	// Length-pushdown rewrites these columns to BIGINT, update the local schema
	for (const auto &[idx, expr] : projection_expressions) {
		if (idx < columns.size()) {
			columns[idx].type = expr.return_type;
		}
		if (idx < root_schema->children.size()) {
			root_schema->children[idx].type = expr.return_type;
		}
	}
}

bool ParquetReader::MetadataCacheEnabled(ClientContext &context) {
	bool metadata_cache = false;
	context.TryGetCurrentSetting("parquet_metadata_cache", metadata_cache);
	return metadata_cache;
}

shared_ptr<ParquetFileMetadataCache> ParquetReader::GetMetadataCacheEntry(ClientContext &context,
                                                                          const OpenFileInfo &file) {
	return ObjectCache::GetObjectCache(context).GetWithTypePrefix<ParquetFileMetadataCache>(file.path);
}

ParquetUnionData::~ParquetUnionData() {
}

optional_idx ParquetUnionData::TryGetCardinalityEstimate() const {
	if (reader) {
		return reader->Cast<ParquetReader>().NumRows();
	}
	if (metadata) {
		return NumericCast<idx_t>(metadata->metadata->file_metadata.num_rows);
	}
	return optional_idx();
}

unique_ptr<BaseStatistics> ParquetUnionData::GetStatistics(ClientContext &context, const Identifier &name) {
	if (reader) {
		return reader->Cast<ParquetReader>().GetStatistics(context, name);
	}
	return ParquetReader::ReadStatistics(context, *this, name);
}

ParquetReader::ParquetReader(ClientContext &context_p, ParquetOptions parquet_options_p,
                             shared_ptr<ParquetFileMetadataCache> metadata_p)
    : BaseFileReader(string()), fs(CachingFileSystem::Get(context_p)), allocator(BufferAllocator::Get(context_p)),
      metadata(std::move(metadata_p)), parquet_options(std::move(parquet_options_p)), rows_read(0) {
	can_use_metadata_statistics = CanUseParquetMetadataStatistics(context_p, metadata, parquet_options);
	interval_bloom_filter_version = ParquetStatisticsUtils::GetIntervalBloomFilterVersion(*GetFileMetadata());
	InitializeSchema(context_p);
}

ParquetReader::~ParquetReader() {
}

const FileMetaData *ParquetReader::GetFileMetadata() const {
	D_ASSERT(metadata);
	D_ASSERT(metadata->metadata);
	return &metadata->metadata->file_metadata;
}

string ParquetReader::GetUniqueFileIdentifier(const duckdb_parquet::EncryptionAlgorithm &encryption_algorithm) {
	if (encryption_algorithm.__isset.AES_GCM_V1) {
		return encryption_algorithm.AES_GCM_V1.aad_file_unique;
	} else if (encryption_algorithm.__isset.AES_GCM_CTR_V1) {
		throw InvalidInputException("File is encrypted with AES_GCM_CTR_V1, but this is not supported by DuckDB");
	} else {
		throw InvalidInputException("File is encrypted but no encryption algorithm is set");
	}
}

uint32_t ParquetReader::Read(duckdb_apache::thrift::TBase &object, TProtocol &iprot) const {
	return object.read(&iprot);
}

uint32_t ParquetReader::ReadEncrypted(duckdb_apache::thrift::TBase &object, TProtocol &iprot,
                                      CryptoMetaData &aad_crypto_metadata) const {
	ParquetCrypto::GenerateAdditionalAuthenticatedData(allocator, aad_crypto_metadata);
	return ParquetCrypto::Read(object, iprot, parquet_options.encryption_config->GetFooterKey(), *encryption_util,
	                           aad_crypto_metadata);
}

uint32_t ParquetReader::ReadData(duckdb_apache::thrift::protocol::TProtocol &iprot, const data_ptr_t buffer,
                                 const uint32_t buffer_size) const {
	return iprot.getTransport()->read(buffer, buffer_size);
}

uint32_t ParquetReader::ReadDataEncrypted(duckdb_apache::thrift::protocol::TProtocol &iprot, const data_ptr_t buffer,
                                          const uint32_t buffer_size, CryptoMetaData &aad_crypto_metadata) const {
	ParquetCrypto::GenerateAdditionalAuthenticatedData(allocator, aad_crypto_metadata);
	return ParquetCrypto::ReadData(iprot, buffer, buffer_size, parquet_options.encryption_config->GetFooterKey(),
	                               *encryption_util, aad_crypto_metadata);
}

static idx_t GetRowGroupOffset(const ParquetReader &reader, idx_t group_idx) {
	idx_t row_group_offset = 0;
	auto &row_groups = reader.GetFileMetadata()->row_groups;
	for (idx_t i = 0; i < group_idx; i++) {
		row_group_offset += row_groups[i].num_rows;
	}
	return row_group_offset;
}

const ParquetRowGroup &ParquetReader::GetGroup(ParquetReaderScanState &state) {
	auto file_meta_data = GetFileMetadata();
	D_ASSERT(state.group_index < file_meta_data->row_groups.size());
	return file_meta_data->row_groups[state.group_index];
}

uint64_t ParquetReader::GetGroupCompressedSize(ParquetReaderScanState &state) {
	const auto &group = GetGroup(state);
	int64_t total_compressed_size = group.__isset.total_compressed_size ? group.total_compressed_size : 0;

	idx_t calc_compressed_size = 0;

	// If the global total_compressed_size is not set, we can still calculate it
	if (group.total_compressed_size == 0) {
		for (auto &column_chunk : group.columns) {
			calc_compressed_size += column_chunk.meta_data.total_compressed_size;
		}
	}

	if (total_compressed_size != 0 && calc_compressed_size != 0 &&
	    (idx_t)total_compressed_size != calc_compressed_size) {
		throw InvalidInputException(
		    "Failed to read file \"%s\": mismatch between calculated compressed size and reported compressed size",
		    GetFileName());
	}

	return total_compressed_size ? total_compressed_size : calc_compressed_size;
}

uint64_t ParquetReader::GetGroupSpan(ParquetReaderScanState &state) {
	auto &group = GetGroup(state);
	idx_t min_offset = NumericLimits<idx_t>::Maximum();
	idx_t max_offset = NumericLimits<idx_t>::Minimum();

	for (auto &column_chunk : group.columns) {
		// Set the min offset
		idx_t current_min_offset = NumericLimits<idx_t>::Maximum();
		if (column_chunk.meta_data.__isset.dictionary_page_offset) {
			current_min_offset = MinValue<idx_t>(current_min_offset, column_chunk.meta_data.dictionary_page_offset);
		}
		if (column_chunk.meta_data.__isset.index_page_offset) {
			current_min_offset = MinValue<idx_t>(current_min_offset, column_chunk.meta_data.index_page_offset);
		}
		current_min_offset = MinValue<idx_t>(current_min_offset, column_chunk.meta_data.data_page_offset);
		min_offset = MinValue<idx_t>(current_min_offset, min_offset);
		max_offset = MaxValue<idx_t>(max_offset, column_chunk.meta_data.total_compressed_size + current_min_offset);
	}

	return max_offset - min_offset;
}

idx_t ParquetReader::GetGroupOffset(ParquetReaderScanState &state) {
	auto &group = GetGroup(state);
	idx_t min_offset = NumericLimits<idx_t>::Maximum();

	for (auto &column_chunk : group.columns) {
		if (column_chunk.meta_data.__isset.dictionary_page_offset) {
			min_offset = MinValue<idx_t>(min_offset, column_chunk.meta_data.dictionary_page_offset);
		}
		if (column_chunk.meta_data.__isset.index_page_offset) {
			min_offset = MinValue<idx_t>(min_offset, column_chunk.meta_data.index_page_offset);
		}
		min_offset = MinValue<idx_t>(min_offset, column_chunk.meta_data.data_page_offset);
	}

	return min_offset;
}

static FilterPropagateResult CheckParquetFloatFilter(ClientContext &context, ColumnReader &reader,
                                                     const Statistics &pq_col_stats, const TableFilter &filter) {
	// floating point values can have values in the [min, max] domain AND nan values
	// check both stats against the filter
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "CheckParquetFloatFilter");
	auto &type = reader.Type();
	auto nan_stats = NumericStats::CreateUnknown(type);
	auto nan_value = Value("nan").DefaultCastAs(type);
	NumericStats::SetMin(nan_stats, nan_value);
	NumericStats::SetMax(nan_stats, nan_value);
	auto nan_prune = expr_filter.CheckStatistics(context, nan_stats);

	auto min_max_stats = ParquetStatisticsUtils::CreateNumericStats(reader.Type(), reader.Schema(), pq_col_stats);
	auto prune = expr_filter.CheckStatistics(context, *min_max_stats);

	// if EITHER of them cannot be pruned - we cannot prune
	if (prune == FilterPropagateResult::NO_PRUNING_POSSIBLE ||
	    nan_prune == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	// if both are the same we can return that value
	if (prune == nan_prune) {
		return prune;
	}
	// if they are different we need to return that we cannot prune
	// e.g. prune = always false, nan_prune = always true -> we don't know
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static bool TryGetNestedBloomFilterLeaf(ColumnReader &column_reader, const Expression &expr,
                                        optional_ptr<ColumnReader> &leaf_reader) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		if (expr.Cast<BoundReferenceExpression>().Index() != 0) {
			return false;
		}
		leaf_reader = &column_reader;
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}

	auto &function = expr.Cast<BoundFunctionExpression>();
	if (function.GetChildren().empty() ||
	    !TryGetNestedBloomFilterLeaf(column_reader, *function.GetChildren()[0], leaf_reader)) {
		return false;
	}

	if (leaf_reader->Type().id() == LogicalTypeId::INTERVAL && function.GetChildren().size() == 1 &&
	    function.Function().GetName() == "normalized_interval") {
		return true;
	}

	// Handle LIST type.
	if (leaf_reader->Type().id() == LogicalTypeId::LIST &&
	    (function.Function().GetName() == "list_extract" || function.Function().GetName() == "array_extract")) {
		leaf_reader = &leaf_reader->Cast<ListColumnReader>().GetChildReader();
		return true;
	}

	// Handle MAP value extraction.
	if (leaf_reader->Type().id() == LogicalTypeId::MAP && function.Function().GetName() == "map_extract_value") {
		auto &entry_reader = leaf_reader->Cast<ListColumnReader>().GetChildReader();
		if (!StructType::IsStruct(entry_reader.Type())) {
			return false;
		}
		auto &struct_reader = entry_reader.Cast<StructColumnReader>();
		if (struct_reader.child_readers.size() != 2 || !struct_reader.child_readers[1]) {
			return false;
		}
		leaf_reader = struct_reader.child_readers[1].get();
		return true;
	}

	// Handle STRUCT type.
	if (StructType::IsStruct(leaf_reader->Type())) {
		idx_t child_idx;
		if (!TryGetStructExtractChildIndex(function, child_idx)) {
			return false;
		}
		auto &struct_reader = leaf_reader->Cast<StructColumnReader>();
		if (child_idx >= struct_reader.child_readers.size() || !struct_reader.child_readers[child_idx]) {
			return false;
		}
		leaf_reader = struct_reader.child_readers[child_idx].get();
		return true;
	}

	return false;
}

static optional_ptr<ColumnReader> TryGetBloomFilterSource(ColumnReader &column_reader) {
	if (column_reader.Schema().schema_type == ParquetColumnSchemaType::COLUMN) {
		return column_reader;
	}
	if (column_reader.Schema().schema_type != ParquetColumnSchemaType::EXPRESSION) {
		return nullptr;
	}
	auto &expr_reader = column_reader.Cast<ExpressionColumnReader>();
	if (expr_reader.child_readers.size() != 1 || !expr_reader.expr ||
	    expr_reader.expr->GetExpressionClass() != ExpressionClass::BOUND_REF) {
		return nullptr;
	}
	if (expr_reader.expr->Cast<BoundReferenceExpression>().Index() != 0) {
		return nullptr;
	}
	return TryGetBloomFilterSource(*expr_reader.child_readers[0]);
}

static bool TryGetComparisonBloomFilterLeaf(ColumnReader &column_reader, const Expression &expr,
                                            optional_ptr<ColumnReader> &leaf_reader,
                                            unique_ptr<TableFilter> &leaf_filter) {
	auto comparison_type = expr.GetExpressionType();
	if (!BoundComparisonExpression::IsComparison(expr) ||
	    (comparison_type != ExpressionType::COMPARE_EQUAL &&
	     comparison_type != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
		return false;
	}

	auto &comparison = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::Left(comparison);
	auto &right = BoundComparisonExpression::Right(comparison);
	optional_ptr<const Expression> column_expr;
	optional_ptr<const BoundConstantExpression> constant;
	if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		constant = left.Cast<BoundConstantExpression>();
		column_expr = &right;
	} else if (right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		constant = right.Cast<BoundConstantExpression>();
		column_expr = &left;
	} else {
		return false;
	}
	if (constant->GetValue().IsNull()) {
		return false;
	}

	if (!TryGetNestedBloomFilterLeaf(column_reader, *column_expr, leaf_reader) ||
	    leaf_reader->Type() != constant->GetValue().type()) {
		return false;
	}

	leaf_filter = ExpressionFilter::CreateComparisonFilter(comparison_type, constant->GetValue());
	return true;
}

static bool TryGetInBloomFilterLeaf(ColumnReader &column_reader, const Expression &expr,
                                    optional_ptr<ColumnReader> &leaf_reader, unique_ptr<TableFilter> &leaf_filter) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
	    expr.GetExpressionType() != ExpressionType::COMPARE_IN) {
		return false;
	}
	auto &in_expr = expr.Cast<BoundOperatorExpression>();
	auto &children = in_expr.GetChildren();
	if (children.size() <= 1 || !TryGetNestedBloomFilterLeaf(column_reader, *children[0], leaf_reader)) {
		return false;
	}

	vector<Value> values;
	values.reserve(children.size() - 1);
	for (idx_t child_idx = 1; child_idx < children.size(); child_idx++) {
		if (children[child_idx]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		auto &constant = children[child_idx]->Cast<BoundConstantExpression>().GetValue();
		if (constant.IsNull()) {
			continue;
		}
		if (leaf_reader->Type() != constant.type()) {
			return false;
		}
		values.push_back(constant);
	}
	if (values.empty()) {
		return false;
	}

	auto column = make_uniq<BoundReferenceExpression>(leaf_reader->Type(), 0ULL);
	leaf_filter =
	    make_uniq<ExpressionFilter>(ExpressionFilter::CreateInExpression(std::move(column), std::move(values)));
	return true;
}

static bool TryGetBloomFilterLeaf(ColumnReader &column_reader, const TableFilter &filter,
                                  optional_ptr<ColumnReader> &leaf_reader, unique_ptr<TableFilter> &leaf_filter) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "ParquetReader::TryGetBloomFilterLeaf");
	auto &expr = *expr_filter.expr;
	return TryGetComparisonBloomFilterLeaf(column_reader, expr, leaf_reader, leaf_filter) ||
	       TryGetInBloomFilterLeaf(column_reader, expr, leaf_reader, leaf_filter);
}

ColumnReader &ParquetReaderScanState::GetColumnReader(idx_t i) {
	return *column_readers[i];
}

FilterPropagateResult ParquetReader::CheckRowGroupFilterStatistics(ClientContext &context,
                                                                   ParquetReaderScanState &state,
                                                                   ColumnReader &column_reader,
                                                                   const TableFilter &filter) {
	auto &group = GetGroup(state);
	auto schema_column_index = column_reader.ColumnIndex();
	auto prune_result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
	bool is_generated_column = schema_column_index >= group.columns.size();
	bool is_expression = column_reader.Schema().schema_type == ParquetColumnSchemaType::EXPRESSION;
	auto stats = column_reader.Stats(state.group_index, group.columns);
	if (stats) {
		bool has_min_max = false;
		if (!is_generated_column) {
			has_min_max = group.columns[schema_column_index].meta_data.statistics.__isset.min_value &&
			              group.columns[schema_column_index].meta_data.statistics.__isset.max_value;
		}
		if (!is_expression && !is_generated_column && has_min_max &&
		    (column_reader.Type().id() == LogicalTypeId::FLOAT || column_reader.Type().id() == LogicalTypeId::DOUBLE) &&
		    ParquetStatisticsUtils::CanHaveNaN(group.columns[schema_column_index].meta_data.statistics,
		                                       parquet_options.can_have_nan)) {
			// floating point columns can have NaN values in addition to the min/max bounds defined in the file
			// in order to do optimal pruning - we prune based on the [min, max] of the file followed by pruning
			// based on nan
			prune_result = CheckParquetFloatFilter(context, column_reader,
			                                       group.columns[schema_column_index].meta_data.statistics, filter);
		} else {
			auto &expr_filter =
			    ExpressionFilter::GetExpressionFilter(filter, "ParquetReader::CheckRowGroupFilterStatistics");
			prune_result = expr_filter.CheckStatistics(context, *stats);
		}
	}
	return prune_result;
}

bool ParquetReader::DynamicFiltersExcludeRowGroup(ClientContext &context, ParquetReaderScanState &state) {
	for (auto &scan_filter : state.scan_filters) {
		auto &expr_filter =
		    ExpressionFilter::GetExpressionFilter(scan_filter.filter, "ParquetReader::DynamicFiltersExcludeRowGroup");
		if (!ExpressionFilter::ContainsInternalFunction(*expr_filter.expr, DynamicFilterScalarFun::NAME)) {
			continue;
		}
		auto &column_reader = state.GetColumnReader(MultiFileLocalIndex(scan_filter.filter_idx));
		auto prune_result = CheckRowGroupFilterStatistics(context, state, column_reader, scan_filter.filter);
		if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			return true;
		}
	}
	return false;
}

void ParquetReader::PrepareRowGroupBuffer(ClientContext &context, ParquetReaderScanState &state, idx_t i) {
	auto &group = GetGroup(state);
	auto col_idx = MultiFileLocalIndex(i);
	auto &column_reader = state.GetColumnReader(col_idx);

	// keep track of column and row group ordinal if data is encrypted
	if (metadata->crypto_metadata && metadata->crypto_metadata->encryption_algorithm.__isset.AES_GCM_CTR_V1) {
		column_reader.InitializeCryptoMetadata(metadata->crypto_metadata->encryption_algorithm,
		                                       GetGroup(state).ordinal);
	}

	if (group.num_rows < 0) {
		throw InvalidInputException("Failed to read file \"%s\": metadata is corrupt. Row group has invalid "
		                            "number of rows (%lld)",
		                            GetFileName(), group.num_rows);
	}
	auto row_group_num_rows = NumericCast<idx_t>(group.num_rows);

	if (filters) {
		// filters contain output chunk index, not file col idx!
		auto filter_entry = filters->TryGetFilterByColumnIndex(col_idx);
		if (filter_entry) {
			auto &filter = *filter_entry;

			auto prune_result = CheckRowGroupFilterStatistics(context, state, column_reader, filter);
			// check the bloom filter if present
			auto bloom_source = TryGetBloomFilterSource(column_reader);
			if (bloom_source) {
				optional_ptr<ColumnReader> bloom_reader = bloom_source;
				optional_ptr<const TableFilter> bloom_filter = &filter;
				unique_ptr<TableFilter> leaf_filter;
				auto check_bloom_filter = prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE;
				if (bloom_source->Type().IsNested()) {
					if (!TryGetBloomFilterLeaf(*bloom_source, filter, bloom_reader, leaf_filter)) {
						bloom_reader = nullptr;
					} else {
						bloom_filter = leaf_filter.get();
						check_bloom_filter = prune_result != FilterPropagateResult::FILTER_ALWAYS_FALSE;
					}
				}
				auto hash_strategy = bloom_reader ? ParquetStatisticsUtils::GetBloomFilterHashStrategy(
				                                        bloom_reader->Schema(), interval_bloom_filter_version)
				                                  : nullopt;
				if (check_bloom_filter && bloom_reader && bloom_filter &&
				    bloom_reader->Schema().schema_type == ParquetColumnSchemaType::COLUMN &&
				    bloom_reader->ColumnIndex() < group.columns.size() && hash_strategy &&
				    ParquetStatisticsUtils::BloomFilterExcludes(
				        *bloom_filter, group.columns[bloom_reader->ColumnIndex()].meta_data, *state.thrift_file_proto,
				        allocator, bloom_reader->Schema(), *hash_strategy)) {
					prune_result = FilterPropagateResult::FILTER_ALWAYS_FALSE;
				}
			}

			if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
			    prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
				// this effectively will skip this chunk - count the skipped rows towards the progress
				rows_read += row_group_num_rows - state.offset_in_group;
				state.offset_in_group = group.num_rows;
				return;
			}
		}
	}

	column_reader.InitializeRead(state.group_index, row_group_num_rows, group.columns, *state.thrift_file_proto);
}

bool ParquetReader::TryAttachPageIndexBuffer(ThriftFileTransport &transport) const {
	CallOnce(page_index_span_computed, [&] {
		idx_t span_begin = NumericLimits<idx_t>::Maximum();
		idx_t span_end = 0;
		for (auto &row_group : GetFileMetadata()->row_groups) {
			for (auto &column_chunk : row_group.columns) {
				if (ParquetPageIndexUtils::HasColumnIndex(column_chunk)) {
					const auto index_begin = NumericCast<idx_t>(column_chunk.column_index_offset);
					span_begin = MinValue(span_begin, index_begin);
					span_end = MaxValue(span_end, index_begin + NumericCast<idx_t>(column_chunk.column_index_length));
				}
				if (ParquetPageIndexUtils::HasOffsetIndex(column_chunk)) {
					const auto index_begin = NumericCast<idx_t>(column_chunk.offset_index_offset);
					span_begin = MinValue(span_begin, index_begin);
					span_end = MaxValue(span_end, index_begin + NumericCast<idx_t>(column_chunk.offset_index_length));
				}
			}
		}
		if (span_end > span_begin && span_end <= transport.GetSize()) {
			page_index_span_begin = span_begin;
			page_index_span_end = span_end;
		}
	});
	if (page_index_span_end <= page_index_span_begin) {
		return false;
	}
	// the cost model decides: the whole index is worth one request when it is no larger than a coalesced read
	const auto span_size = page_index_span_end - page_index_span_begin;
	if (span_size > transport.GetAcceptedColumnGap()) {
		return false;
	}
	{
		lock_guard<mutex> guard(page_index_buffer_lock);
		if (!page_index_buffer_loaded) {
			auto buffer = allocator.Allocate(span_size);
			auto handles = transport.GetCachingFileHandle().Read(span_size, page_index_span_begin);
			handles.CopyTo(buffer.get(), span_size);
			page_index_buffer = std::move(buffer);
			page_index_buffer_loaded = true;
		}
	}
	transport.AddFilledReadHead(page_index_span_begin, span_size, page_index_buffer.get());
	return true;
}

void ParquetReader::PreparePageIndex(ClientContext &context, ParquetReaderScanState &state) {
	state.page_index.Reset();
	state.page_index_active = false;
	state.page_index_range_idx = 0;
	try {
		PreparePageIndexInternal(context, state);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		const auto type = error.Type();
		if (type == ExceptionType::INTERNAL || type == ExceptionType::INTERRUPT ||
		    type == ExceptionType::OUT_OF_MEMORY || Exception::InvalidatesDatabase(type)) {
			throw;
		}
		// a page index that cannot be read or evaluated is ignored: the row group is read as if it had none. Corrupt
		// index bytes surface as Thrift protocol errors, invalid input or reads past the end of the file, so only the
		// errors that do not come from the file itself are propagated
		auto &trans = reinterpret_cast<ThriftFileTransport &>(*state.thrift_file_proto->getTransport());
		trans.ClearPrefetch();
		state.page_index.Reset();
		state.page_index_active = false;
	}
}

void ParquetReader::PreparePageIndexInternal(ClientContext &context, ParquetReaderScanState &state) {
	auto &group = GetGroup(state);
	if (!filters || !state.use_page_index || parquet_options.encryption_config || group.num_rows <= 0 ||
	    state.offset_in_group == NumericCast<idx_t>(group.num_rows)) {
		// no filters to prune with, the page index is disabled, or the row group was skipped already
		return;
	}
	const auto row_group_num_rows = NumericCast<idx_t>(group.num_rows);
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*state.thrift_file_proto->getTransport());
	const auto file_size = trans.GetSize();

	// the filter columns whose column chunks carry a page index: plain (non-nested, non-repeated) columns only
	struct PageIndexFilterColumn {
		idx_t column_chunk_idx;
		reference<ColumnReader> reader;
		reference<const TableFilter> filter;
	};
	vector<PageIndexFilterColumn> filter_columns;
	idx_t column_index_begin = NumericLimits<idx_t>::Maximum();
	idx_t column_index_end = 0;
	for (auto &scan_filter : state.scan_filters) {
		auto &column_reader = state.GetColumnReader(MultiFileLocalIndex(scan_filter.filter_idx));
		if (column_reader.IsSkipped() || column_reader.Schema().schema_type != ParquetColumnSchemaType::COLUMN ||
		    column_reader.Type().IsNested() || column_reader.MaxRepeat() != 0 ||
		    scan_filter.filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
			continue;
		}
		const auto column_chunk_idx = column_reader.ColumnIndex();
		if (column_chunk_idx >= group.columns.size()) {
			continue;
		}
		auto &column_chunk = group.columns[column_chunk_idx];
		if (!ParquetPageIndexUtils::HasColumnIndex(column_chunk) ||
		    !ParquetPageIndexUtils::HasOffsetIndex(column_chunk)) {
			continue;
		}
		const auto begin = NumericCast<idx_t>(column_chunk.column_index_offset);
		column_index_begin = MinValue(column_index_begin, begin);
		column_index_end = MaxValue(column_index_end, begin + NumericCast<idx_t>(column_chunk.column_index_length));
		filter_columns.push_back({column_chunk_idx, column_reader, scan_filter.filter});
	}
	if (filter_columns.empty()) {
		return;
	}

	// the OffsetIndex of every column chunk the scan reads, to turn pages into rows (filter columns) and to find the
	// pages of those rows (all columns) - they are stored next to each other as well, right after the ColumnIndexes
	unordered_set<idx_t> scanned_column_chunks;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		state.GetColumnReader(MultiFileLocalIndex(i)).GatherColumnChunks(scanned_column_chunks);
	}
	idx_t offset_index_begin = NumericLimits<idx_t>::Maximum();
	idx_t offset_index_end = 0;
	for (auto column_chunk_idx : scanned_column_chunks) {
		auto &column_chunk = group.columns[column_chunk_idx];
		if (!ParquetPageIndexUtils::HasOffsetIndex(column_chunk)) {
			continue;
		}
		const auto begin = NumericCast<idx_t>(column_chunk.offset_index_offset);
		offset_index_begin = MinValue(offset_index_begin, begin);
		offset_index_end = MaxValue(offset_index_end, begin + NumericCast<idx_t>(column_chunk.offset_index_length));
	}
	if (column_index_end > file_size || offset_index_end > file_size) {
		throw InvalidInputException("Failed to read file \"%s\": metadata is corrupt. Page index lies outside the file",
		                            GetFileName());
	}

	// the page index of the whole file is read once per reader when it fits one coalesced read; otherwise this row
	// group fetches the ColumnIndexes of its filter columns, and the OffsetIndexes of its scanned columns join that
	// read when the bytes between the two ranges are within the accepted column gap
	const bool whole_index_attached = TryAttachPageIndexBuffer(trans);
	const auto index_begin = MinValue(column_index_begin, offset_index_begin);
	const auto index_end = MaxValue(column_index_end, offset_index_end);
	const auto covered = (column_index_end - column_index_begin) + (offset_index_end - offset_index_begin);
	const auto gap_between = index_end - index_begin > covered ? index_end - index_begin - covered : 0;
	const bool fetch_indexes_together = !whole_index_attached && gap_between <= trans.GetAcceptedColumnGap();
	if (!whole_index_attached) {
		if (fetch_indexes_together) {
			trans.Prefetch(index_begin, index_end - index_begin);
		} else {
			trans.Prefetch(column_index_begin, column_index_end - column_index_begin);
		}
	}
	vector<vector<bool>> page_may_match(filter_columns.size());
	bool any_page_pruned = false;
	for (idx_t i = 0; i < filter_columns.size(); i++) {
		auto &filter_column = filter_columns[i];
		auto &column_chunk = group.columns[filter_column.column_chunk_idx];
		duckdb_parquet::ColumnIndex column_index(state.page_index.arena);
		ParquetPageIndexUtils::ReadColumnIndex(*state.thrift_file_proto, column_chunk, column_index);
		if (!ParquetPageIndexUtils::ColumnIndexIsValid(column_index, column_index.null_pages.size())) {
			continue;
		}
		auto &column_reader = filter_column.reader.get();
		bool can_have_nan = parquet_options.can_have_nan;
		if (column_chunk.__isset.meta_data && column_chunk.meta_data.__isset.statistics) {
			can_have_nan = ParquetStatisticsUtils::CanHaveNaN(column_chunk.meta_data.statistics, can_have_nan);
		}
		ParquetPageIndexUtils::FilterPages(column_reader.Schema(), column_index, filter_column.filter.get(),
		                                   can_have_nan, page_may_match[i]);
		for (auto may_match : page_may_match[i]) {
			any_page_pruned = any_page_pruned || !may_match;
		}
	}
	if (!any_page_pruned) {
		// every page may match: the OffsetIndex is not needed
		trans.ClearPrefetch();
		return;
	}

	if (!whole_index_attached && !fetch_indexes_together) {
		trans.Prefetch(offset_index_begin, offset_index_end - offset_index_begin);
	}
	state.page_index.offset_indexes.resize(group.columns.size());
	for (auto column_chunk_idx : scanned_column_chunks) {
		auto &column_chunk = group.columns[column_chunk_idx];
		if (!ParquetPageIndexUtils::HasOffsetIndex(column_chunk)) {
			continue;
		}
		auto offset_index = make_uniq<duckdb_parquet::OffsetIndex>(state.page_index.arena);
		ParquetPageIndexUtils::ReadOffsetIndex(*state.thrift_file_proto, column_chunk, *offset_index);
		if (!ParquetPageIndexUtils::OffsetIndexIsValid(*offset_index, row_group_num_rows, file_size, column_chunk)) {
			continue;
		}
		state.page_index.offset_indexes[column_chunk_idx] = std::move(offset_index);
	}
	// the index bytes are not needed anymore
	trans.ClearPrefetch();

	// the rows that may match are the rows of the pages that may match, intersected over the filter columns
	bool first = true;
	vector<ParquetPageRowRange> row_ranges;
	for (idx_t i = 0; i < filter_columns.size(); i++) {
		auto &offset_index = state.page_index.offset_indexes[filter_columns[i].column_chunk_idx];
		if (!offset_index || page_may_match[i].size() != offset_index->page_locations.size()) {
			continue;
		}
		auto column_ranges =
		    ParquetPageIndexUtils::PagesToRowRanges(*offset_index, page_may_match[i], row_group_num_rows);
		row_ranges =
		    first ? std::move(column_ranges) : ParquetPageIndexUtils::IntersectRowRanges(row_ranges, column_ranges);
		first = false;
	}
	if (first) {
		return;
	}
	const auto rows_kept = ParquetPageIndexUtils::RowCount(row_ranges);
	if (rows_kept == row_group_num_rows) {
		return;
	}
	if (state.op) {
		DUCKDB_LOG(context, PhysicalOperatorLogType, *state.op, "ParquetReader", "SkipPages",
		           {{"file", file.path},
		            {"row_group_id", to_string(state.group_index)},
		            {"rows_skipped", to_string(row_group_num_rows - rows_kept)},
		            {"rows_total", to_string(row_group_num_rows)}});
	}
	if (row_ranges.empty()) {
		// no page of the row group can hold a match: skip the row group
		rows_read += row_group_num_rows - state.offset_in_group;
		state.offset_in_group = row_group_num_rows;
		return;
	}
	state.page_index.row_ranges = std::move(row_ranges);
	state.page_index_active = true;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		state.GetColumnReader(MultiFileLocalIndex(i)).InitializePageIndex(state.page_index);
	}
}

idx_t ParquetReader::SkipToPageIndexRows(ParquetReaderScanState &state) {
	const auto group_num_rows = NumericCast<idx_t>(GetGroup(state).num_rows);
	auto &ranges = state.page_index.row_ranges;
	auto &range_idx = state.page_index_range_idx;
	while (range_idx < ranges.size() && ranges[range_idx].end <= state.offset_in_group) {
		range_idx++;
	}
	if (range_idx == ranges.size()) {
		// no rows that may match are left: skip the rest of the row group
		rows_read += group_num_rows - state.offset_in_group;
		state.offset_in_group = group_num_rows;
		return 0;
	}
	auto &range = ranges[range_idx];
	if (range.begin > state.offset_in_group) {
		// skip the rows in front of the range in every column: with an OffsetIndex this jumps over the pages
		const auto skip_count = range.begin - state.offset_in_group;
		for (idx_t i = 0; i < column_ids.size(); i++) {
			state.GetColumnReader(MultiFileLocalIndex(i)).Skip(skip_count);
		}
		rows_read += skip_count;
		state.offset_in_group += skip_count;
	}
	// stay within the range, so that no page that was ruled out is touched
	return MinValue<idx_t>(STANDARD_VECTOR_SIZE, range.end - state.offset_in_group);
}

idx_t ParquetReader::NumRows() const {
	return NumericCast<idx_t>(GetFileMetadata()->num_rows);
}

idx_t ParquetReader::NumRowGroups() const {
	return GetFileMetadata()->row_groups.size();
}

idx_t ParquetReader::GetFileSize() const {
	return file_handle->GetFileSize();
}

idx_t ParquetReader::GetDataSize() const {
	idx_t data_size = 0;
	for (auto &row_group : GetFileMetadata()->row_groups) {
		data_size += row_group.total_compressed_size;
	}
	return data_size;
}

ParquetScanFilter::ParquetScanFilter(ClientContext &context, ProjectionIndex filter_idx, TableFilter &filter)
    : filter_idx(filter_idx), filter(filter) {
	filter_state = TableFilterState::Initialize(context, filter);
}

ParquetScanFilter::~ParquetScanFilter() {
}

unique_ptr<CachingFileHandle> ParquetReader::OpenScanHandle(ClientContext &context) const {
	auto flags = FileFlags::FILE_FLAGS_READ;
	flags.SetRequestSizing(RequestSizing::BY_READER);
	if (ShouldAndCanPrefetch(context, *file_handle)) {
		flags |= FileFlags::FILE_FLAGS_PARALLEL_ACCESS;
		if (file_handle->IsRemoteFile()) {
			flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
		}
	}
	return fs.OpenFile(context, file, flags);
}

void ParquetReader::PrepareReadAhead(ClientContext &context, GlobalTableFunctionState &) {
	// pre-open the scan handle while the file is opened, so the first InitializeScan skips its file-open
	InitializeScanHandle(context);
}

void ParquetReader::InitializeScanHandle(ClientContext &context) const {
	CallOnce(scan_handle_initialized, [&] { shared_scan_handle = OpenScanHandle(context); });
}

void ParquetReader::InitializeScan(ClientContext &context, ParquetReaderScanState &state, idx_t group_to_read) const {
	state.resuming_payload = false;
	state.offset_in_group = 0;
	state.filter_count = 0;
	state.group_index = group_to_read;
	state.sel.Initialize(STANDARD_VECTOR_SIZE);
	if (!state.file_handle || state.file_handle->GetPath() != file_handle->GetPath()) {
		state.prefetch_mode = ShouldAndCanPrefetch(context, *file_handle);
		// all scan states share one handle (opened with parallel access), so open handles and
		// connections scale with the number of readers instead of the number of row-group jobs
		InitializeScanHandle(context);
		state.file_handle = shared_scan_handle;
	}
	state.scan_filters.clear();
	state.use_page_index = true;
	context.TryGetCurrentSetting("parquet_use_page_index", state.use_page_index);
	if (filters) {
		state.adaptive_filter_cache.InitializeAdaptiveFilter(*filters, filter_global_indices, context.logger,
		                                                     file.path);
		for (auto &entry : *filters) {
			state.scan_filters.emplace_back(context, entry.GetIndex(), entry.Filter());
		}
	}
	if (state.filter_eliminated_all_rows.size() != state.scan_filters.size()) {
		state.filter_eliminated_all_rows.assign(state.scan_filters.size(), false);
	}

	state.thrift_file_proto = CreateThriftFileProtocol(context, *state.file_handle, state.prefetch_mode,
	                                                   DetermineAcceptedColumnGap(context, state));

	state.column_readers.resize(column_indexes.size());
	for (idx_t i = 0; i < column_indexes.size(); i++) {
		auto it = expression_map.find(ProjectionIndex(i));
		auto &index = column_indexes[i];
		auto column_id = index.GetPrimaryIndex();
		if (auto it = projection_expressions.find(column_id);
		    it != projection_expressions.end() &&
		    it->second.type == ParquetReaderProjectionExpressionType::BYTE_LENGTH) {
			auto &schema = root_schema->children[column_id];
			state.column_readers[i] = make_uniq<ByteArrayLengthColumnReader>(*this, schema);
			continue;
		}
		if (it != expression_map.end()) {
			auto &expression_data = it->second;
			auto &expression = expression_data.expression;
			auto &expression_column_indexes = expression_data.column_indexes;
			D_ASSERT(!expression_column_indexes.empty());

			vector<unique_ptr<ColumnReader>> child_readers;
			for (auto &id : expression_column_indexes) {
				auto &schema = root_schema->children[id.GetPrimaryIndex()];
				child_readers.push_back(CreateReaderRecursive(context, id, schema));
			}
			auto expr_schema = make_uniq<ParquetColumnSchema>(ParquetColumnSchema::FromParentSchema(
			    child_readers[0]->Schema(), expression->GetReturnType(), ParquetColumnSchemaType::EXPRESSION));
			auto expr_reader = make_uniq<ExpressionColumnReader>(context, std::move(child_readers), expression->Copy(),
			                                                     std::move(expr_schema));
			state.column_readers[i] = std::move(expr_reader);
		} else {
			auto &index = column_indexes[i];
			auto column_id = index.GetPrimaryIndex();
			auto &schema = root_schema->children[column_id];
			auto column_reader = CreateReaderRecursive(context, index, schema);
			state.column_readers[i] = std::move(column_reader);
		}
	}

	state.define_buf.resize(allocator, STANDARD_VECTOR_SIZE);
	state.repeat_buf.resize(allocator, STANDARD_VECTOR_SIZE);
}

shared_ptr<ParquetReader> ParquetReader::CreateMetadataReader(ClientContext &context, ParquetOptions parquet_options,
                                                              shared_ptr<ParquetFileMetadataCache> metadata) {
	return shared_ptr<ParquetReader>(new ParquetReader(context, std::move(parquet_options), std::move(metadata)));
}

void ParquetReader::GetPartitionStats(vector<PartitionStatistics> &result) {
	if (!can_use_metadata_statistics) {
		return;
	}
	GetPartitionStats(*GetFileMetadata(), result, *root_schema, parquet_options);
}

struct ParquetPartitionRowGroup : public PartitionRowGroup {
	ParquetPartitionRowGroup(const duckdb_parquet::FileMetaData &metadata_p,
	                         optional_ptr<ParquetColumnSchema> root_schema_p,
	                         optional_ptr<ParquetOptions> parquet_options_p, const idx_t row_group_idx_p)
	    : metadata(metadata_p), root_schema(root_schema_p), parquet_options(parquet_options_p),
	      row_group_idx(row_group_idx_p) {
	}

	const duckdb_parquet::FileMetaData &metadata;
	const optional_ptr<ParquetColumnSchema> root_schema;
	const optional_ptr<ParquetOptions> parquet_options;
	const idx_t row_group_idx;

	unique_ptr<BaseStatistics> GetColumnStatistics(const StorageIndex &storage_index) override {
		const idx_t primary_index = storage_index.GetPrimaryIndex();
		if (primary_index >= root_schema->children.size()) {
			return nullptr;
		}
		D_ASSERT(metadata.row_groups.size() > row_group_idx);

		const auto &row_group = metadata.row_groups[row_group_idx];
		const auto &column_schema = root_schema->children[primary_index];
		auto column_stats = column_schema.Stats(metadata, *parquet_options, row_group_idx, row_group.columns);
		if (!column_stats || !storage_index.IsPushdownExtract() || !column_stats->GetType().IsNested()) {
			return column_stats;
		}
		return column_stats->PushdownExtract(storage_index.GetChildIndex(0));
	}

	bool MinMaxIsExact(const StorageIndex &storage_index) override {
		const idx_t primary_index = storage_index.GetPrimaryIndex();
		if (primary_index >= root_schema->children.size()) {
			return false;
		}
		D_ASSERT(metadata.row_groups.size() > row_group_idx);

		// Special handle generated columns.
		const auto &column_schema = root_schema->children[primary_index];
		if (column_schema.schema_type == ParquetColumnSchemaType::FILE_ROW_NUMBER) {
			return true;
		}

		const auto &row_group = metadata.row_groups[row_group_idx];
		const auto &column_chunk = row_group.columns[primary_index];

		if (column_chunk.__isset.meta_data && column_chunk.meta_data.__isset.statistics &&
		    column_chunk.meta_data.statistics.__isset.is_min_value_exact &&
		    column_chunk.meta_data.statistics.__isset.is_max_value_exact) {
			const auto &stats = column_chunk.meta_data.statistics;
			return stats.is_min_value_exact && stats.is_max_value_exact;
		}
		return false;
	}

	bool HasPendingWrites() override {
		// Parquet row groups are read directly from a file, so there is no notion of pending/uncheckpointed writes.
		return false;
	}
};

void ParquetReader::GetPartitionStats(const duckdb_parquet::FileMetaData &metadata, vector<PartitionStatistics> &result,
                                      optional_ptr<ParquetColumnSchema> root_schema,
                                      optional_ptr<ParquetOptions> parquet_options) {
	idx_t offset = 0;
	for (idx_t i = 0; i < metadata.row_groups.size(); i++) {
		auto &row_group = metadata.row_groups[i];
		PartitionStatistics partition_stats;
		partition_stats.row_start = offset;
		partition_stats.count = row_group.num_rows;
		partition_stats.count_type = CountType::COUNT_EXACT;
		if (root_schema && parquet_options) {
			partition_stats.partition_row_group =
			    make_shared_ptr<ParquetPartitionRowGroup>(metadata, root_schema, parquet_options, i);
		}
		offset += row_group.num_rows;
		result.push_back(partition_stats);
	}
}

// Async I/O tasks for the read heads in the index range [from, to), skipping any that are already fetched.
static vector<unique_ptr<AsyncTask>>
CollectIOTasks(ThriftFileTransport &trans, const shared_ptr<CachingFileHandle> &file_handle, idx_t from, idx_t to) {
	auto &read_heads = trans.GetReadHeads();
	vector<unique_ptr<AsyncTask>> io_tasks;
	for (idx_t i = from; i < to; i++) {
		auto &read_head = read_heads[i];
		if (!read_head->data_isset) {
			// fetch the read head's bytes on the async pool
			io_tasks.push_back(make_uniq<CallbackAsyncTask>(
			    [read_head, file_handle] { read_head->Fetch(*file_handle); }, read_head->size));
		}
	}
	return io_tasks;
}

ParquetPrefetchStrategy ParquetReader::WholeGroupPrefetch(ParquetReaderScanState &state, ThriftFileTransport &trans,
                                                          const duckdb_parquet::RowGroup &group,
                                                          uint64_t total_row_group_span, bool log_prefetch) {
	auto total_compressed_size = GetGroupCompressedSize(state);
	if (total_compressed_size > 0) {
		trans.RegisterPrefetch(GetGroupOffset(state), total_row_group_span, false);
		trans.FinalizeRegistration();
	}
	if (log_prefetch) {
		state.prefetch_metrics.logger.strategy = ParquetPrefetchStrategy::WHOLE_GROUP;
		vector<ParquetPrefetchColumn> pending_registrations;
		for (auto &column_chunk : group.columns) {
			auto &path = column_chunk.meta_data.path_in_schema;
			string name =
			    path.empty() ? string() : StringUtil::Join(path, ".", [](const duckdb_parquet::ParquetString &part) {
				    return string(part);
			    });
			pending_registrations.push_back({name, ParquetColumnChunkFileOffset(column_chunk)});
		}
		state.prefetch_metrics.logger.GeneratePrefetchGroup(trans, pending_registrations, group.columns);
	}
	return ParquetPrefetchStrategy::WHOLE_GROUP;
}

ParquetPrefetchStrategy ParquetReader::ColumnWisePrefetch(ParquetReaderScanState &state, ThriftFileTransport &trans,
                                                          const duckdb_parquet::RowGroup &group,
                                                          bool filters_look_unselective, bool log_prefetch) const {
	bool has_non_optional_filter = false;
	if (filters) {
		for (auto &entry : *filters) {
			if (!ExpressionFilter::IsRootOptionalFilter(entry.Filter())) {
				has_non_optional_filter = true;
			}
		}
	}
	const bool lazy_fetch = has_non_optional_filter && !filters_look_unselective;

	vector<bool> already_registered(column_ids.size(), false);
	vector<ParquetPrefetchColumn> pending_registrations;

	if (lazy_fetch && !state.scan_filters.empty()) {
		auto &adaptive_filter = state.adaptive_filter_cache.GetAdaptiveFilter();
		const auto &permutation = adaptive_filter.GetPermutation();
		for (idx_t i = 0; i < state.scan_filters.size(); i++) {
			// We must do the filter grouping based on the set that excludes all rows
			if (i > 0 && state.filter_eliminated_all_rows[permutation[i - 1]]) {
				trans.FinalizeRegistration();
				if (log_prefetch) {
					state.prefetch_metrics.logger.GeneratePrefetchGroup(trans, pending_registrations, group.columns);
				}
			}
			auto &scan_filter = state.scan_filters[permutation[i]];
			MultiFileLocalIndex local_idx(scan_filter.filter_idx);
			auto &child = state.GetColumnReader(local_idx);
			child.RegisterPrefetch(trans, true);
			if (log_prefetch) {
				pending_registrations.emplace_back(child.Schema().name, child.PrefetchOffset());
			}
			already_registered[local_idx.GetIndex()] = true;
		}
		// Done with the filter columns, time to merge the non-filter columns.
		trans.FinalizeRegistration();
		state.filter_head_count = trans.GetReadHeads().size();
		if (log_prefetch) {
			state.prefetch_metrics.logger.GeneratePrefetchGroup(trans, pending_registrations, group.columns);
		}
	}

	for (idx_t i = 0; i < column_ids.size(); i++) {
		// We do the remaining columns that have not yet been prefetched by the previous steps
		// These can be the remaining projections, or all columns if the filters were not selective
		if (already_registered[i]) {
			continue;
		}
		auto col_idx = MultiFileLocalIndex(i);
		auto &child = state.GetColumnReader(col_idx);
		if (child.IsSkipped()) {
			//! Column reader is skipped entirely
			continue;
		}
		child.RegisterPrefetch(trans, true);
		if (log_prefetch) {
			pending_registrations.emplace_back(child.Schema().name, child.PrefetchOffset());
		}
	}
	trans.FinalizeRegistration();
	ParquetPrefetchStrategy strategy;
	if (state.page_index_active) {
		strategy = lazy_fetch ? ParquetPrefetchStrategy::PREFETCH_FILTERS_PAGES
		                      : ParquetPrefetchStrategy::COLUMN_WISE_EAGER_PAGES;
	} else {
		strategy = lazy_fetch ? ParquetPrefetchStrategy::PREFETCH_FILTERS : ParquetPrefetchStrategy::COLUMN_WISE_EAGER;
	}
	if (log_prefetch) {
		state.prefetch_metrics.logger.GeneratePrefetchGroup(trans, pending_registrations, group.columns);
		state.prefetch_metrics.logger.strategy = strategy;
	}
	return strategy;
}

ParquetPrefetchStrategy ParquetReader::RegisterRowGroupReads(ClientContext &context, ParquetReaderScanState &state) {
	const bool log_prefetch =
	    Logger::Get(context).ShouldLog(ParquetPrefetchLogType::NAME, ParquetPrefetchLogType::LEVEL);
	state.offset_in_group = 0;
	ParquetPrefetchStrategy strategy = ParquetPrefetchStrategy::NONE;

	auto &trans = reinterpret_cast<ThriftFileTransport &>(*state.thrift_file_proto->getTransport());
	trans.ClearPrefetch();
	state.filter_head_count = 0;

	if (state.prefetch_mode) {
		trans.SetAcceptedColumnGap(DetermineAcceptedColumnGap(context, state));
	}

	// TODO: only need this if we have a deletion vector?
	state.group_offset = GetRowGroupOffset(*this, state.group_index);

	auto &group = GetGroup(state);
	// several readers can read the same column chunk (e.g. two fields of one variant column), count each chunk once
	unordered_set<idx_t> scanned_column_chunks;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto col_idx = MultiFileLocalIndex(i);
		PrepareRowGroupBuffer(context, state, col_idx);
		state.GetColumnReader(i).GatherColumnChunks(scanned_column_chunks);
	}
	// the row group statistics could not rule the row group out - the page index may still rule out pages
	PreparePageIndex(context, state);
	uint64_t to_scan_compressed_bytes = 0;
	for (auto column_chunk : scanned_column_chunks) {
		to_scan_compressed_bytes += NumericCast<uint64_t>(group.columns[column_chunk].meta_data.total_compressed_size);
	}
	const bool row_group_skipped = state.offset_in_group == (idx_t)group.num_rows;
	if (row_group_skipped) {
		++state.row_groups_skipped;
	} else {
		++state.row_groups_read;
	}
	// counted in Process when the scan of the row group starts: a row group that is read ahead but never scanned,
	// or whose prefetch fails, is not counted
	state.pending_bytes_scanned = row_group_skipped ? 0 : to_scan_compressed_bytes;
	if (state.op) {
		DUCKDB_LOG(context, PhysicalOperatorLogType, *state.op, "ParquetReader",
		           row_group_skipped ? "SkipRowGroup" : "ReadRowGroup",
		           {{"file", file.path}, {"row_group_id", to_string(state.group_index)}});
	}

	if (state.prefetch_mode && !row_group_skipped) {
		uint64_t total_row_group_span = GetGroupSpan(state);

		double scan_percentage = (double)(to_scan_compressed_bytes) / static_cast<double>(total_row_group_span);

		if (to_scan_compressed_bytes > total_row_group_span) {
			if (!state.file_handle->OnDiskFile()) {
				throw IOException(
				    "The parquet file '%s' seems to have incorrectly set page offsets. This interferes with DuckDB's "
				    "prefetching optimization. DuckDB may still be able to scan this file by manually disabling the "
				    "prefetching mechanism using: 'SET disable_parquet_prefetching=true'.",
				    GetFileName());
			}
			// broken page offsets cannot be prefetched, local files fall back to synchronous reads
			state.prefetch_mode = false;
		}
		if (state.prefetch_mode) {
			// whole group and column wise prefetch fetch eagerly, filter prefetch fetches lazily
			if (parquet_options.prefetch_strategy == ParquetPrefetchStrategyOption::WHOLE_GROUP) {
				strategy = WholeGroupPrefetch(state, trans, group, total_row_group_span, log_prefetch);
			} else {
				bool filters_look_unselective = false;
				if (filters) {
					if (state.prefetch_metrics.row_groups_executed == 0) {
						filters_look_unselective = true;
						if (scan_percentage > ParquetReaderPrefetchConfig::WHOLE_GROUP_PREFETCH_MINIMUM_SCAN) {
							unordered_set<idx_t> filter_column_chunks;
							for (auto &scan_filter : state.scan_filters) {
								state.GetColumnReader(scan_filter.filter_idx.GetIndex())
								    .GatherColumnChunks(filter_column_chunks);
							}
							uint64_t filter_compressed_bytes = 0;
							for (auto column_chunk : filter_column_chunks) {
								filter_compressed_bytes +=
								    NumericCast<uint64_t>(group.columns[column_chunk].meta_data.total_compressed_size);
							}
							filters_look_unselective =
							    filter_column_chunks.empty() ||
							    static_cast<double>(filter_compressed_bytes) /
							            static_cast<double>(total_row_group_span) >
							        ParquetReaderPrefetchConfig::FIRST_GROUP_PREFETCH_MAXIMUM_FILTER_SCAN;
						}
					} else {
						filters_look_unselective = static_cast<double>(state.prefetch_metrics.row_groups_with_matches) /
						                               static_cast<double>(state.prefetch_metrics.row_groups_executed) >
						                           ParquetReaderPrefetchConfig::PREFETCH_FILTER_MINIMUM_MATCH_RATIO;
					}
				}

				if ((!filters || filters_look_unselective) &&
				    scan_percentage > ParquetReaderPrefetchConfig::WHOLE_GROUP_PREFETCH_MINIMUM_SCAN &&
				    !state.page_index_active) {
					strategy = WholeGroupPrefetch(state, trans, group, total_row_group_span, log_prefetch);
				} else {
					// with a page index only the pages that may match are registered, adjacent ones are coalesced
					strategy = ColumnWisePrefetch(state, trans, group, filters_look_unselective, log_prefetch);
				}
			}
			if (log_prefetch) {
				state.prefetch_metrics.logger.accepted_column_gap = trans.GetAcceptedColumnGap();
			}
		}
	}
	state.resuming_payload = false;
	return strategy;
}

AsyncResult ParquetReader::ScheduleRowGroupReads(ParquetReaderScanState &state, ParquetPrefetchStrategy strategy) {
	if (strategy == ParquetPrefetchStrategy::NONE) {
		// nothing to prefetch
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*state.thrift_file_proto->getTransport());
	auto read_head_count = trans.GetReadHeads().size();
	vector<unique_ptr<AsyncTask>> io_tasks;
	switch (strategy) {
	case ParquetPrefetchStrategy::PREFETCH_FILTERS:
	case ParquetPrefetchStrategy::PREFETCH_FILTERS_PAGES:
		// schedule only the filter columns' I/O, they are last in our list
		io_tasks = CollectIOTasks(trans, state.file_handle, read_head_count - state.filter_head_count, read_head_count);
		break;
	case ParquetPrefetchStrategy::WHOLE_GROUP:
	case ParquetPrefetchStrategy::COLUMN_WISE_EAGER:
	case ParquetPrefetchStrategy::COLUMN_WISE_EAGER_PAGES:
		// schedule the I/O for all columns up front
		io_tasks = CollectIOTasks(trans, state.file_handle, 0, read_head_count);
		break;
	default:
		throw InternalException("Unexpected parquet prefetch strategy when scheduling I/O");
	}
	return AsyncResult::FromTasks(std::move(io_tasks), TaskSchedulerType::ASYNC);
}

void ParquetReader::FinishRowGroup(ClientContext &context, ParquetReaderScanState &state, bool log_prefetch) {
	if (log_prefetch && state.prefetch_metrics.filter_ran) {
		LogRowGroupPrefetch(context, file.path, state.group_index, state);
	}
	state.prefetch_metrics.FinalizeRowGroupSelectivity();
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*state.thrift_file_proto->getTransport());
	trans.ClearPrefetch();
}

idx_t ParquetReader::EvaluateFilters(ParquetReaderScanState &state, DataChunk &result, idx_t scan_count,
                                     uint8_t *define_ptr, uint8_t *repeat_ptr, bool log_prefetch) {
	idx_t filter_count = result.size();
	D_ASSERT(filter_count == scan_count);

	state.sel.Initialize(nullptr);
	D_ASSERT(!filters || state.scan_filters.size() == filters->FilterCount());

	bool is_first_filter = true;
	if (deletion_filter) {
		auto row_start = UnsafeNumericCast<row_t>(state.offset_in_group + state.group_offset);
		filter_count = deletion_filter->Filter(row_start, scan_count, state.sel);
		//! FIXME: does this need to be set?
		//! As part of 'DirectFilter' we also initialize reads of the child readers
		is_first_filter = false;
	}
	if (!filters) {
		return filter_count;
	}

	// first load the columns that are used in filters
	auto &adaptive_filter = state.adaptive_filter_cache.GetAdaptiveFilter();
	auto filter_state = adaptive_filter.BeginFilter();
	const auto &permutation = adaptive_filter.GetPermutation();
	for (idx_t i = 0; i < state.scan_filters.size(); i++) {
		auto &scan_filter = state.scan_filters[permutation[i]];
		MultiFileLocalIndex local_idx(scan_filter.filter_idx);
		auto &child_reader = state.GetColumnReader(local_idx);
		if (filter_count == 0) {
			child_reader.Skip(scan_count);
			continue;
		}

		auto &result_vector = result.data[local_idx.GetIndex()];
		ColumnReaderInput reader_input(scan_count, define_ptr, repeat_ptr);
		child_reader.Filter(reader_input, result_vector, scan_filter.filter, *scan_filter.filter_state, state.sel,
		                    filter_count, is_first_filter);
		if (log_prefetch) {
			auto &filters_used = state.prefetch_metrics.logger.filters_used;
			if (filters_used.size() != state.scan_filters.size()) {
				filters_used.assign(state.scan_filters.size(), false);
			}
			filters_used[permutation[i]] = true;
		}
		is_first_filter = false;
		if (filter_count == 0) {
			state.filter_eliminated_all_rows[permutation[i]] = true;
		}
	}
	adaptive_filter.EndFilter(filter_state);
	state.prefetch_metrics.filter_ran = true;
	if (filter_count > 0) {
		state.prefetch_metrics.had_match = true;
	}
	return filter_count;
}

// Whether column position `i` is used by a filter.
static bool IsFilterColumn(const vector<ParquetScanFilter> &scan_filters, idx_t column_index) {
	for (auto &scan_filter : scan_filters) {
		if (MultiFileLocalIndex(scan_filter.filter_idx).GetIndex() == column_index) {
			return true;
		}
	}
	return false;
}

void ParquetReader::DecodeRemainingColumns(ParquetReaderScanState &state, DataChunk &result, idx_t filter_count,
                                           uint8_t *define_ptr, uint8_t *repeat_ptr) {
	for (idx_t i = 0; i < column_ids.size(); i++) {
		MultiFileLocalIndex col_idx(i);
		if (IsFilterColumn(state.scan_filters, i)) {
			// already decoded (or skipped) by EvaluateFilters
			continue;
		}
		if (filter_count == 0) {
			state.GetColumnReader(col_idx).Skip(result.size());
			continue;
		}
		auto &result_vector = result.data[i];
		auto &child_reader = state.GetColumnReader(col_idx);
		if (metadata->crypto_metadata && metadata->crypto_metadata->encryption_algorithm.__isset.AES_GCM_V1) {
			child_reader.InitializeCryptoMetadata(metadata->crypto_metadata->encryption_algorithm,
			                                      GetGroup(state).ordinal);
		}
		ColumnReaderInput reader_input(result.size(), define_ptr, repeat_ptr);
		child_reader.Select(reader_input, result_vector, state.sel, filter_count);
	}
}

vector<unique_ptr<AsyncTask>> ParquetReader::ScheduleRemainingColumns(ParquetReaderScanState &state, DataChunk &result,
                                                                      idx_t scan_count) {
	if (state.filter_count == 0 || state.filter_head_count == 0) {
		return {};
	}
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*state.thrift_file_proto->getTransport());
	auto payload_head_count = trans.GetReadHeads().size() - state.filter_head_count;
	// payload columns are the leading read heads
	auto io_tasks = CollectIOTasks(trans, state.file_handle, 0, payload_head_count);
	if (io_tasks.empty()) {
		// no payload to do
		return {};
	}
	state.resuming_payload = true;
	return io_tasks;
}

AsyncResult ParquetReader::ProcessFilters(ParquetReaderScanState &state, DataChunk &result, idx_t scan_count,
                                          uint8_t *define_ptr, uint8_t *repeat_ptr, bool log_prefetch) {
	if (!state.resuming_payload) {
		state.filter_count = EvaluateFilters(state, result, scan_count, define_ptr, repeat_ptr, log_prefetch);
		auto io_tasks = ScheduleRemainingColumns(state, result, scan_count);
		if (!io_tasks.empty()) {
			return AsyncResult(std::move(io_tasks), TaskSchedulerType::ASYNC);
		}
	}
	DecodeRemainingColumns(state, result, state.filter_count, define_ptr, repeat_ptr);
	if (scan_count != state.filter_count) {
		result.Slice(state.sel, state.filter_count);
	}
	return SourceResultType::HAVE_MORE_OUTPUT;
}

AsyncResult ParquetReader::Process(ClientContext &context, ParquetReaderScanState &state, DataChunk &result) {
	const bool log_prefetch =
	    Logger::Get(context).ShouldLog(ParquetPrefetchLogType::NAME, ParquetPrefetchLogType::LEVEL);
	const idx_t group_num_rows = GetGroup(state).num_rows;
	// Queued row groups may predate the current dynamic filter. Do not revisit a partially decoded chunk.
	if (state.offset_in_group == 0 && !state.resuming_payload && DynamicFiltersExcludeRowGroup(context, state)) {
		rows_read += group_num_rows;
		state.offset_in_group = group_num_rows;
	}
	auto scan_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, group_num_rows - state.offset_in_group);
	if (state.page_index_active && scan_count > 0) {
		// read only the rows of pages that may match according to the page index
		scan_count = SkipToPageIndexRows(state);
	}
	if (!state.resuming_payload) {
		result.SetChildCardinality(scan_count);
	}

	if (scan_count == 0) {
		// the row group is fully consumed
		FinishRowGroup(context, state, log_prefetch);
		return SourceResultType::FINISHED;
	}
	if (state.pending_bytes_scanned > 0) {
		state.bytes_scanned += state.pending_bytes_scanned;
		QueryProfiler::Get(context).TrackBytesScanned(state.pending_bytes_scanned);
		state.pending_bytes_scanned = 0;
	}

	auto &deletion_filter = this->deletion_filter;

	state.define_buf.zero();
	state.repeat_buf.zero();

	auto define_ptr = (uint8_t *)state.define_buf.ptr;
	auto repeat_ptr = (uint8_t *)state.repeat_buf.ptr;

	if (filters || deletion_filter) {
		auto res = ProcessFilters(state, result, scan_count, define_ptr, repeat_ptr, log_prefetch);
		if (res.GetResultType() == AsyncResultType::BLOCKED) {
			// we must wait  on the payload I/O
			return res;
		}
	} else {
		for (idx_t i = 0; i < column_ids.size(); i++) {
			auto col_idx = MultiFileLocalIndex(i);
			auto file_col_idx = column_ids[col_idx];
			auto &result_vector = result.data[i];
			auto &child_reader = state.GetColumnReader(col_idx);
			if (metadata->crypto_metadata && metadata->crypto_metadata->encryption_algorithm.__isset.AES_GCM_V1) {
				child_reader.InitializeCryptoMetadata(metadata->crypto_metadata->encryption_algorithm,
				                                      GetGroup(state).ordinal);
			}
			ColumnReaderInput reader_input(scan_count, define_ptr, repeat_ptr);
			auto rows_read = child_reader.Read(reader_input, result_vector);
			if (rows_read != scan_count) {
				throw InvalidInputException("Mismatch in parquet read for column %llu, expected %llu rows, got %llu",
				                            file_col_idx, scan_count, rows_read);
			}
		}
	}

	result.SetChildCardinality(result.size());
	rows_read += scan_count;
	state.offset_in_group += scan_count;
	state.resuming_payload = false;
	return SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
