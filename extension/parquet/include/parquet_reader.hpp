//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <exception>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "duckdb.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/once_flag.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_adaptive_filter_cache.hpp"
#include "duckdb/common/multi_file/multi_file_options.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "column_reader.hpp"
#include "parquet_page_index.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/mutex.hpp"
#include "parquet_prefetch_cost_model.hpp"
#include "parquet_file_metadata_cache.hpp"
#include "parquet_rle_bp_decoder.hpp"
#include "parquet_types.h"
#include "resizable_buffer.hpp"
#include "duckdb/execution/adaptive_filter.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/projection_index.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "parquet_column_schema.hpp"
#include "thrift/protocol/TProtocol.h"
#include "reader/string_column_reader.hpp"

namespace duckdb_apache {
namespace thrift {
class TBase;
} // namespace thrift
} // namespace duckdb_apache

namespace duckdb_parquet {
class EncryptionAlgorithm;
class FileMetaData;
class RowGroup;
class SchemaElement;

namespace format {
class FileMetaData;
}
} // namespace duckdb_parquet

namespace duckdb {
class Allocator;
class ClientContext;
class BaseStatistics;
class TableFilterSet;
class ParquetEncryptionConfig;
class ParquetReader;
class DataChunk;
class Deserializer;
class EncryptionUtil;
enum class ParquetIntervalBloomFilterVersion : uint8_t;
class PhysicalOperator;
class Serializer;
class TableFilter;
class ThriftFileTransport;
struct CryptoMetaData;
struct GlobalTableFunctionState;
struct LocalTableFunctionState;
struct PartitionStatistics;
struct TableFilterState;

struct ParquetReaderPrefetchConfig {
	//! Percentage of data in a row group span that should be scanned for enabling whole group prefetch
	static constexpr double WHOLE_GROUP_PREFETCH_MINIMUM_SCAN = 0.95;
	//! How many row groups need to produce at least one surviving row (from filtering)
	static constexpr double PREFETCH_FILTER_MINIMUM_MATCH_RATIO = 0.9;
	//! Before observing matches, only speculate on filter columns that are cheap to read
	static constexpr double FIRST_GROUP_PREFETCH_MAXIMUM_FILTER_SCAN = 0.05;
};

enum class ParquetPrefetchStrategy : uint8_t {
	NONE,
	WHOLE_GROUP,             //! Prefetches the whole group
	PREFETCH_FILTERS,        //! Used when we have fully selective filters, so they are prefetched earlier
	COLUMN_WISE_EAGER,       //! Used when we have projections and optional only filters
	PREFETCH_FILTERS_PAGES,  //! PREFETCH_FILTERS, reading only the pages the page index keeps
	COLUMN_WISE_EAGER_PAGES, //! COLUMN_WISE_EAGER, reading only the pages the page index keeps
};

const char *ParquetPrefetchStrategyToString(ParquetPrefetchStrategy strategy);

enum class ParquetPrefetchStrategyOption : uint8_t {
	AUTO,        //! Uses the runtime strategy to pick between ParquetPrefetchStrategy
	WHOLE_GROUP, //! Always do the whole row group
};

ParquetPrefetchStrategyOption ParquetPrefetchStrategyOptionFromString(const string &value);

template <>
const char *EnumUtil::ToChars<ParquetPrefetchStrategyOption>(ParquetPrefetchStrategyOption value);

template <>
ParquetPrefetchStrategyOption EnumUtil::FromString<ParquetPrefetchStrategyOption>(const char *value);

//! How INT96 (deprecated Impala timestamp) columns are exposed by the Parquet reader
enum class ParquetInt96AsOption : uint8_t {
	TIMESTAMP,    //! Read as TIMESTAMP (microsecond precision) - the default
	TIMESTAMP_NS, //! Read as TIMESTAMP_NS, preserving the nanosecond part (out-of-range values saturate)
	STRUCT        //! Read as STRUCT(date DATE, time TIME_NS), preserving the full range and precision
};

ParquetInt96AsOption ParquetInt96AsOptionFromString(const string &value);

template <>
const char *EnumUtil::ToChars<ParquetInt96AsOption>(ParquetInt96AsOption value);

template <>
ParquetInt96AsOption EnumUtil::FromString<ParquetInt96AsOption>(const char *value);

template <>
const char *EnumUtil::ToChars<StringColumnReader::Utf8ValidationOption>(StringColumnReader::Utf8ValidationOption value);

template <>
StringColumnReader::Utf8ValidationOption
EnumUtil::FromString<StringColumnReader::Utf8ValidationOption>(const char *value);

struct ParquetScanFilter {
	ParquetScanFilter(ClientContext &context, ProjectionIndex filter_idx, TableFilter &filter);
	~ParquetScanFilter();
	ParquetScanFilter(ParquetScanFilter &&) = default;

