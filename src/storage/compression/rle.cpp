#include "duckdb/common/types/null_value.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include <functional>

namespace duckdb {

using rle_count_t = uint16_t;

template <class T>
bool RLEValueEqual(const T &lhs, const T &rhs) {
	return lhs == rhs;
}

template <>
bool RLEValueEqual(const float &lhs, const float &rhs) {
	// IEEE == collapses +0 and -0; compare the bit pattern instead.
	return Load<uint32_t>(const_data_ptr_cast(&lhs)) == Load<uint32_t>(const_data_ptr_cast(&rhs));
}

template <>
bool RLEValueEqual(const double &lhs, const double &rhs) {
	return Load<uint64_t>(const_data_ptr_cast(&lhs)) == Load<uint64_t>(const_data_ptr_cast(&rhs));
}

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct EmptyRLEWriter {
	template <class VALUE_TYPE>
	static void Operation(VALUE_TYPE value, rle_count_t count, void *dataptr, bool is_null) {
	}
};

template <class T>
struct RLEState {
	RLEState() : seen_count(0), last_value(NullValue<T>()), last_seen_count(0), dataptr(nullptr) {
	}

	idx_t seen_count;
	T last_value;
	rle_count_t last_seen_count;
	void *dataptr;
	bool all_null = true;

public:
	template <class OP>
	void Flush() {
		OP::template Operation<T>(last_value, last_seen_count, dataptr, all_null);
	}

	template <class OP = EmptyRLEWriter>
	void Update(const T *data, ValidityMask &validity, idx_t idx) {
		if (validity.RowIsValid(idx)) {
			if (all_null) {
				// no value seen yet
				// assign the current value, and increment the seen_count
				// note that we increment last_seen_count rather than setting it to 1
				// this is intentional: this is the first VALID value we see
				// but it might not be the first value in case of nulls!
				last_value = data[idx];
				seen_count++;
				last_seen_count++;
				all_null = false;
			} else if (RLEValueEqual(last_value, data[idx])) {
				// the last value is identical to this value: increment the last_seen_count
				last_seen_count++;
			} else {
				// the values are different
				// issue the callback on the last value
				// edge case: if a value has exactly 2^16 repeated values, we can end up here with last_seen_count = 0
				if (last_seen_count > 0) {
					Flush<OP>();
					seen_count++;
				}

				// increment the seen_count and put the new value into the RLE slot
				last_value = data[idx];
				last_seen_count = 1;
			}
		} else {
			// NULL value: we merely increment the last_seen_count
			last_seen_count++;
		}
		if (last_seen_count == NumericLimits<rle_count_t>::Maximum()) {
			// we have seen the same value so many times in a row we are at the limit of what fits in our count
			// write away the value and move to the next value
			Flush<OP>();
			last_seen_count = 0;
			seen_count++;
		}
	}
};

template <class T>
struct RLEAnalyzeState : public AnalyzeState {
	explicit RLEAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager) {
	}

	RLEState<T> state;
};

template <class T>
unique_ptr<AnalyzeState> RLEInitAnalyze(ColumnData &col_data, PhysicalType type) {
	return make_uniq<RLEAnalyzeState<T>>(col_data.GetBlockManager());
}

template <class T>
bool RLEAnalyze(AnalyzeState &state, const Vector &input) {
	auto &rle_state = state.template Cast<RLEAnalyzeState<T>>();
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);

	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	const auto count = input.size();
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		rle_state.state.Update(data, vdata.validity, idx);
	}
	return true;
}

