#include "duckdb/storage/compression/dictionary/analyze.hpp"
#include "duckdb/storage/compression/dictionary/compression.hpp"
#include "duckdb/storage/compression/dictionary/decompression.hpp"

#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/string_uncompressed.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"

/*
Data layout per segment:
+------------------------------------------------------+
|                  Header                              |
|   +----------------------------------------------+   |
|   |   dictionary_compression_header_t  header    |   |
|   +----------------------------------------------+   |
|                                                      |
+------------------------------------------------------+
|             Selection Buffer               |
|   +------------------------------------+   |
|   |   uint16_t index_buffer_idx[]      |   |
|   +------------------------------------+   |
|      tuple index -> index buffer idx       |
|                                            |
+--------------------------------------------+
|               Index Buffer                 |
|   +------------------------------------+   |
|   |   uint16_t  dictionary_offset[]    |   |
|   +------------------------------------+   |
|  string_index -> offset in the dictionary  |
|                                            |
+--------------------------------------------+
|                Dictionary                  |
|   +------------------------------------+   |
|   |   uint8_t *raw_string_data         |   |
|   +------------------------------------+   |
|      the string data without lengths       |
|                                            |
+--------------------------------------------+
*/

namespace duckdb {

struct DictionaryCompressionStorage {
	static unique_ptr<AnalyzeState> StringInitAnalyze(ColumnData &col_data, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, const Vector &input);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);

	static unique_ptr<CompressionState> InitCompression(ColumnDataCheckpointData &checkpoint_data,
	                                                    unique_ptr<AnalyzeState> state);
	static void Compress(CompressionState &state_p, const Vector &scan_vector);
	static void FinalizeCompress(CompressionState &state_p);

	static unique_ptr<SegmentScanState> StringInitScan(const QueryContext &context, ColumnSegment &segment);
	template <bool ALLOW_DICT_VECTORS>
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
unique_ptr<AnalyzeState> DictionaryCompressionStorage::StringInitAnalyze(ColumnData &col_data, PhysicalType type) {
	auto &storage_manager = col_data.GetStorageManager();
	if (StorageManager::TargetAtLeastVersion(StorageVersion::V1_3_0, storage_manager.GetStorageVersion())) {
		// dict_fsst introduced - disable dictionary
		return nullptr;
	}

	return make_uniq<DictionaryAnalyzeState>(col_data.GetBlockManager());
}

bool DictionaryCompressionStorage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<DictionaryAnalyzeState>();
	return DictionaryCompression::UpdateState(state, input);
}

idx_t DictionaryCompressionStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.Cast<DictionaryAnalyzeState>();

	if (state.current_tuple_count != 0) {
		state.UpdateMaxUniqueCount();
	}

	auto width = BitpackingPrimitives::MinimumBitWidth(state.current_unique_count + 1);
	auto req_space = DictionaryCompression::RequiredSpace(state.current_tuple_count, state.current_unique_count,
	                                                      state.current_dict_size, width);

	const auto total_space = state.segment_count * state.info.GetBlockSize() + req_space;
	return LossyNumericCast<idx_t>(DictionaryCompression::MINIMUM_COMPRESSION_RATIO * float(total_space));
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
unique_ptr<CompressionState> DictionaryCompressionStorage::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                                           unique_ptr<AnalyzeState> state_p) {
	const auto &state = state_p->Cast<DictionaryAnalyzeState>();
	return make_uniq<DictionaryCompressionCompressState>(checkpoint_data, state.max_unique_count_across_segments);
}

void DictionaryCompressionStorage::Compress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<DictionaryCompressionCompressState>();
	DictionaryCompression::UpdateState(state, scan_vector);
}

void DictionaryCompressionStorage::FinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<DictionaryCompressionCompressState>();
	state.Flush(true);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
unique_ptr<SegmentScanState> DictionaryCompressionStorage::StringInitScan(const QueryContext &context,
                                                                          ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	return make_uniq<CompressedStringScanState>(buffer_manager.Pin(context, segment.GetBlockHandle()), segment);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <bool ALLOW_DICT_VECTORS>
void DictionaryCompressionStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                                     Vector &result, idx_t result_offset) {
	// clear any previously locked buffers and get the primary buffer handle
	auto &scan_state = state.scan_state->Cast<CompressedStringScanState>();

	auto start = state.GetPositionInSegment();
	D_ASSERT(start <= segment.count.load());
	D_ASSERT(scan_count <= segment.count.load() - start);
	if (state.string_scan_type == StringScanType::COPY_STRINGS) {
		scan_state.ScanToFlatVector<true>(result, result_offset, start, scan_count, StringScanType::COPY_STRINGS);
		return;
	}
	if (!scan_state.dictionary) {
		scan_state.InitializeDictionary(segment);
	}
	if (!ALLOW_DICT_VECTORS || scan_count != STANDARD_VECTOR_SIZE) {
		scan_state.ScanToFlatVector(result, result_offset, start, scan_count);
	} else {
		scan_state.ScanToDictionaryVector(segment, result, result_offset, start, scan_count);
	}
}

void DictionaryCompressionStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                              Vector &result) {
	StringScanPartial<true>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction DictionaryCompressionFun::GetFunction(PhysicalType data_type) {
	return CompressionFunction(
	    CompressionType::COMPRESSION_DICTIONARY, data_type, DictionaryCompressionStorage ::StringInitAnalyze,
	    DictionaryCompressionStorage::StringAnalyze, DictionaryCompressionStorage::StringFinalAnalyze,
	    DictionaryCompressionStorage::InitCompression, DictionaryCompressionStorage::Compress,
	    DictionaryCompressionStorage::FinalizeCompress, DictionaryCompressionStorage::StringInitScan,
	    DictionaryCompressionStorage::StringScan, DictionaryCompressionStorage::StringScanPartial<false>,
	    UncompressedFunctions::EmptySkip, UncompressedStringStorage::StringInitSegment);
}

bool DictionaryCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	return physical_type == PhysicalType::VARCHAR;
}

} // namespace duckdb