	ProjectionIndex filter_idx;
	TableFilter &filter;
	unique_ptr<TableFilterState> filter_state;
};

struct ParquetPrefetchColumn {
	string name;
	idx_t offset;

	ParquetPrefetchColumn(string name_p, idx_t offset_p) : name(std::move(name_p)), offset(offset_p) {
	}

	bool operator<(const ParquetPrefetchColumn &other) const {
		return offset < other.offset;
	}
};
//! Logger-only prefetch metrics: populated only when ParquetPrefetch logging is enabled.
struct ParquetLoggerPrefetchMetrics {
	//! Physical prefetch groups (column-name batches) issued for the current row group, in order
	vector<vector<string>> prefetch_groups;
	//! Tracks which scan_filters were evaluated in the current row group (indexed by scan_filter position)
	vector<bool> filters_used;
	//! Prefetch strategy chosen for the current row group
	ParquetPrefetchStrategy strategy = ParquetPrefetchStrategy::NONE;
	//! Accepted column gap (bytes)
	uint64_t accepted_column_gap = 0;

	void Reset() {
		prefetch_groups.clear();
		std::fill(filters_used.begin(), filters_used.end(), false);
		strategy = ParquetPrefetchStrategy::NONE;
		accepted_column_gap = 0;
	}

	//! Build a prefetch group
	void GeneratePrefetchGroup(ThriftFileTransport &trans, vector<ParquetPrefetchColumn> &requested_columns,
	                           const duckdb_parquet::ColumnChunkList &all_chunks);
};

struct ParquetPrefetchMetrics {
	//! Whether any filter was evaluated against the current row group
	bool filter_ran = false;
	//! Whether the current row group produced at least one surviving row after filtering
	bool had_match = false;
	//! Total number of row groups for which filters were evaluated across the scan
	idx_t row_groups_executed = 0;
	//! Number of those row groups that produced at least one surviving row
	idx_t row_groups_with_matches = 0;

	ParquetLoggerPrefetchMetrics logger;

	void FinalizeRowGroupSelectivity() {
		if (filter_ran) {
			row_groups_executed++;
			if (had_match) {
				row_groups_with_matches++;
			}
		}
		filter_ran = false;
		had_match = false;
		logger.Reset();
	}
};

struct ParquetReaderScanState {
public:
	ColumnReader &GetColumnReader(idx_t i);

public:
	//! The row group index this scan state decodes
	idx_t group_index;
	idx_t offset_in_group;
	idx_t group_offset;
	shared_ptr<CachingFileHandle> file_handle;
	vector<unique_ptr<ColumnReader>> column_readers;
	duckdb_base_std::unique_ptr<duckdb_apache::thrift::protocol::TProtocol> thrift_file_proto;

	//! Set while resuming payload-column decode after the filter-column I/O blocked (vs a fresh row-group pass)
	bool resuming_payload = false;
	SelectionVector sel;

	ResizeableBuffer define_buf;
	ResizeableBuffer repeat_buf;

	bool prefetch_mode = false;
	//! Number of filter head counts, used for prefetching
	idx_t filter_head_count = 0;
	//! Surviving row count
	idx_t filter_count = 0;

	ParquetPrefetchMetrics prefetch_metrics;

	//! Per-thread adaptive filter cache
	MultiFileAdaptiveFilterCache adaptive_filter_cache;
	//! Table filter list
	vector<ParquetScanFilter> scan_filters;
	//! true once the filter at this index has driven the surviving row count to zero
	vector<bool> filter_eliminated_all_rows;