template <class T>
idx_t RLEFinalAnalyze(AnalyzeState &state) {
	auto &rle_state = state.template Cast<RLEAnalyzeState<T>>();
	return (sizeof(rle_count_t) + sizeof(T)) * rle_state.state.seen_count;
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
struct RLEConstants {
	static constexpr const idx_t RLE_HEADER_SIZE = sizeof(uint64_t);
};

template <class T, bool WRITE_STATISTICS>
struct RLECompressState : public StandardCompressionState {
	explicit RLECompressState(ColumnDataCheckpointData &checkpoint_data_p)
	    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_RLE) {
		CreateEmptySegment();

		state.dataptr = (void *)this;
		max_rle_count = MaxRLECount();
	}

	struct RLEWriter {
		template <class VALUE_TYPE>
		static void Operation(VALUE_TYPE value, rle_count_t count, void *dataptr, bool is_null) {
			auto state = reinterpret_cast<RLECompressState<T, WRITE_STATISTICS> *>(dataptr);
			state->WriteValue(value, count, is_null);
		}
	};

	idx_t MaxRLECount() {
		auto entry_size = sizeof(T) + sizeof(rle_count_t);
		return AlignValueFloor((info.GetBlockSize() - RLEConstants::RLE_HEADER_SIZE) / entry_size);
	}

	void CreateEmptySegment() {
		CreateAndPinNewSegment();
	}

	void Append(UnifiedVectorFormat &vdata, idx_t count) {
		auto data = UnifiedVectorFormat::GetData<T>(vdata);
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			if (WRITE_STATISTICS && !vdata.validity.RowIsValid(idx)) {
				stats_writer.SetHasNull();
			}
			state.template Update<RLECompressState<T, WRITE_STATISTICS>::RLEWriter>(data, vdata.validity, idx);
		}
	}

	void WriteValue(T value, rle_count_t count, bool is_null) {
		// write the RLE entry
		auto handle_ptr = handle.GetDataMutable() + RLEConstants::RLE_HEADER_SIZE;
		auto data_pointer = reinterpret_cast<T *>(handle_ptr);
		auto index_pointer = reinterpret_cast<rle_count_t *>(handle_ptr + max_rle_count * sizeof(T));
		data_pointer[entry_count] = value;
		index_pointer[entry_count] = count;
		entry_count++;

		// update meta data
		if (WRITE_STATISTICS) {
			if (!is_null) {
				stats_writer.Update(value);
			} else {
				stats_writer.SetHasNull();
			}
		}
		current_segment->count += count;

		if (entry_count == max_rle_count) {
			// we have finished writing this segment: flush it and create a new segment
			FlushSegment();
			CreateEmptySegment();
			entry_count = 0;
		}
	}

	void FlushSegment() {
		// flush the segment
		// we compact the segment by moving the counts so they are directly next to the values
		idx_t counts_size = sizeof(rle_count_t) * entry_count;
		idx_t original_rle_offset = RLEConstants::RLE_HEADER_SIZE + max_rle_count * sizeof(T);
		idx_t minimal_rle_offset = RLEConstants::RLE_HEADER_SIZE + sizeof(T) * entry_count;
		idx_t aligned_rle_offset = AlignValue(minimal_rle_offset);
		idx_t total_segment_size = aligned_rle_offset + counts_size;
		auto data_ptr = handle.GetDataMutable();
		if (aligned_rle_offset > minimal_rle_offset) {
			memset(data_ptr + minimal_rle_offset, 0, aligned_rle_offset - minimal_rle_offset);
		}
		memmove(data_ptr + aligned_rle_offset, data_ptr + original_rle_offset, counts_size);
		// store the final RLE offset within the segment
		Store<uint64_t>(aligned_rle_offset, data_ptr);

		FlushCurrentSegment(stats_writer, total_segment_size);
	}

	void Finalize() {
		state.template Flush<RLECompressState<T, WRITE_STATISTICS>::RLEWriter>();

		FlushSegment();
		current_segment.reset();
	}

	RLEState<T> state;
	StatsWriter<T> stats_writer;
	idx_t entry_count = 0;
	idx_t max_rle_count;
};

template <class T, bool WRITE_STATISTICS>
unique_ptr<CompressionState> RLEInitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                unique_ptr<AnalyzeState> state) {
	return make_uniq<RLECompressState<T, WRITE_STATISTICS>>(checkpoint_data);
}

template <class T, bool WRITE_STATISTICS>
void RLECompress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<RLECompressState<T, WRITE_STATISTICS>>();
	UnifiedVectorFormat vdata;
	scan_vector.ToUnifiedFormat(vdata);

	state.Append(vdata, scan_vector.size());
}

template <class T, bool WRITE_STATISTICS>
void RLEFinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<RLECompressState<T, WRITE_STATISTICS>>();
	state.Finalize();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
[[noreturn]] static void ThrowRLECountOffsetInvalid() {
	throw DataCorruptionException("Corrupted RLE segment: rle_count_offset is corrupted");
}

