#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/checkpoint/write_overflow_strings_to_disk.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/partial_block_manager.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct FixedSizeAnalyzeState : public AnalyzeState {
	explicit FixedSizeAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager), count(0) {
	}

	idx_t count;
};

unique_ptr<AnalyzeState> FixedSizeInitAnalyze(ColumnData &col_data, PhysicalType type) {
	return make_uniq<FixedSizeAnalyzeState>(col_data.GetBlockManager());
}

bool FixedSizeAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<FixedSizeAnalyzeState>();
	state.count += input.size();
	return true;
}

template <class T>
idx_t FixedSizeFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.template Cast<FixedSizeAnalyzeState>();
	return sizeof(T) * state.count;
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
struct UncompressedCompressState : public CompressionState {
public:
	explicit UncompressedCompressState(ColumnDataCheckpointData &checkpoint_data);

public:
	virtual void CreateEmptySegment();
	void FlushSegment(idx_t segment_size);
	void Finalize(idx_t segment_size);
	idx_t FinalizeAppend();

public:
	unique_ptr<ColumnSegment> current_segment;
	ColumnAppendState append_state;
};

UncompressedCompressState::UncompressedCompressState(ColumnDataCheckpointData &checkpoint_data)
    : CompressionState(checkpoint_data, CompressionType::COMPRESSION_UNCOMPRESSED) {
	UncompressedCompressState::CreateEmptySegment();
}

void UncompressedCompressState::CreateEmptySegment() {
	auto &type = checkpoint_data.GetType();

	auto compressed_segment = CreateNewSegment();
	if (type.InternalType() == PhysicalType::VARCHAR) {
		auto &state = compressed_segment->GetSegmentState()->Cast<UncompressedStringSegmentState>();
		auto &storage_manager = checkpoint_data.GetStorageManager();
		if (!storage_manager.InMemory()) {
			auto &partial_block_manager = checkpoint_data.GetCheckpointState().GetPartialBlockManager();
			state.block_manager = partial_block_manager.GetBlockManager();
			state.overflow_writer = make_uniq<WriteOverflowStringsToDisk>(partial_block_manager);
		}
	}
	current_segment = std::move(compressed_segment);
	current_segment->InitializeAppend(append_state);
	append_state.InitializeStats(type);
}

void UncompressedCompressState::FlushSegment(idx_t segment_size) {
	auto &state = checkpoint_data.GetCheckpointState();
	if (current_segment->GetType().InternalType() == PhysicalType::VARCHAR) {
		auto &segment_state = current_segment->GetSegmentState()->Cast<UncompressedStringSegmentState>();
		if (segment_state.overflow_writer) {
			segment_state.overflow_writer->Flush();
			segment_state.overflow_writer.reset();
		}
	}
	append_state.child_appends.clear();
	append_state.append_state.reset();
	append_state.lock.reset();
	state.FlushSegmentInternal(std::move(current_segment), segment_size);
}

void UncompressedCompressState::Finalize(idx_t segment_size) {
	FlushSegment(segment_size);
	current_segment.reset();
}

idx_t UncompressedCompressState::FinalizeAppend() {
	current_segment->GetStatsMutable().Merge(*append_state.append_stats);
	return current_segment->FinalizeAppend(append_state);
}

unique_ptr<CompressionState> UncompressedFunctions::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                                    unique_ptr<AnalyzeState> state) {
	return make_uniq<UncompressedCompressState>(checkpoint_data);
}

void UncompressedFunctions::Compress(CompressionState &state_p, const Vector &data) {
	auto &state = state_p.Cast<UncompressedCompressState>();
	UnifiedVectorFormat vdata;
	data.ToUnifiedFormat(vdata);

	idx_t offset = 0;
	idx_t remaining = data.size();
	while (remaining > 0) {
		idx_t appended = state.current_segment->Append(state.append_state, vdata, offset, remaining);
		if (appended == remaining) {
			// appended everything: finished
			return;
		}
		// the segment is full: flush it to disk
		state.FlushSegment(state.FinalizeAppend());

		// now create a new segment and continue appending
		state.CreateEmptySegment();
		offset += appended;
		remaining -= appended;
	}
}

void UncompressedFunctions::FinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<UncompressedCompressState>();
	state.Finalize(state.FinalizeAppend());
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
struct FixedSizeScanState : public SegmentScanState {
	explicit FixedSizeScanState(BufferHandle handle_p, const ColumnSegment &segment)
	    : handle(std::move(handle_p)),
	      reader(CompressionSegmentReader::FromSegment(handle, segment, "fixed-size segment")) {
	}

	BufferHandle handle;
	CompressionSegmentReader reader;
};

unique_ptr<SegmentScanState> FixedSizeInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	return make_uniq<FixedSizeScanState>(std::move(handle), segment);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void FixedSizeScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                          idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<FixedSizeScanState>();
	auto start = state.GetPositionInSegment();

	auto source_data = scan_state.reader.GetArraySlice<T>(0, start, scan_count);

	// copy the data from the base table
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetDataMutable<T>(result);
	memcpy(result_data + result_offset, source_data.data(), scan_count * sizeof(T));
}

template <class T>
void FixedSizeScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	auto &scan_state = state.scan_state->template Cast<FixedSizeScanState>();
	auto start = state.GetPositionInSegment();

	auto source_data = scan_state.reader.GetArraySlice<T>(0, start, scan_count);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	FlatVector::SetData(result, data_ptr_cast(const_cast<T *>(source_data.data())), count_t(scan_count));
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
static unique_ptr<CompressionAppendState> FixedSizeInitAppend(ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(segment.GetBlockHandle());
	return make_uniq<CompressionAppendState>(std::move(handle));
}