	//! Whether the page index (ColumnIndex / OffsetIndex) is used to skip pages, see parquet_use_page_index
	bool use_page_index = true;
	//! The page index of the current row group, filled when it rules out pages
	ParquetRowGroupPageIndex page_index;
	//! Whether page_index.row_ranges restricts the rows the scan reads from the current row group
	bool page_index_active = false;
	//! The entry of page_index.row_ranges the scan is at
	idx_t page_index_range_idx = 0;

	//! (optional) pointer to the PhysicalOperator for logging
	optional_ptr<const PhysicalOperator> op;

	//! Per-thread counters for row groups whose data was read / skipped, incremented as row groups are processed.
	//! Read in ParquetScanGetMetrics and surfaced as the standard row_groups_scanned / total_row_groups_to_scan
	//! profiling metrics (the profiler sums them across threads).
	idx_t row_groups_read = 0;
	idx_t row_groups_skipped = 0;
	//! Compressed size of the column chunks of the row groups read, surfaced as bytes_scanned the same way
	idx_t bytes_scanned = 0;
	//! Compressed size of the column chunks of the registered row group, counted once its scan starts
	idx_t pending_bytes_scanned = 0;

	//! Prefetch cost model
	PrefetchCostModelState cost_model_state;
};

//! A column of the "schema" option, as plans serialized before that became an option of every multi-file reader
//! store it - see ParquetOptionsSerialization::legacy_schema
struct ParquetColumnDefinition {
public:
	MultiFileColumnDefinition ToMultiFileColumnDefinition() const;
	bool operator==(const ParquetColumnDefinition &other) const {
		return field_id == other.field_id && name == other.name && type == other.type &&
		       default_value == other.default_value && identifier == other.identifier && children == other.children;
	}

public:
	// DEPRECATED, use 'identifier' instead
	int32_t field_id;
	string name;
	LogicalType type;
	Value default_value;
	Value identifier;
	vector<ParquetColumnDefinition> children;

public:
	void Serialize(Serializer &serializer) const;
	static ParquetColumnDefinition Deserialize(Deserializer &deserializer);
};

struct ParquetOptions {
	explicit ParquetOptions() {
	}
	explicit ParquetOptions(ClientContext &context);

	bool binary_as_string = false;
	bool variant_legacy_encoding = false;
	shared_ptr<ParquetEncryptionConfig> encryption_config;

	idx_t explicit_cardinality = 0;
	bool can_have_nan = false; // if floats or doubles can contain NaN values (ignored if nan_count is present)
	ParquetPrefetchStrategyOption prefetch_strategy = ParquetPrefetchStrategyOption::AUTO;
	StringColumnReader::Utf8ValidationOption utf8_validation_option =
	    StringColumnReader::Utf8ValidationOption::STRICT_UTF8;
	ParquetInt96AsOption int96_as = ParquetInt96AsOption::TIMESTAMP;
};

struct ParquetOptionsSerialization {
	ParquetOptionsSerialization() = default;
	ParquetOptionsSerialization(ParquetOptions parquet_options_p, MultiFileOptions file_options_p)
	    : parquet_options(std::move(parquet_options_p)), file_options(std::move(file_options_p)) {
	}

	ParquetOptions parquet_options;
	MultiFileOptions file_options;
	//! Only read from plans serialized before "file_row_number" and "schema" became options of every multi-file
	//! reader - they are then moved into the file options
	bool legacy_file_row_number = false;
	vector<ParquetColumnDefinition> legacy_schema;

public:
	void Serialize(Serializer &serializer) const;
	static ParquetOptionsSerialization Deserialize(Deserializer &deserializer);
};

struct ParquetUnionData : public BaseUnionData {
	explicit ParquetUnionData(OpenFileInfo file_p) : BaseUnionData(std::move(file_p)) {
	}
	~ParquetUnionData() override;

	optional_idx TryGetCardinalityEstimate() const override;
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, const Identifier &name) override;

	ParquetOptions options;
	shared_ptr<ParquetFileMetadataCache> metadata;
	unique_ptr<ParquetColumnSchema> root_schema;
};

enum class ParquetReaderProjectionExpressionType : uint8_t {
	BYTE_LENGTH,
};

template <>
const char *EnumUtil::ToChars<ParquetReaderProjectionExpressionType>(ParquetReaderProjectionExpressionType value);