[[noreturn]] static void ThrowRLERunCountArrayExhausted() {
	throw DataCorruptionException("Corrupted RLE segment: run counts do not cover the segment row count");
}

[[noreturn]] static void ThrowRLERunCountExceedsRemaining(rle_count_t run_count, idx_t remaining_count) {
	throw DataCorruptionException("Corrupted RLE segment: run count %d exceeds the remaining row count %d", run_count,
	                              remaining_count);
}

template <class T>
struct RLEScanState : public SegmentScanState {
	struct ValidatedRun {
		T value;
		rle_count_t length;
	};

	struct SegmentLayout {
		SegmentLayout(unsafe_array_ptr<const T> values_p, unsafe_array_ptr<const rle_count_t> run_counts_p,
		              idx_t entry_capacity_p, idx_t segment_count)
		    : values(values_p), run_counts(run_counts_p), entry_capacity(entry_capacity_p), validated_entry_count(0),
		      unvalidated_row_count(segment_count) {
		}

		//! Validate enough runs to cover every row in the segment.
		idx_t ValidateAllRuns() {
			while (unvalidated_row_count > 0) {
				ValidateThrough(validated_entry_count);
			}
			return validated_entry_count;
		}

		//! Validate through entry_index and return that run.
		ValidatedRun ValidateAndGetRun(idx_t entry_index) {
			ValidateThrough(entry_index);
			return {values[entry_index], run_counts[entry_index]};
		}

		//! Return the first validated_run_count run values.
		unsafe_array_ptr<const T> GetRunValues(idx_t validated_run_count) const {
			D_ASSERT(validated_run_count <= validated_entry_count);
			return values.SubArray(0, validated_run_count);
		}

		idx_t GetEntryCapacity() const {
			return entry_capacity;
		}

	private:
		//! Validate through entry_index without advancing the scan.
		void ValidateThrough(idx_t entry_index) {
			while (validated_entry_count <= entry_index) {
				if (validated_entry_count >= entry_capacity) {
					ThrowRLERunCountArrayExhausted();
				}
				auto run_count = run_counts[validated_entry_count];
				if (run_count > unvalidated_row_count) {
					ThrowRLERunCountExceedsRemaining(run_count, unvalidated_row_count);
				}
				unvalidated_row_count -= run_count;
				validated_entry_count++;
			}
		}

		//! Values read from the segment between RLE_HEADER_SIZE and rle_count_offset.
		unsafe_array_ptr<const T> values;
		//! Run counts read from the segment between rle_count_offset and the segment end.
		unsafe_array_ptr<const rle_count_t> run_counts;
		//! Number of value/run-count pairs that fit in both segment regions.
		idx_t entry_capacity;
		//! Number of run counts already validated against the segment row count.
		idx_t validated_entry_count;
		//! Segment rows not yet covered by validated run counts.
		idx_t unvalidated_row_count;
	};

	static SegmentLayout ReadSegmentLayout(const BufferHandle &handle, ColumnSegment &segment, idx_t segment_count) {
		auto reader = CompressionSegmentReader::FromSegment(handle, segment, "RLE segment");
		auto rle_count_offset = reader.Read<uint64_t>();
		if (rle_count_offset < RLEConstants::RLE_HEADER_SIZE || rle_count_offset > reader.Size()) {
			ThrowRLECountOffsetInvalid();
		}

		auto value_capacity = (rle_count_offset - RLEConstants::RLE_HEADER_SIZE) / sizeof(T);
		auto count_capacity = (reader.Size() - rle_count_offset) / sizeof(rle_count_t);
		auto entry_capacity = MinValue<idx_t>(value_capacity, count_capacity);
		auto values = reader.template GetArray<T>(RLEConstants::RLE_HEADER_SIZE, value_capacity);
		auto run_counts = reader.template GetArray<rle_count_t>(rle_count_offset, count_capacity);
		return SegmentLayout(values, run_counts, entry_capacity, segment_count);
	}

	explicit RLEScanState(BufferHandle handle_p, ColumnSegment &segment)
	    : handle(std::move(handle_p)), segment_count(segment.count.load()),
	      layout(ReadSegmentLayout(handle, segment, segment_count)), entry_pos(0), position_in_entry(0) {
	}

