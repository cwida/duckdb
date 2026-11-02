#pragma once

#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/statistics/stats_writer.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"

namespace duckdb {

class EmptyValidityCompression {
public:
	struct EmptyValidityCompressionState : public CompressionState {
	public:
		explicit EmptyValidityCompressionState(ColumnDataCheckpointData &checkpoint_data)
		    : CompressionState(checkpoint_data, CompressionType::COMPRESSION_EMPTY) {
		}
		~EmptyValidityCompressionState() override {
		}

	public:
		idx_t count = 0;
		idx_t non_nulls = 0;
	};
	struct EmptyValiditySegmentScanState : public SegmentScanState {
		EmptyValiditySegmentScanState() {
		}
	};

public:
	static CompressionFunction CreateFunction() {
		CompressionFunction result(CompressionType::COMPRESSION_EMPTY, PhysicalType::BIT, nullptr, nullptr, nullptr,
		                           InitCompression, Compress, FinalizeCompress, InitScan, Scan, ScanPartial, Skip,
		                           InitSegment);
		result.filter = Filter;
		result.select = Select;
		result.supports_null_block = true;
		return result;
	}

public:
	static unique_ptr<CompressionState> InitCompression(ColumnDataCheckpointData &checkpoint_data,
	                                                    unique_ptr<AnalyzeState> state_p) {
		return make_uniq<EmptyValidityCompressionState>(checkpoint_data);
	}
	static void Compress(CompressionState &state_p, const Vector &scan_vector) {
		auto &state = state_p.Cast<EmptyValidityCompressionState>();
		UnifiedVectorFormat format;
		scan_vector.ToUnifiedFormat(format);
		const auto count = scan_vector.size();
		state.non_nulls += format.validity.CountValid(count);
		state.count += count;
	}
	static void FinalizeCompress(CompressionState &state_p) {
		auto &state = state_p.Cast<EmptyValidityCompressionState>();
		auto &checkpoint_data = state.checkpoint_data;

		auto compressed_segment = state.CreateNewSegment();
		compressed_segment->count = state.count;

		StatsWriter<void> stats_writer;
		if (state.non_nulls != state.count) {
			stats_writer.SetHasNull();
		}
		if (state.non_nulls != 0) {
			stats_writer.SetHasValid();
		}
		stats_writer.Merge(compressed_segment->GetStatsMutable());

		auto &buffer_manager = BufferManager::GetBufferManager(checkpoint_data.GetDatabase());
		auto handle = buffer_manager.Pin(compressed_segment->GetBlockHandle());

		auto &checkpoint_state = checkpoint_data.GetCheckpointState();
		checkpoint_state.FlushSegment(std::move(compressed_segment), std::move(handle), 0);
	}
	static unique_ptr<SegmentScanState> InitScan(const QueryContext &context, ColumnSegment &segment) {
		return make_uniq<EmptyValiditySegmentScanState>();
	}
	static void ScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                        idx_t result_offset) {
		return;
	}
	static void Scan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
		return;
	}
	static void Skip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
		return;
	}
	static unique_ptr<CompressedSegmentState> InitSegment(ColumnSegment &segment, block_id_t block_id,
	                                                      optional_ptr<ColumnSegmentState> segment_state) {
		return nullptr;
	}
	static void Filter(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
	                   SelectionVector &sel, idx_t &sel_count, const TableFilter &filter,
	                   TableFilterState &filter_state) {
	}
	static void Select(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
	                   const SelectionVector &sel, idx_t sel_count, idx_t result_offset, ScanVectorType scan_type) {
	}
};

} // namespace duckdb
