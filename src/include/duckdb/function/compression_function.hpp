//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/compression_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/enums/scan_vector_type.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/function/compression_info.hpp"

namespace duckdb {
class CompressionFunction;
class DatabaseInstance;
class ColumnData;
struct ColumnDataCheckpointData;
class ColumnSegment;
class SegmentStatistics;
class TableFilter;
struct TableFilterState;
struct ColumnSegmentState;
struct ColumnScanState;
struct PrefetchState;
struct SegmentScanState;

struct AnalyzeState {
	explicit AnalyzeState(BlockManager &block_manager_p) : block_manager(block_manager_p), info(block_manager) {
	}
	virtual ~AnalyzeState() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

	BlockManager &block_manager;
	CompressionInfo info;
};

struct CompressionState {
	explicit CompressionState(ColumnDataCheckpointData &checkpoint_data, CompressionType compression_type);
	virtual ~CompressionState() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
	const LogicalType &GetType();

	unique_ptr<ColumnSegment> CreateNewSegment();

public:
	ColumnDataCheckpointData &checkpoint_data;
	const CompressionFunction &function;
	BlockManager &block_manager;
	CompressionInfo info;
};

struct CompressedSegmentState {
	virtual ~CompressedSegmentState() {
	}

	//! Display info for PRAGMA storage_info
	virtual string GetSegmentInfo() const { // LCOV_EXCL_START
		return "";
	} // LCOV_EXCL_STOP

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct CompressionAppendState {
	explicit CompressionAppendState(BufferHandle handle_p) : handle(std::move(handle_p)) {
	}
	virtual ~CompressionAppendState() {
	}

	BufferHandle handle;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
//! The analyze functions are used to determine whether or not to use this compression method
//! The system first determines the potential compression methods to use based on the physical type of the column
//! After that the following steps are taken:
//! 1. The init_analyze is called to initialize the analyze state of every candidate compression method
//! 2. The analyze method is called with all of the input data in the order in which it must be stored.
//!    analyze can return "false". In that case, the compression method is taken out of consideration early.
//! 3. The final_analyze method is called, which should return a score for the compression method

//! The system then decides which compression function to use based on the analyzed score (returned from final_analyze)
typedef unique_ptr<AnalyzeState> (*compression_init_analyze_t)(ColumnData &col_data, PhysicalType type);
typedef bool (*compression_analyze_t)(AnalyzeState &state, const Vector &input);
typedef idx_t (*compression_final_analyze_t)(AnalyzeState &state);

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
typedef unique_ptr<CompressionState> (*compression_init_compression_t)(ColumnDataCheckpointData &checkpoint_data,
                                                                       unique_ptr<AnalyzeState> state);
typedef void (*compression_compress_data_t)(CompressionState &state, const Vector &scan_vector);
typedef void (*compression_compress_finalize_t)(CompressionState &state);

//===--------------------------------------------------------------------===//
// Uncompress / Scan
//===--------------------------------------------------------------------===//
typedef void (*compression_init_prefetch_t)(ColumnSegment &segment, PrefetchState &prefetch_state);
typedef unique_ptr<SegmentScanState> (*compression_init_segment_scan_t)(const QueryContext &context,
                                                                        ColumnSegment &segment);

//! Function prototype used for reading an entire vector (STANDARD_VECTOR_SIZE)
typedef void (*compression_scan_vector_t)(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                          Vector &result);
//! Function prototype used for reading an arbitrary ('scan_count') number of values
typedef void (*compression_scan_partial_t)(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                           Vector &result, idx_t result_offset);
//! Function prototype used for reading a subset of the values of a vector indicated by a selection vector
//! The selected values are written to 'result' starting at 'result_offset'. Emitting a compressed vector (e.g. a
//! constant vector) is only allowed if 'scan_type' is SCAN_ENTIRE_VECTOR
typedef void (*compression_select_t)(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                                     const SelectionVector &sel, idx_t sel_count, idx_t result_offset,
                                     ScanVectorType scan_type);
//! Function prototype used for applying a filter to a vector while scanning that vector
typedef void (*compression_filter_t)(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                                     SelectionVector &sel, idx_t &sel_count, const TableFilter &filter,
                                     TableFilterState &filter_state);
//! Function prototype used for skipping 'skip_count' values, non-trivial if random-access is not supported for the
//! compressed data.
typedef void (*compression_skip_t)(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count);

//===--------------------------------------------------------------------===//
// Append (optional)
//===--------------------------------------------------------------------===//
typedef unique_ptr<CompressedSegmentState> (*compression_init_segment_t)(
    ColumnSegment &segment, block_id_t block_id, optional_ptr<ColumnSegmentState> segment_state);
typedef unique_ptr<CompressionAppendState> (*compression_init_append_t)(ColumnSegment &segment);
typedef idx_t (*compression_append_t)(CompressionAppendState &append_state, ColumnSegment &segment,
                                      BaseStatistics &stats, UnifiedVectorFormat &data, idx_t offset, idx_t count);
typedef idx_t (*compression_finalize_append_t)(ColumnSegment &segment, BaseStatistics &stats);
typedef void (*compression_revert_append_t)(ColumnSegment &segment, idx_t new_count);

//===--------------------------------------------------------------------===//
// Serialization (optional)
//===--------------------------------------------------------------------===//
//! Function prototype for serializing the segment state
typedef unique_ptr<ColumnSegmentState> (*compression_serialize_state_t)(ColumnSegment &segment);
//! Function prototype for deserializing the segment state
typedef unique_ptr<ColumnSegmentState> (*compression_deserialize_state_t)(Deserializer &deserializer);
//! Function prototype for cleaning up the segment state when the column data is dropped
typedef void (*compression_visit_block_ids_t)(const ColumnSegment &segment, BlockIdVisitor &visitor);

//===--------------------------------------------------------------------===//
// GetSegmentInfo (optional)
//===--------------------------------------------------------------------===//
//! Function prototype for retrieving segment information straight from the column segment
typedef InsertionOrderPreservingMap<string> (*compression_get_segment_info_t)(QueryContext context,
                                                                              ColumnSegment &segment);

enum class CompressionValidity : uint8_t { REQUIRES_VALIDITY, NO_VALIDITY_REQUIRED };

class CompressionFunction {
public:
	CompressionFunction(CompressionType type, PhysicalType data_type, compression_init_analyze_t init_analyze,
	                    compression_analyze_t analyze, compression_final_analyze_t final_analyze,
	                    compression_init_compression_t init_compression, compression_compress_data_t compress,
	                    compression_compress_finalize_t compress_finalize, compression_init_segment_scan_t init_scan,
	                    compression_scan_vector_t scan_vector, compression_scan_partial_t scan_partial,
	                    compression_skip_t skip, compression_init_segment_t init_segment = nullptr,
	                    compression_init_append_t init_append = nullptr, compression_append_t append = nullptr,
	                    compression_finalize_append_t finalize_append = nullptr,
	                    compression_revert_append_t revert_append = nullptr,
	                    compression_serialize_state_t serialize_state = nullptr,
	                    compression_deserialize_state_t deserialize_state = nullptr,
	                    compression_visit_block_ids_t visit_block_ids = nullptr,
	                    compression_init_prefetch_t init_prefetch = nullptr, compression_select_t select = nullptr,
	                    compression_filter_t filter = nullptr)
	    : type(type), data_type(data_type), init_analyze(init_analyze), analyze(analyze), final_analyze(final_analyze),
	      init_compression(init_compression), compress(compress), compress_finalize(compress_finalize),
	      init_prefetch(init_prefetch), init_scan(init_scan), scan_vector(scan_vector), scan_partial(scan_partial),
	      select(select), filter(filter), skip(skip), init_segment(init_segment), init_append(init_append),
	      append(append), finalize_append(finalize_append), revert_append(revert_append),
	      serialize_state(serialize_state), deserialize_state(deserialize_state), visit_block_ids(visit_block_ids) {
	}

	//! Compression type
	CompressionType type;
	//! The data type this function can compress
	PhysicalType data_type;

	//! Analyze step: determine which compression function is the most effective
	//! init_analyze is called once to set up the analyze state
	compression_init_analyze_t init_analyze;
	//! analyze is called several times (once per vector in the row group)
	//! analyze should return true, unless compression is no longer possible with this compression method
	//! in that case false should be returned
	compression_analyze_t analyze;
	//! final_analyze should return the score of the compression function
	//! ideally this is the exact number of bytes required to store the data
	//! this is not required/enforced: it can be an estimate as well
	//! also this function can return DConstants::INVALID_INDEX to skip this compression method
	compression_final_analyze_t final_analyze;

	//! Compression step: actually compress the data
	//! init_compression is called once to set up the comperssion state
	compression_init_compression_t init_compression;
	//! compress is called several times (once per vector in the row group)
	compression_compress_data_t compress;
	//! compress_finalize is called after
	compression_compress_finalize_t compress_finalize;

	//! Initialize prefetch state with required I/O data to scan this segment
	compression_init_prefetch_t init_prefetch;
	//! init_scan is called to set up the scan state
	compression_init_segment_scan_t init_scan;
	//! scan_vector scans an entire vector using the scan state
	compression_scan_vector_t scan_vector;
	//! scan_partial scans a subset of a vector
	//! this can request > vector_size as well
	//! this is used if a vector crosses segment boundaries, or for child columns of lists
	compression_scan_partial_t scan_partial;
	//! scan a subset of a vector
	compression_select_t select;
	//! Scan and apply a filter to a vector while scanning
	compression_filter_t filter;
	//! Skip forward in the compressed segment
	compression_skip_t skip;

	// Append functions
	//! This only really needs to be defined for uncompressed segments

	//! Initialize a compressed segment (optional)
	compression_init_segment_t init_segment;
	//! Initialize the append state (optional)
	compression_init_append_t init_append;
	//! Append to the compressed segment (optional)
	compression_append_t append;
	//! Finalize an append to the segment
	compression_finalize_append_t finalize_append;
	//! Revert append (optional)
	compression_revert_append_t revert_append;

	// State serialize functions
	//! This is only necessary if the segment state has information that must be written to disk in the metadata

	//! Serialize the segment state to the metadata (optional)
	compression_serialize_state_t serialize_state;
	//! Deserialize the segment state to the metadata (optional)
	compression_deserialize_state_t deserialize_state;
	//! Iterate over any extra block ids used by the compression algorithm (optional)
	compression_visit_block_ids_t visit_block_ids;

	// Get Segment Info
	//! This is only necessary if you want to convey more information about the segment in the 'pragma_storage_info'
	//! result

	//! Get stringified segment information directly from reading the column segment
	compression_get_segment_info_t get_segment_info = nullptr;

	//! Whether the validity mask should be separately compressed
	//! or this compression function can also be used to decompress the validity
	CompressionValidity validity = CompressionValidity::REQUIRES_VALIDITY;

	//! Whether persistent segments can be stored without a backing data block.
	bool supports_null_block = false;
};

//! The set of compression functions
struct CompressionFunctionSet {
	static constexpr idx_t COMPRESSION_TYPE_COUNT = 16;
	static constexpr idx_t PHYSICAL_TYPE_COUNT = 19;

public:
	CompressionFunctionSet();

	vector<reference<const CompressionFunction>> GetCompressionFunctions(PhysicalType physical_type);
	optional_ptr<const CompressionFunction> GetCompressionFunction(CompressionType type, PhysicalType physical_type);
	void SetDisabledCompressionMethods(const vector<CompressionType> &methods);
	vector<CompressionType> GetDisabledCompressionMethods() const;

private:
	atomic<bool> is_disabled[COMPRESSION_TYPE_COUNT];
	vector<vector<CompressionFunction>> functions;

private:
	static idx_t GetCompressionIndex(PhysicalType physical_type);
	static idx_t GetCompressionIndex(CompressionType type);
	void ResetDisabledMethods();
};

} // namespace duckdb