	//! Advances past empty entries while validating the current run.
	ValidatedRun ValidateAndGetCurrentRun() {
		while (true) {
			auto run = layout.ValidateAndGetRun(entry_pos);
			if (run.length != 0) {
				return run;
			}
			// Empty runs do not represent rows.
			ForwardToNextRun();
		}
	}

	inline void SkipInternal(idx_t skip_count) {
		while (skip_count > 0) {
			auto run_count = ValidateAndGetCurrentRun().length;
			D_ASSERT(position_in_entry < run_count);
			idx_t skip_amount = MinValue<idx_t>(skip_count, run_count - position_in_entry);

			skip_count -= skip_amount;
			position_in_entry += skip_amount;
			if (ExhaustedRun(run_count)) {
				ForwardToNextRun();
			}
		}
	}

	void Skip(ColumnSegment &segment, idx_t skip_count) {
		SkipInternal(skip_count);
	}

	inline void ForwardToNextRun() {
		// handled all entries in this RLE value
		// move to the next entry
		entry_pos++;
		if (entry_pos > layout.GetEntryCapacity()) {
			ThrowRLERunCountArrayExhausted();
		}
		position_in_entry = 0;
	}

	inline bool ExhaustedRun(rle_count_t run_length) const {
		return position_in_entry >= run_length;
	}

	BufferHandle handle;
	//! Segment row count loaded from the file and used to validate runs as they are reached.
	const idx_t segment_count;
	SegmentLayout layout;
	idx_t entry_pos;
	idx_t position_in_entry;
	//! If we are running a filter over the column - the runs that match the filter
	unsafe_unique_array<bool> matching_runs;
	idx_t matching_run_count = 0;
};

template <class T>
unique_ptr<SegmentScanState> RLEInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	auto result = make_uniq<RLEScanState<T>>(std::move(handle), segment);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void RLESkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	D_ASSERT(state.GetPositionInSegment() <= segment.count);
	D_ASSERT(skip_count <= state.GetPositionInSegment());
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();
	scan_state.Skip(segment, skip_count);
}

template <bool ENTIRE_VECTOR>
static bool CanEmitConstantVector(idx_t position, idx_t run_length, idx_t scan_count) {
	if (!ENTIRE_VECTOR) {
		return false;
	}
	if (scan_count != STANDARD_VECTOR_SIZE) {
		// Only when we can fill an entire Vector can we emit a ConstantVector, because subsequent scans require the
		// input Vector to be flat
		return false;
	}
	D_ASSERT(position < run_length);
	auto remaining_in_run = run_length - position;
	// The amount of values left in this run are equal or greater than the amount of values we need to scan
	return remaining_in_run >= scan_count;
}

template <class T>
static void RLEScanConstant(RLEScanState<T> &scan_state, const typename RLEScanState<T>::ValidatedRun &run,
                            idx_t scan_count, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	FlatVector::SetSize(result, count_t(scan_count));
	auto result_data = ConstantVector::GetData<T>(result);
	result_data[0] = run.value;
	scan_state.position_in_entry += scan_count;
	if (scan_state.ExhaustedRun(run.length)) {
		scan_state.ForwardToNextRun();
	}
}

template <class T, bool ENTIRE_VECTOR>
void RLEScanPartialInternal(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                            idx_t result_offset) {
	D_ASSERT(state.GetPositionInSegment() <= segment.count);
	D_ASSERT(scan_count <= segment.count - state.GetPositionInSegment());
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();

	// If we are scanning an entire Vector and it contains only a single run
	auto current_run = scan_state.ValidateAndGetCurrentRun();
	if (scan_state.position_in_entry < current_run.length &&
	    CanEmitConstantVector<ENTIRE_VECTOR>(scan_state.position_in_entry, current_run.length, scan_count)) {
		RLEScanConstant<T>(scan_state, current_run, scan_count, result);
		return;
	}

	auto result_data = FlatVector::GetDataMutable<T>(result);

	const idx_t result_end = result_offset + scan_count;
	while (result_offset < result_end) {
		auto run = scan_state.ValidateAndGetCurrentRun();
		auto run_count = run.length - scan_state.position_in_entry;
		idx_t remaining_scan_count = result_end - result_offset;
		T element = run.value;
		if (DUCKDB_UNLIKELY(run_count > remaining_scan_count)) {
			for (idx_t i = 0; i < remaining_scan_count; i++) {
				result_data[result_offset + i] = element;
			}
			scan_state.position_in_entry += remaining_scan_count;
			break;
		}

		for (idx_t i = 0; i < run_count; i++) {
			result_data[result_offset + i] = element;
		}

		result_offset += run_count;
		scan_state.ForwardToNextRun();
	}
}

