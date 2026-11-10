//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "duckdb.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/function/copy_function.hpp"
#include "parquet_statistics.hpp"
#include "column_writer.hpp"
#include "parquet_field_id.hpp"
#include "parquet_shredding.hpp"
#include "parquet_types.h"
#include "parquet_geometry.hpp"
#include "writer/parquet_cdc_chunker.hpp"
#include "writer/parquet_write_stats.hpp"
#include "thrift/protocol/TCompactProtocol.h"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb_apache {
namespace thrift {
class TBase;

namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache

namespace duckdb {
class FileSystem;
class FileOpener;
class ParquetEncryptionConfig;
class ParquetStatsAccumulator;
class Serializer;
class Deserializer;
class ColumnWriterStatistics;
struct CopyFunctionFileStatistics;
class ClientContext;
class EncryptionUtil;
class GeoParquetFileMetadata;
class ParquetWriter;
class PhysicalOperator;
enum class GeoParquetVersion : uint8_t;

struct PreparedParquetLayout {
	vector<duckdb_parquet::SchemaElement> schema;
	ShreddingType shredding_types;
};

struct PreparedRowGroup {
	duckdb_parquet::RowGroup row_group;
	vector<unique_ptr<ColumnWriterState>> states;
	PreparedParquetLayout layout;
};

//! Content-defined page boundaries, computed up front for one row group by rolling the chunker over the
//! values in file order without writing anything. Cut positions are the value indices opening a new page,
//! per CDC-cut top-level column, per input chunk of that column.
class ParquetCdcBoundaries {
public:
	struct ColumnCuts {
		idx_t column_index = 0;
		vector<vector<idx_t>> cuts_per_chunk;
	};

	const ColumnCuts *FindColumn(idx_t column_index) const {
		for (auto &column : columns) {
			if (column.column_index == column_index) {
				return &column;
			}
		}
		return nullptr;
	}