struct StandardFixedSizeAppend {
	template <class T>
	static void Append(BaseStatistics &stats, data_ptr_t target, idx_t target_offset, UnifiedVectorFormat &adata,
	                   idx_t offset, idx_t count) {
		auto sdata = UnifiedVectorFormat::GetData<T>(adata);
		auto tdata = reinterpret_cast<T *>(target);
		if (adata.validity.CanHaveNull()) {
			for (idx_t i = 0; i < count; i++) {
				auto source_idx = adata.sel->get_index(offset + i);
				auto target_idx = target_offset + i;
				bool is_null = !adata.validity.RowIsValid(source_idx);
				if (!is_null) {
					stats.SetHasNoNullFast();
					stats.UpdateNumericStats<T>(sdata[source_idx]);
					tdata[target_idx] = sdata[source_idx];
				} else {
					stats.SetHasNullFast();
					// we insert a NullValue<T> in the null gap for debuggability
					// this value should never be used or read anywhere
					tdata[target_idx] = NullValue<T>();
				}
			}
		} else {
			stats.SetHasNoNullFast();
			for (idx_t i = 0; i < count; i++) {
				auto source_idx = adata.sel->get_index(offset + i);
				auto target_idx = target_offset + i;
				stats.UpdateNumericStats<T>(sdata[source_idx]);
				tdata[target_idx] = sdata[source_idx];
			}
		}
	}
};

struct ListFixedSizeAppend {
	template <class T>
	static void Append(BaseStatistics &stats, data_ptr_t target, idx_t target_offset, UnifiedVectorFormat &adata,
	                   idx_t offset, idx_t count) {
		auto sdata = UnifiedVectorFormat::GetData<uint64_t>(adata);
		auto tdata = reinterpret_cast<uint64_t *>(target);
		for (idx_t i = 0; i < count; i++) {
			auto source_idx = adata.sel->get_index(offset + i);
			auto target_idx = target_offset + i;
			tdata[target_idx] = sdata[source_idx];
		}
	}
};

template <class T, class OP>
idx_t FixedSizeAppend(CompressionAppendState &append_state, ColumnSegment &segment, BaseStatistics &stats,
                      UnifiedVectorFormat &data, idx_t offset, idx_t count) {
	auto target_ptr = append_state.handle.GetDataMutable() + segment.GetBlockOffset();
	idx_t max_tuple_count = segment.SegmentSize() / sizeof(T);
	idx_t copy_count = MinValue<idx_t>(count, max_tuple_count - segment.count);

	OP::template Append<T>(stats, target_ptr, segment.count, data, offset, copy_count);
	segment.count += copy_count;
	return copy_count;
}

template <class T>
idx_t FixedSizeFinalizeAppend(ColumnSegment &segment, BaseStatistics &stats) {
	return segment.count * sizeof(T);
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
template <class T, class APPENDER = StandardFixedSizeAppend>
CompressionFunction FixedSizeGetFunction(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_UNCOMPRESSED, data_type, FixedSizeInitAnalyze,
	                           FixedSizeAnalyze, FixedSizeFinalAnalyze<T>, UncompressedFunctions::InitCompression,
	                           UncompressedFunctions::Compress, UncompressedFunctions::FinalizeCompress,
	                           FixedSizeInitScan, FixedSizeScan<T>, FixedSizeScanPartial<T>,
	                           UncompressedFunctions::EmptySkip, nullptr, FixedSizeInitAppend,
	                           FixedSizeAppend<T, APPENDER>, FixedSizeFinalizeAppend<T>);
}

CompressionFunction FixedSizeUncompressed::GetFunction(PhysicalType data_type) {
	switch (data_type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return FixedSizeGetFunction<int8_t>(data_type);
	case PhysicalType::INT16:
		return FixedSizeGetFunction<int16_t>(data_type);
	case PhysicalType::INT32:
		return FixedSizeGetFunction<int32_t>(data_type);
	case PhysicalType::INT64:
		return FixedSizeGetFunction<int64_t>(data_type);
	case PhysicalType::UINT8:
		return FixedSizeGetFunction<uint8_t>(data_type);
	case PhysicalType::UINT16:
		return FixedSizeGetFunction<uint16_t>(data_type);
	case PhysicalType::UINT32:
		return FixedSizeGetFunction<uint32_t>(data_type);
	case PhysicalType::UINT64:
		return FixedSizeGetFunction<uint64_t>(data_type);
	case PhysicalType::INT128:
		return FixedSizeGetFunction<hugeint_t>(data_type);
	case PhysicalType::UINT128:
		return FixedSizeGetFunction<uhugeint_t>(data_type);
	case PhysicalType::FLOAT:
		return FixedSizeGetFunction<float>(data_type);
	case PhysicalType::DOUBLE:
		return FixedSizeGetFunction<double>(data_type);
	case PhysicalType::INTERVAL:
		return FixedSizeGetFunction<interval_t>(data_type);
	case PhysicalType::LIST:
		return FixedSizeGetFunction<uint64_t, ListFixedSizeAppend>(data_type);
	default:
		throw InternalException("Unsupported type for FixedSizeUncompressed::GetFunction");
	}
}

} // namespace duckdb