template <class T>
void RLEScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                    idx_t result_offset) {
	return RLEScanPartialInternal<T, false>(segment, state, scan_count, result, result_offset);
}

template <class T>
void RLEScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	RLEScanPartialInternal<T, true>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
template <class T>
void RLESelect(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
               const SelectionVector &sel, idx_t sel_count, idx_t result_offset, ScanVectorType scan_type) {
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();

	// If we are scanning an entire Vector and it contains only a single run we don't need to select at all
	auto current_run = scan_state.ValidateAndGetCurrentRun();
	if (scan_type == ScanVectorType::SCAN_ENTIRE_VECTOR && scan_state.position_in_entry < current_run.length &&
	    CanEmitConstantVector<true>(scan_state.position_in_entry, current_run.length, vector_count)) {
		RLEScanConstant<T>(scan_state, current_run, vector_count, result);
		return;
	}

	auto result_data = FlatVector::Writer<T>(result, sel_count, result_offset);

	// Keep the current run until the selection advances past it.
	idx_t run_end = current_run.length - scan_state.position_in_entry;
	idx_t prev_idx = 0;
	for (idx_t i = 0; i < sel_count; i++) {
		auto next_idx = sel.get_index(i);
		if (next_idx < prev_idx) {
			InternalException::Throw("Error in RLESelect - selection vector indices are not ordered");
		}
		D_ASSERT(next_idx < vector_count);
		while (next_idx >= run_end) {
			scan_state.ForwardToNextRun();
			current_run = scan_state.ValidateAndGetCurrentRun();
			run_end += current_run.length;
		}
		result_data.WriteValue(current_run.value);
		prev_idx = next_idx;
	}
	// Restore the position at the last selected row before skipping the tail.
	scan_state.position_in_entry = current_run.length - (run_end - prev_idx);
	scan_state.SkipInternal(vector_count - prev_idx);
}