	vector<ColumnCuts> columns;
};

//! One row group inside the content-defined chunking pipeline: the boundary scan ran on the flushing
//! thread, and the heavy prepare it unlocked runs on a worker, which owns the batch and its cuts until it
//! completes. The prepared row group is appended in batch order once the future resolves.
struct ParquetCdcPipelineEntry {
	unique_ptr<PreparedRowGroup> prepared;
	std::future<void> future;
};

struct ParquetBloomFilterEntry {
	unique_ptr<ParquetBloomFilter> bloom_filter;
	idx_t row_group_idx;
	idx_t column_idx;
};

enum class ParquetVersion : uint8_t {
	V1 = 1, //! Excludes DELTA_BINARY_PACKED, DELTA_LENGTH_BYTE_ARRAY, BYTE_STREAM_SPLIT
	V2 = 2, //! Includes the encodings above
};

enum class TimeStampIsAdjustedToUTC : uint8_t {
	//! true if TZ, false if not
	AUTO,
	//! Always true
	ALWAYS_TRUE,
	//! Always false
	ALWAYS_FALSE,
};

class ParquetWriteTransformData {
public:
	ParquetWriteTransformData(ClientContext &context, const vector<LogicalType> &types,
	                          vector<unique_ptr<Expression>> expressions);

public:
	ColumnDataCollection &ApplyTransform(ColumnDataCollection &input);
	bool MatchesTypes(const vector<LogicalType> &other_types) const;

private:
	//! The buffer to store the transformed chunks of a rowgroup
	ColumnDataCollection buffer;
	//! The types used to bind the expressions and initialize the buffer
	vector<LogicalType> types;
	//! The expression(s) to apply to the input chunk
	vector<unique_ptr<Expression>> expressions;
	//! The expression executor used to transform the input chunk
	ExpressionExecutor executor;
	//! The intermediate chunk to target the transform to
	DataChunk chunk;
};

struct ParquetWriteLocalState : public LocalFunctionData {
public:
	explicit ParquetWriteLocalState(ClientContext &context, const vector<LogicalType> &types);

public:
	ColumnDataCollection buffer;
	ColumnDataAppendState append_state;
	//! If any of the column writers require a transformation to a different shape, this will be initialized and used
	unique_ptr<ParquetWriteTransformData> transform_data;
};

struct ParquetWriteGlobalState : public GlobalFunctionData {
public:
	ParquetWriteGlobalState();
	~ParquetWriteGlobalState() override;

public:
	unique_ptr<ParquetWriter> writer;
	optional_ptr<const PhysicalOperator> op;
	mutex lock;
	unique_ptr<ColumnDataCollection> combine_buffer;
	//! If any of the column writers require a transformation to a different shape, this will be initialized and used
	unique_ptr<ParquetWriteTransformData> transform_data;
	//! Row groups whose content-defined boundaries are scanned but whose heavy prepare is still running,
	//! oldest first - declared last so it is torn down before the writer the prepares reference
	std::deque<unique_ptr<ParquetCdcPipelineEntry>> cdc_pipeline;
	//! How many prepares may run at once; 0 until the first CDC batch sizes it from the scheduler
	idx_t cdc_pipeline_depth = 0;
};

struct ParquetWriterOptions {
	//! The file path to use for the written parquet file
	string file_name;
	//! The flags to use when opening the written parquet file
	FileOpenFlags open_flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
	//! Types of the columns
	vector<LogicalType> sql_types;
	//! Names of the columns
	vector<string> column_names;
	//! The compression codec to use for the written file
	duckdb_parquet::CompressionCodec::type codec;
	//! The field-ids to assign to the column(s) and their fields
	ChildFieldIDs field_ids;
	//! The mapping of name->type to shred VARIANT on, set explicitly by the user
	ShreddingType shredding_types;
	//! The encryption config for the file we're writing
	shared_ptr<ParquetEncryptionConfig> encryption_config;
	//! The maximum amount of items that can be put in a dictionary, per column/field
	optional_idx dictionary_size_limit;
	//! The maximum bytes of string-data to put into the dictionary, per column/field
	idx_t string_dictionary_page_size_limit;
	//! The maximum uncompressed size of a data page
	idx_t data_page_size_limit;
	//! Cut data pages at content-defined boundaries rather than only when they reach the size limit above. This
	//! changes *where* pages end, not what is in them: a page that contains no changed value comes out of a
	//! rewrite byte-identical, which is what lets a content-addressed store keep the bytes it already has.
	bool content_defined_chunking = false;
	//! The smallest page a content-defined cut will produce, in uncompressed value bytes. Together with the maximum
	//! it fixes the average page, which comes out (min + max) / 2 long - Arrow's semantics, see
	//! writer/parquet_cdc_chunker.hpp.
	idx_t cdc_min_chunk_size = PARQUET_CDC_DEFAULT_MIN_CHUNK_SIZE;
	//! The largest page a content-defined cut will produce, in uncompressed value bytes
	idx_t cdc_max_chunk_size = PARQUET_CDC_DEFAULT_MAX_CHUNK_SIZE;
	//! Bits taken off the boundary mask on top of what the two sizes imply; 0 is Arrow's default
	int32_t cdc_norm_level = PARQUET_CDC_DEFAULT_NORM_LEVEL;
	//! Whether to use bloom filters or not
	bool enable_bloom_filters;
	//! Maximum ratio of false-positives to allow in the written bloom filter
	double bloom_filter_false_positive_ratio;
	//! For the given 'codec', the compression level to use
	int64_t compression_level;
	//! Parquet version to use, higher versions enable more advanced compression modes
	ParquetVersion parquet_version;
	//! GEO Parquet version to use
	GeoParquetVersion geoparquet_version;
	//! Whether timestamp columns should be written as the INT96 type in the parquet file
	bool write_timestamp_as_int96;
	//! Whether to adjust timestamp values to UTC when writing
	TimeStampIsAdjustedToUTC timestamp_is_adjusted_to_utc;
	//! Which columns should be marked as 'required' in the written parquet file
	vector<bool> not_null_columns;
};

class ParquetWriter {
public:
	ParquetWriter(ClientContext &context, FileSystem &fs, ParquetWriterOptions &&options,
	              const vector<pair<string, string>> &kv_metadata);
	~ParquetWriter();

public:
	void PrepareRowGroup(ColumnDataCollection &buffer, PreparedRowGroup &result,
	                     unique_ptr<ParquetWriteTransformData> &transform_data,
	                     const ParquetCdcBoundaries *cdc_boundaries = nullptr);
	void FlushRowGroup(PreparedRowGroup &row_group);
	//! Prepares and writes a row group. The copy operators flush one batch at a time per file, which
	//! content-defined chunking relies on: its chunkers carry state from one row group to the next
	void Flush(ColumnDataCollection &buffer, unique_ptr<ParquetWriteTransformData> &transform_data);
	//! Rolls the content-defined chunker over a row group's values in file order, writing nothing, and
	//! returns the boundaries that PrepareRowGroup will then cut pages at. The chunker is the only state
	//! that needs file order, so this serial scan is what lets the heavy prepares run off the ordered path.
	unique_ptr<ParquetCdcBoundaries> ComputeCdcBoundaries(ColumnDataCollection &buffer);
	void Finalize();

	static duckdb_parquet::Type::type DuckDBTypeToParquetType(const LogicalType &duckdb_type,
	                                                          bool write_timestamp_as_int96);
	static void SetSchemaProperties(const LogicalType &duckdb_type, duckdb_parquet::SchemaElement &schema_ele,
	                                bool allow_geometry, ClientContext &context, bool write_timestamp_as_int96,
	                                TimeStampIsAdjustedToUTC timestamp_is_adjusted_to_utc);