template <>
ParquetReaderProjectionExpressionType EnumUtil::FromString<ParquetReaderProjectionExpressionType>(const char *value);

struct ParquetReaderProjectionExpression {
	ParquetReaderProjectionExpressionType type;
	LogicalType return_type;

	void Serialize(Serializer &serializer) const;
	static ParquetReaderProjectionExpression Deserialize(Deserializer &deserializer);
};

class ParquetReader : public BaseFileReader {
public:
	//! Virtual column identifier for the "file_row_group_number" column (the file-relative row group index of each row)
	static constexpr column_t COLUMN_IDENTIFIER_FILE_ROW_GROUP_NUMBER = UINT64_C(9223372036854775820);

public:
	ParquetReader(ClientContext &context, OpenFileInfo file, ParquetOptions parquet_options,
	              shared_ptr<ParquetFileMetadataCache> metadata = nullptr,
	              unordered_map<idx_t, ParquetReaderProjectionExpression> projection_expressions = {});
	~ParquetReader() override;

	mutable CachingFileSystem fs;
	Allocator &allocator;
	shared_ptr<ParquetFileMetadataCache> metadata;
	ParquetOptions parquet_options;
	unique_ptr<ParquetColumnSchema> root_schema;
	shared_ptr<EncryptionUtil> encryption_util;
	bool can_use_metadata_statistics = false;
	//! How many rows have been read from this file
	atomic<idx_t> rows_read {0};
	ParquetIntervalBloomFilterVersion interval_bloom_filter_version {};
	//! Storage indices of columns where expressions like strlen/octet_length are pushed down
	unordered_map<idx_t, ParquetReaderProjectionExpression> projection_expressions;

public:
	string GetReaderType() const override {
		return "Parquet";
	}