//===--------------------------------------------------------------------===//
// Filter
//===--------------------------------------------------------------------===//
template <class T>
void RLEFilter(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result, SelectionVector &sel,
               idx_t &sel_count, const TableFilter &filter, TableFilterState &filter_state) {
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();

	if (!scan_state.matching_runs) {
		// we haven't applied the filter yet
		// apply the filter to all RLE values at once
		auto total_run_count = scan_state.layout.ValidateAllRuns();
		auto run_values = scan_state.layout.GetRunValues(total_run_count);
		auto data_pointer = const_cast<T *>(run_values.data());

		// initialize the filter set to all false (all runs are filtered out)
		scan_state.matching_runs = make_unsafe_uniq_array<bool>(total_run_count);
		memset(scan_state.matching_runs.get(), 0, sizeof(bool) * total_run_count);

		// execute the filter over all runs at once
		Vector run_vector(result.GetType(), data_ptr_cast(data_pointer), total_run_count);

		SelectionVector run_matches;
		scan_state.matching_run_count = total_run_count;
		ColumnSegment::FilterSelection(run_matches, run_vector, filter_state, total_run_count,
		                               scan_state.matching_run_count);

		// for any runs that pass the filter - set the matches to true
		for (idx_t i = 0; i < scan_state.matching_run_count; i++) {
			auto idx = run_matches.get_index(i);
			scan_state.matching_runs[idx] = true;
		}
	}
	if (scan_state.matching_run_count == 0) {
		// early-out, no runs match the filter so the filter can never pass
		sel_count = 0;
		return;
	}
	// scan (the subset of) the matching runs AND set the output selection vector with the rows that match
	auto result_data = FlatVector::GetDataMutable<T>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	idx_t matching_count = 0;
	SelectionVector matching_sel(sel_count);
	if (!sel.IsSet()) {
		// no selection vector yet - fast path
		// this is essentially the normal scan, but we apply the filter and fill the selection vector
		idx_t result_offset = 0;
		idx_t result_end = sel_count;
		while (result_offset < result_end) {
			auto run = scan_state.ValidateAndGetCurrentRun();
			auto run_count = run.length - scan_state.position_in_entry;
			idx_t remaining_scan_count = result_end - result_offset;
			// the run is scanned - scan it
			T element = run.value;
			if (DUCKDB_UNLIKELY(run_count > remaining_scan_count)) {
				if (scan_state.matching_runs[scan_state.entry_pos]) {
					for (idx_t i = 0; i < remaining_scan_count; i++) {
						result_data[result_offset + i] = element;
						matching_sel.set_index(matching_count++, result_offset + i);
					}
				}
				scan_state.position_in_entry += remaining_scan_count;
				break;
			}

			if (scan_state.matching_runs[scan_state.entry_pos]) {
				for (idx_t i = 0; i < run_count; i++) {
					result_data[result_offset + i] = element;
					matching_sel.set_index(matching_count++, result_offset + i);
				}
			}

			result_offset += run_count;
			scan_state.ForwardToNextRun();
		}
	} else {
		// we already have a selection applied - this is more complex since we need to merge it with our filter
		// use a simpler (but slower) approach
		idx_t prev_idx = 0;
		for (idx_t i = 0; i < sel_count; i++) {
			auto read_idx = sel.get_index(i);
			if (read_idx < prev_idx) {
				InternalException::Throw("Error in RLEFilter - selection vector indices are not ordered");
			}
			D_ASSERT(read_idx < vector_count);
			// skip forward to the next index
			scan_state.SkipInternal(read_idx - prev_idx);
			prev_idx = read_idx;
			auto run = scan_state.ValidateAndGetCurrentRun();
			if (!scan_state.matching_runs[scan_state.entry_pos]) {
				// this run is filtered out - we don't need to scan it
				continue;
			}
			// the run is not filtered out - read the element
			result_data[read_idx] = run.value;
			matching_sel.set_index(matching_count++, read_idx);
		}
		// skip the tail
		scan_state.SkipInternal(vector_count - prev_idx);
	}

	// set up the filter result
	if (matching_count != sel_count) {
		sel.Initialize(matching_sel);
		sel_count = matching_count;
	}
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
template <class T, bool WRITE_STATISTICS = true>
CompressionFunction GetRLEFunction(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_RLE, data_type, RLEInitAnalyze<T>, RLEAnalyze<T>,
	                           RLEFinalAnalyze<T>, RLEInitCompression<T, WRITE_STATISTICS>,
	                           RLECompress<T, WRITE_STATISTICS>, RLEFinalizeCompress<T, WRITE_STATISTICS>,
	                           RLEInitScan<T>, RLEScan<T>, RLEScanPartial<T>, RLESkip<T>, nullptr, nullptr, nullptr,
	                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, RLESelect<T>, RLEFilter<T>);
}

CompressionFunction RLEFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL: {
		auto function = GetRLEFunction<int8_t>(type);
		function.filter = nullptr;
		return function;
	}
	case PhysicalType::INT8:
		return GetRLEFunction<int8_t>(type);
	case PhysicalType::INT16:
		return GetRLEFunction<int16_t>(type);
	case PhysicalType::INT32:
		return GetRLEFunction<int32_t>(type);
	case PhysicalType::INT64:
		return GetRLEFunction<int64_t>(type);
	case PhysicalType::INT128:
		return GetRLEFunction<hugeint_t>(type);
	case PhysicalType::UINT128:
		return GetRLEFunction<uhugeint_t>(type);
	case PhysicalType::UINT8:
		return GetRLEFunction<uint8_t>(type);
	case PhysicalType::UINT16:
		return GetRLEFunction<uint16_t>(type);
	case PhysicalType::UINT32:
		return GetRLEFunction<uint32_t>(type);
	case PhysicalType::UINT64:
		return GetRLEFunction<uint64_t>(type);
	case PhysicalType::FLOAT:
		return GetRLEFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetRLEFunction<double>(type);
	case PhysicalType::LIST:
		return GetRLEFunction<uint64_t, false>(type);
	default:
		throw InternalException("Unsupported type for RLE");
	}
}

bool RLEFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
	case PhysicalType::LIST:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