	ClientContext &GetContext() {
		return context;
	}
	duckdb_apache::thrift::protocol::TProtocol *GetProtocol() {
		return protocol.get();
	}
	duckdb_parquet::CompressionCodec::type GetCodec() {
		return options.codec;
	}
	duckdb_parquet::Type::type GetType(idx_t schema_idx) {
		return file_meta_data.schema[schema_idx].type;
	}
	LogicalType GetSQLType(idx_t schema_idx) const {
		return options.sql_types[schema_idx];
	}
	AsyncFileWriter &GetWriter() {
		return *writer;
	}
	idx_t FileSize() const {
		return writer->GetTotalWritten();
	}
	optional_idx DictionarySizeLimit() const {
		return options.dictionary_size_limit;
	}
	idx_t StringDictionaryPageSizeLimit() const {
		return options.string_dictionary_page_size_limit;
	}
	idx_t DataPageSizeLimit() const {
		return options.data_page_size_limit;
	}
	bool ContentDefinedChunking() const {
		return options.content_defined_chunking;
	}
	idx_t CdcMinChunkSize() const {
		return options.cdc_min_chunk_size;
	}
	idx_t CdcMaxChunkSize() const {
		return options.cdc_max_chunk_size;
	}
	int32_t CdcNormLevel() const {
		return options.cdc_norm_level;
	}
	bool EnableBloomFilters() const {
		return options.enable_bloom_filters;
	}
	double BloomFilterFalsePositiveRatio() const {
		return options.bloom_filter_false_positive_ratio;
	}
	int64_t CompressionLevel() const {
		return options.compression_level;
	}
	idx_t NumberOfRowGroups() const {
		return file_meta_data.row_groups.size();
	}
	ParquetVersion GetParquetVersion() const {
		return options.parquet_version;
	}
	GeoParquetVersion GetGeoParquetVersion() const {
		return options.geoparquet_version;
	}
	bool WriteTimestampAsInt96() const {
		return options.write_timestamp_as_int96;
	}
	TimeStampIsAdjustedToUTC TimestampIsAdjustedToUTC() const {
		return options.timestamp_is_adjusted_to_utc;
	}
	const string &GetFileName() const {
		return options.file_name;
	}
	void AnalyzeSchema(ColumnDataCollection &buffer, vector<unique_ptr<ColumnWriter>> &column_writers);

	uint32_t Write(const duckdb_apache::thrift::TBase &object);
	uint32_t WriteData(const const_data_ptr_t buffer, const uint32_t buffer_size);
	unique_ptr<AsyncWriteBuffer> PrepareWrite(const duckdb_apache::thrift::TBase &object);
	unique_ptr<AsyncWriteBuffer> PrepareWriteData(unique_ptr<AsyncWriteBuffer> buffer);
	uint32_t WriteData(unique_ptr<AsyncWriteBuffer> buffer);

	GeoParquetFileMetadata &GetGeoParquetData();

	static bool TryGetParquetType(const LogicalType &duckdb_type,
	                              optional_ptr<duckdb_parquet::Type::type> type = nullptr,
	                              bool write_timestamp_as_int96 = false);

	void BufferBloomFilter(idx_t col_idx, unique_ptr<ParquetBloomFilter> bloom_filter);
	void SetWrittenStatistics(CopyFunctionFileStatistics &written_stats);
	void FlushColumnStats(idx_t col_idx, duckdb_parquet::ColumnChunk &chunk,
	                      optional_ptr<ColumnWriterStatistics> writer_stats);
	void FlushNestedColumnStats(idx_t schema_idx, idx_t null_count, idx_t num_values);
	void InitializePreprocessing(unique_ptr<ParquetWriteTransformData> &transform_data);
	void InitializeSchemaElements();

private:
	void GatherWrittenStatistics();
	void InitializeColumnOrders(idx_t unique_columns);
	void InitializeStatsUnifiers();
	void InitializeSchemaFromPreparedRowGroup(const PreparedRowGroup &prepared);
	void InitializeColumnWriters();
	idx_t InitializeColumnWriterSchemaIndices();
	PreparedParquetLayout ExportPreparedLayout() const;

	void VerifyPreparedRowGroup(const PreparedRowGroup &prepared) const;
#ifdef DEBUG
	idx_t LeafColumnWriterCounts() const;
#endif

private:
	ClientContext &context;
	ParquetWriterOptions options;
	shared_ptr<EncryptionUtil> encryption_util;

	unique_ptr<AsyncFileWriter> writer;
	std::shared_ptr<duckdb_apache::thrift::protocol::TProtocol> protocol;
	duckdb_parquet::FileMetaData file_meta_data;
	std::mutex lock;

	vector<unique_ptr<ColumnWriter>> column_writers;

	unique_ptr<GeoParquetFileMetadata> geoparquet_data;
	vector<ParquetBloomFilterEntry> bloom_filters;

	optional_ptr<CopyFunctionFileStatistics> written_stats;
	unique_ptr<ParquetStatsAccumulator> stats_accumulator;
};

} // namespace duckdb