	shared_ptr<BaseUnionData> GetUnionData(idx_t file_idx) override;
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, const Identifier &name) override;

	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	void PrepareScan(ClientContext &context, GlobalTableFunctionState &gstate_p,
	                 LocalTableFunctionState &lstate_p) override;
	AsyncResult ScheduleIO(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	AsyncResult Scan(ClientContext &context, GlobalTableFunctionState &global_state,
	                 LocalTableFunctionState &local_state, DataChunk &chunk) override;
	void FinishFile(ClientContext &context, GlobalTableFunctionState &gstate_p) override;
	double GetProgressInFile(ClientContext &context) override;
	void PrepareReadAhead(ClientContext &context, GlobalTableFunctionState &gstate) override;

public:
	//! Initialize the state for the next rowgroup to read
	void InitializeScan(ClientContext &context, ParquetReaderScanState &state, idx_t group_to_read) const;

	idx_t NumRows() const;
	idx_t NumRowGroups() const;
	idx_t GetFileSize() const;
	idx_t GetDataSize() const;

	const duckdb_parquet::FileMetaData *GetFileMetadata() const;
	ParquetIntervalBloomFilterVersion GetIntervalBloomFilterVersion() const {
		return interval_bloom_filter_version;
	}
	string static GetUniqueFileIdentifier(const duckdb_parquet::EncryptionAlgorithm &encryption_algorithm);

	uint32_t Read(duckdb_apache::thrift::TBase &object, TProtocol &iprot) const;
	uint32_t ReadEncrypted(duckdb_apache::thrift::TBase &object, TProtocol &iprot,
	                       CryptoMetaData &aad_crypto_metadata) const;
	uint32_t ReadData(duckdb_apache::thrift::protocol::TProtocol &iprot, const data_ptr_t buffer,
	                  const uint32_t buffer_size) const;
	uint32_t ReadDataEncrypted(duckdb_apache::thrift::protocol::TProtocol &iprot, const data_ptr_t buffer,
	                           const uint32_t buffer_size, CryptoMetaData &aad_crypto_metadata) const;

	unique_ptr<BaseStatistics> ReadStatistics(const Identifier &name);

	CachingFileHandle &GetHandle() {
		return *file_handle;
	}

	static unique_ptr<BaseStatistics> ReadStatistics(ClientContext &context, ParquetOptions parquet_options,
	                                                 shared_ptr<ParquetFileMetadataCache> metadata,
	                                                 const Identifier &name);
	static unique_ptr<BaseStatistics> ReadStatistics(ClientContext &context, const ParquetUnionData &union_data,
	                                                 const Identifier &name);
	//! The statistics of a virtual column of a file, read from its metadata
	static unique_ptr<BaseStatistics> ReadVirtualColumnStatistics(ClientContext &context,
	                                                              const ParquetOptions &parquet_options,
	                                                              const shared_ptr<ParquetFileMetadataCache> &metadata,
	                                                              column_t virtual_column_id);
	unique_ptr<BaseStatistics> GetVirtualColumnStatistics(ClientContext &context, column_t virtual_column_id) override;

	LogicalType DeriveLogicalType(const SchemaElement &s_ele, ParquetColumnSchema &schema) const;
	static LogicalType DeriveLogicalType(const SchemaElement &s_ele, const ParquetOptions &options,
	                                     ParquetColumnSchema &schema);

	void AddVirtualColumn(column_t virtual_column_id) override;

	void GetPartitionStats(vector<PartitionStatistics> &result);
	//! Construct a reader over the metadata of a file without opening the file - it can describe the schema and the
	//! statistics of the file, but not read it
	static shared_ptr<ParquetReader> CreateMetadataReader(ClientContext &context, ParquetOptions parquet_options,
	                                                      shared_ptr<ParquetFileMetadataCache> metadata);
	static void GetPartitionStats(const duckdb_parquet::FileMetaData &metadata, vector<PartitionStatistics> &result,
	                              optional_ptr<ParquetColumnSchema> root_schema = nullptr,
	                              optional_ptr<ParquetOptions> parquet_options = nullptr);
	static bool MetadataCacheEnabled(ClientContext &context);
	static shared_ptr<ParquetFileMetadataCache> GetMetadataCacheEntry(ClientContext &context, const OpenFileInfo &file);

private:
	//! Construct a parquet reader but **do not** open a file, used in ReadStatistics only
	ParquetReader(ClientContext &context, ParquetOptions parquet_options,
	              shared_ptr<ParquetFileMetadataCache> metadata);

	void InitializeSchema(ClientContext &context);
	//! Parse the schema of the file
	unique_ptr<ParquetColumnSchema> ParseSchema(ClientContext &context);
	ParquetColumnSchema ParseSchemaRecursive(idx_t depth, idx_t max_define, idx_t max_repeat, idx_t &next_schema_idx,
	                                         idx_t &next_file_idx, ClientContext &context);

	unique_ptr<ColumnReader> CreateReaderRecursive(ClientContext &context, const ColumnIndex &index,
	                                               const ParquetColumnSchema &schema) const;
	const duckdb_parquet::RowGroup &GetGroup(ParquetReaderScanState &state);
	uint64_t GetGroupCompressedSize(ParquetReaderScanState &state);
	idx_t GetGroupOffset(ParquetReaderScanState &state);
	//! Group span is the distance between the min page offset and the max page offset plus the max page compressed size
	uint64_t GetGroupSpan(ParquetReaderScanState &state);
	FilterPropagateResult CheckRowGroupFilterStatistics(ClientContext &context, ParquetReaderScanState &state,
	                                                    ColumnReader &column_reader, const TableFilter &filter);
	bool FiltersExcludeRowGroup(ClientContext &context, ParquetReaderScanState &state, bool dynamic_filters_only);
	void PrepareRowGroupBuffer(ClientContext &context, ParquetReaderScanState &state, idx_t out_col_idx);
	//! Reads the page index of the row group and works out which rows may match the filters; a page index that
	//! cannot be read or evaluated is ignored
	void PreparePageIndex(ClientContext &context, ParquetReaderScanState &state);
	void PreparePageIndexInternal(ClientContext &context, ParquetReaderScanState &state);
	//! Serves the page index of the whole file from one buffer that is read once per reader and shared by every row
	//! group and thread, when the index fits the transport's accepted column gap; returns false when it does not
	bool TryAttachPageIndexBuffer(ThriftFileTransport &transport) const;
	//! Moves the scan to the next rows of the row group that may match according to the page index, skipping the
	//! rows in between in every column reader; returns the number of rows to scan next (0 if none are left)
	idx_t SkipToPageIndexRows(ParquetReaderScanState &state);
	//! Whole-group prefetch strategy.
	ParquetPrefetchStrategy WholeGroupPrefetch(ParquetReaderScanState &state, ThriftFileTransport &trans,
	                                           const duckdb_parquet::RowGroup &group, uint64_t total_row_group_span,
	                                           bool log_prefetch);
	//! Column-wise prefetch strategy.
	ParquetPrefetchStrategy ColumnWisePrefetch(ParquetReaderScanState &state, ThriftFileTransport &trans,
	                                           const duckdb_parquet::RowGroup &group, bool filters_look_unselective,
	                                           bool log_prefetch) const;
	//! Register the read-heads to fetch, and select prefetch strategy
	ParquetPrefetchStrategy RegisterRowGroupReads(ClientContext &context, ParquetReaderScanState &state);
	//! Build the async I/O tasks for the registered read-heads
	AsyncResult ScheduleRowGroupReads(ParquetReaderScanState &state, ParquetPrefetchStrategy strategy);
	//! Process up to STANDARD_VECTOR_SIZE rows of the current row group into result.
	AsyncResult Process(ClientContext &context, ParquetReaderScanState &state, DataChunk &result);
	//! Log and finalize the row group's prefetch metrics
	void FinishRowGroup(ClientContext &context, ParquetReaderScanState &state, bool log_prefetch);
	//! Process filters
	AsyncResult ProcessFilters(ParquetReaderScanState &state, DataChunk &result, idx_t scan_count, uint8_t *define_ptr,
	                           uint8_t *repeat_ptr, bool log_prefetch);
	//! Run the filters into state.sel; returns the surviving row count. Advances every filter column.
	idx_t EvaluateFilters(ParquetReaderScanState &state, DataChunk &result, idx_t scan_count, uint8_t *define_ptr,
	                      uint8_t *repeat_ptr, bool log_prefetch);
	//! Async-fetch the surviving payload columns (stashing the filter columns); empty if no fetch is needed.
	vector<unique_ptr<AsyncTask>> ScheduleRemainingColumns(ParquetReaderScanState &state, DataChunk &result,
	                                                       idx_t scan_count);
	//! Read the remaining (non-filter) columns into result.
	void DecodeRemainingColumns(ParquetReaderScanState &state, DataChunk &result, idx_t filter_count,
	                            uint8_t *define_ptr, uint8_t *repeat_ptr);
	ParquetColumnSchema ParseColumnSchema(const SchemaElement &s_ele, idx_t max_define, idx_t max_repeat,
	                                      idx_t schema_index, idx_t column_index,
	                                      ParquetColumnSchemaType type = ParquetColumnSchemaType::COLUMN);

	MultiFileColumnDefinition ParseColumnDefinition(const duckdb_parquet::FileMetaData &file_meta_data,
	                                                ParquetColumnSchema &element);
	unique_ptr<AdditionalAuthenticatedData> GenerateAAD(uint8_t module_type, uint16_t row_group_ordinal,
	                                                    uint16_t column_ordinal, uint16_t page_ordinal) const;
	optional_ptr<const BaseStatistics> GetVariantStats(const ParquetColumnSchema &schema) const;
	//! Open a file handle for scanning, resolving the open flags from the prefetch mode
	unique_ptr<CachingFileHandle> OpenScanHandle(ClientContext &context) const;
	//! Initialize the shared scan handle, including when pre-opening it for read-ahead
	void InitializeScanHandle(ClientContext &context) const;

private:
	unique_ptr<CachingFileHandle> file_handle;
	mutable OnceFlag scan_handle_initialized;
	//! The byte range of the file that holds the page indexes of every row group (0 if there is none), computed once
	mutable OnceFlag page_index_span_computed;
	mutable idx_t page_index_span_begin = 0;
	mutable idx_t page_index_span_end = 0;
	//! The bytes of that range, read once and shared by every scan state of this reader
	mutable mutex page_index_buffer_lock;
	mutable AllocatedData page_index_buffer;
	mutable bool page_index_buffer_loaded = false;
	//! Scan handle shared by all scan states of this reader
	mutable shared_ptr<CachingFileHandle> shared_scan_handle;
	mutable annotated_mutex variant_stats_lock;
	mutable unordered_map<idx_t, unique_ptr<BaseStatistics>> variant_stats_cache;
};

} // namespace duckdb
