#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Mask constants
//===--------------------------------------------------------------------===//
// LOWER_MASKS contains masks with all the lower bits set until a specific value
// LOWER_MASKS[0] has the 0 lowest bits set, i.e.:
// 0b0000000000000000000000000000000000000000000000000000000000000000,
// LOWER_MASKS[10] has the 10 lowest bits set, i.e.:
// 0b0000000000000000000000000000000000000000000000000000000111111111,
// etc...
// 0b0000000000000000000000000000000000000001111111111111111111111111,
// ...
// 0b0000000000000000000001111111111111111111111111111111111111111111,
// until LOWER_MASKS[64], which has all bits set:
// 0b1111111111111111111111111111111111111111111111111111111111111111
// generated with this python snippet:
// for i in range(65):
//   print(hex(int((64 - i) * '0' + i * '1', 2)) + ",")
const validity_t ValidityUncompressed::LOWER_MASKS[] = {0x0,
                                                        0x1,
                                                        0x3,
                                                        0x7,
                                                        0xf,
                                                        0x1f,
                                                        0x3f,
                                                        0x7f,
                                                        0xff,
                                                        0x1ff,
                                                        0x3ff,
                                                        0x7ff,
                                                        0xfff,
                                                        0x1fff,
                                                        0x3fff,
                                                        0x7fff,
                                                        0xffff,
                                                        0x1ffff,
                                                        0x3ffff,
                                                        0x7ffff,
                                                        0xfffff,
                                                        0x1fffff,
                                                        0x3fffff,
                                                        0x7fffff,
                                                        0xffffff,
                                                        0x1ffffff,
                                                        0x3ffffff,
                                                        0x7ffffff,
                                                        0xfffffff,
                                                        0x1fffffff,
                                                        0x3fffffff,
                                                        0x7fffffff,
                                                        0xffffffff,
                                                        0x1ffffffff,
                                                        0x3ffffffff,
                                                        0x7ffffffff,
                                                        0xfffffffff,
                                                        0x1fffffffff,
                                                        0x3fffffffff,
                                                        0x7fffffffff,
                                                        0xffffffffff,
                                                        0x1ffffffffff,
                                                        0x3ffffffffff,
                                                        0x7ffffffffff,
                                                        0xfffffffffff,
                                                        0x1fffffffffff,
                                                        0x3fffffffffff,
                                                        0x7fffffffffff,
                                                        0xffffffffffff,
                                                        0x1ffffffffffff,
                                                        0x3ffffffffffff,
                                                        0x7ffffffffffff,
                                                        0xfffffffffffff,
                                                        0x1fffffffffffff,
                                                        0x3fffffffffffff,
                                                        0x7fffffffffffff,
                                                        0xffffffffffffff,
                                                        0x1ffffffffffffff,
                                                        0x3ffffffffffffff,
                                                        0x7ffffffffffffff,
                                                        0xfffffffffffffff,
                                                        0x1fffffffffffffff,
                                                        0x3fffffffffffffff,
                                                        0x7fffffffffffffff,
                                                        0xffffffffffffffff};

// UPPER_MASKS contains masks with all the highest bits set until a specific value
// UPPER_MASKS[0] has the 0 highest bits set, i.e.:
// 0b0000000000000000000000000000000000000000000000000000000000000000,
// UPPER_MASKS[10] has the 10 highest bits set, i.e.:
// 0b1111111111110000000000000000000000000000000000000000000000000000,
// etc...
// 0b1111111111111111111111110000000000000000000000000000000000000000,
// ...
// 0b1111111111111111111111111111111111111110000000000000000000000000,
// until UPPER_MASKS[64], which has all bits set:
// 0b1111111111111111111111111111111111111111111111111111111111111111
// generated with this python snippet:
// for i in range(65):
//   print(hex(int(i * '1' + (64 - i) * '0', 2)) + ",")
const validity_t ValidityUncompressed::UPPER_MASKS[] = {0x0,
                                                        0x8000000000000000,
                                                        0xc000000000000000,
                                                        0xe000000000000000,
                                                        0xf000000000000000,
                                                        0xf800000000000000,
                                                        0xfc00000000000000,
                                                        0xfe00000000000000,
                                                        0xff00000000000000,
                                                        0xff80000000000000,
                                                        0xffc0000000000000,
                                                        0xffe0000000000000,
                                                        0xfff0000000000000,
                                                        0xfff8000000000000,
                                                        0xfffc000000000000,
                                                        0xfffe000000000000,
                                                        0xffff000000000000,
                                                        0xffff800000000000,
                                                        0xffffc00000000000,
                                                        0xffffe00000000000,
                                                        0xfffff00000000000,
                                                        0xfffff80000000000,
                                                        0xfffffc0000000000,
                                                        0xfffffe0000000000,
                                                        0xffffff0000000000,
                                                        0xffffff8000000000,
                                                        0xffffffc000000000,
                                                        0xffffffe000000000,
                                                        0xfffffff000000000,
                                                        0xfffffff800000000,
                                                        0xfffffffc00000000,
                                                        0xfffffffe00000000,
                                                        0xffffffff00000000,
                                                        0xffffffff80000000,
                                                        0xffffffffc0000000,
                                                        0xffffffffe0000000,
                                                        0xfffffffff0000000,
                                                        0xfffffffff8000000,
                                                        0xfffffffffc000000,
                                                        0xfffffffffe000000,
                                                        0xffffffffff000000,
                                                        0xffffffffff800000,
                                                        0xffffffffffc00000,
                                                        0xffffffffffe00000,
                                                        0xfffffffffff00000,
                                                        0xfffffffffff80000,
                                                        0xfffffffffffc0000,
                                                        0xfffffffffffe0000,
                                                        0xffffffffffff0000,
                                                        0xffffffffffff8000,
                                                        0xffffffffffffc000,
                                                        0xffffffffffffe000,
                                                        0xfffffffffffff000,
                                                        0xfffffffffffff800,
                                                        0xfffffffffffffc00,
                                                        0xfffffffffffffe00,
                                                        0xffffffffffffff00,
                                                        0xffffffffffffff80,
                                                        0xffffffffffffffc0,
                                                        0xffffffffffffffe0,
                                                        0xfffffffffffffff0,
                                                        0xfffffffffffffff8,
                                                        0xfffffffffffffffc,
                                                        0xfffffffffffffffe,
                                                        0xffffffffffffffff};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct ValidityAnalyzeState : public AnalyzeState {
	explicit ValidityAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager), count(0) {
	}

	idx_t count;
};

unique_ptr<AnalyzeState> ValidityInitAnalyze(ColumnData &col_data, PhysicalType type) {
	return make_uniq<ValidityAnalyzeState>(col_data.GetBlockManager());
}

bool ValidityAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<ValidityAnalyzeState>();
	state.count += input.size();
	return true;
}

idx_t ValidityFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.Cast<ValidityAnalyzeState>();
	return (state.count + 7) / 8;
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
struct ValidityScanState : public SegmentScanState {
	explicit ValidityScanState(BufferHandle handle_p, ColumnSegment &segment)
	    : handle(std::move(handle_p)),
	      reader(CompressionSegmentReader::FromSegment(handle, segment, "validity segment")),
	      block_id(segment.GetBlockHandle()->BlockId()) {
	}

	BufferHandle handle;
	CompressionSegmentReader reader;
	block_id_t block_id;
};

static idx_t ValidityEntryCount(idx_t count) {
	return count / ValidityMask::BITS_PER_VALUE + (count % ValidityMask::BITS_PER_VALUE != 0);
}

static unsafe_array_ptr<const validity_t> ValidityScanData(const CompressionSegmentReader &reader, idx_t start,
                                                           idx_t scan_count) {
	D_ASSERT(scan_count > 0);
	auto entry_start = start / ValidityMask::BITS_PER_VALUE;
	auto entry_end = (start + scan_count - 1) / ValidityMask::BITS_PER_VALUE;
	return reader.GetArraySlice<validity_t>(0, entry_start, entry_end - entry_start + 1);
}

unique_ptr<SegmentScanState> ValidityInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	return make_uniq<ValidityScanState>(std::move(handle), segment);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//

void ValidityUncompressed::UnalignedScan(unsafe_array_ptr<const validity_t> input, idx_t input_size, idx_t input_start,
                                         ValidityMask &result_mask, idx_t result_offset, idx_t scan_count) {
	D_ASSERT(input_start < input_size);
	D_ASSERT(scan_count <= input_size - input_start);
	D_ASSERT(ValidityEntryCount(input_size) <= input.size());

#ifdef DEBUG
	// save boundary entries to verify we don't corrupt surrounding bits later.
	idx_t debug_first_entry = result_offset / ValidityMask::BITS_PER_VALUE;
	idx_t debug_last_entry = (result_offset + scan_count - 1) / ValidityMask::BITS_PER_VALUE;
	auto debug_result_data = (validity_t *)result_mask.GetData();
	validity_t debug_original_first_entry =
	    debug_result_data ? debug_result_data[debug_first_entry] : ValidityMask::ValidityBuffer::MAX_ENTRY;
	validity_t debug_original_last_entry =
	    debug_result_data ? debug_result_data[debug_last_entry] : ValidityMask::ValidityBuffer::MAX_ENTRY;

	// save original result validity for in-range verification (usually this function is meant to be called
	// with all result bits set to valid, but in some instances, the result bits may be invalid, in which case,
	// the result bits should remain invalid, i.e. we do not copy over the input bit.
	ValidityMask debug_original_result(scan_count);
	if (debug_result_data) {
		for (idx_t i = 0; i < scan_count; i++) {
			if (!result_mask.RowIsValid(result_offset + i)) {
				debug_original_result.SetInvalid(i);
			}
		}
	}
#endif

#if STANDARD_VECTOR_SIZE < 128
	// fallback for tiny vector sizes
	// the bitwise ops we use below don't work if the vector size is too small
	for (idx_t i = 0; i < scan_count; i++) {
		idx_t source_idx = input_start + i;
		if (!ValidityMask::RowIsValid(input[source_idx / ValidityMask::BITS_PER_VALUE],
		                              source_idx % ValidityMask::BITS_PER_VALUE)) {
			if (result_mask.CannotHaveNull()) {
				result_mask.Initialize();
			}
			result_mask.SetInvalid(result_offset + i);
		}
	}
#else
	// the code below does what the fallback code above states, but using bitwise ops:
	auto result_data = (validity_t *)result_mask.GetData();

	// set up the initial positions
	// we need to find the validity_entry to modify, together with the bit-index WITHIN the validity entry
	idx_t result_entry = result_offset / ValidityMask::BITS_PER_VALUE;
	idx_t result_idx = result_offset - result_entry * ValidityMask::BITS_PER_VALUE;

	// same for the input: find the validity_entry we are pulling from, together with the bit-index WITHIN that entry
	idx_t input_entry = input_start / ValidityMask::BITS_PER_VALUE;
	idx_t input_idx = input_start - input_entry * ValidityMask::BITS_PER_VALUE;

	// Window scanning algorithm -- the goal is to copy a contiguous sequence of bits from input into result,
	// and to do this using bit operations on 64 bit fields.
	//
	// The algorithm is simply explained with the diagram below: each loop iteration is numbered, and within each
	// iteration we are copying the numbered window from the input to the corresponding window in the result.
	//
	// input_entry and result_entry are the 64 bit entries, i.e. on any given loop iteration we only want to be
	// performing bit operations on 64 bit entries.
	//
	// input_index and result_index are the current offset within each entry. We can calculate the current window size
	// as the minimum between the remaining bits to process in each entry.
	//
	// INPUT:
	//  0                             63|                              127|                            191
	//  +-------------------------------+--------------------------------+--------------------------------+
	//  |                      [   1   ]|[          2         ][   3    ]|[          4         ][   5   ]|
	//  +-------------------------------+--------------------------------+--------------------------------+
	//
	//  RESULT:
	//  0                             63|                             127|                              191
	//  +-------------------------------+--------------------------------+--------------------------------+
	//   [   1   ][          2         ]|[   3    ][          4         ]|[   5    ]                      |
	//  +-------------------------------+--------------------------------+--------------------------------+
	//
	// Note: in case this ever becomes a bottleneck, it should be possible to make each loop iteration branchless.
	// The idea would be to do an odd iteration before the loop, then have two loops depending on the layout of the
	// windows that will either shift left then right on each iteration, or the other loop will always shift right
	// then left. For example, in the diagram above, we would first apply the first window outside of the loop
	// beforehand, then we can see that each loop iteration requires us to shift right, fetch a new result entry,
	// shift left, fetch a new input entry. This would have to be generalized to two possible branchless loops,
	// depending on the input.

	// now start the bit games
	idx_t pos = 0;
	while (pos < scan_count) {
		validity_t input_mask = input[input_entry];
		idx_t bits_left = scan_count - pos;

		// these are bits left within the current entries (possibly extra than what we need).
		idx_t input_bits_left = ValidityMask::BITS_PER_VALUE - input_idx;
		idx_t result_bits_left = ValidityMask::BITS_PER_VALUE - result_idx;

		// these are the bits left within the current entries that need to be processed.
		idx_t input_window_size = MinValue(bits_left, input_bits_left);
		idx_t result_window_size = MinValue(bits_left, result_bits_left);

		// the smaller of the two is our next window to copy from input to result.
		idx_t window_size = MinValue(input_window_size, result_window_size);

		// Within each loop iteration, copy the window from the starting index in input over to the starting index
		// of result, without corrupting surrounding bits in the result entry.

		// First, line up the windows:
		if (result_idx < input_idx) {
			// X is arbitrary bits, P is arbitrary protected bits.
			// INPUT ENTRY:
			// 63                                                                                                 0
			// +--------------------------------------------------------------------------------------------------+
			// |XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX[=============WINDOW=============]XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX|
			// +--------------------------------------------------------------------------------------------------+
			// 								                                     ^
			// 						                                         input_idx
			//
			// RESULT ENTRY:
			// 63                                                                                                 0
			// +--------------------------------------------------------------------------------------------------+
			// |PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP[=============WINDOW=============]PPPPPPPPPPPPPPPPPPPPPP|
			// +--------------------------------------------------------------------------------------------------+
			// 										                                       ^
			// 								                                           result_idx
			//
			idx_t shift_amount = input_idx - result_idx;
			input_mask = input_mask >> shift_amount;
		} else {
			// current_result_idx >= current_input_idx
			idx_t shift_amount = result_idx - input_idx;
			input_mask = (input_mask & ~UPPER_MASKS[shift_amount]);

			// X is arbitrary bits, P is arbitrary protected bits.
			// Note the zeroed out bits in INPUT_ENTRY - these have to be zeroed before shifting left to align with
			// result window, to prevent overflow.
			//
			// INPUT ENTRY:
			// 63                                                                                                 0
			// +--------------------------------------------------------------------------------------------------+
			// |000000000000XXXXXXXXXXXXXXXXXXXX[=============WINDOW=============]XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX|
			// +--------------------------------------------------------------------------------------------------+
			// 																     ^
			// 											                     input_idx
			//
			// RESULT ENTRY:
			// 63                                                                                                 0
			// +--------------------------------------------------------------------------------------------------+
			// |PPPPPPPPPPPPPPPPPPPP[=============WINDOW=============]PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP|
			// +--------------------------------------------------------------------------------------------------+
			// 													     ^
			// 									                 result_idx
			input_mask = input_mask << shift_amount;
		}

		// Once the windows are aligned, mask the input to prevent overwriting protected bits in the result_mask.
		auto protected_upper_bits = UPPER_MASKS[ValidityMask::BITS_PER_VALUE - result_idx - window_size];
		auto protected_lower_bits = LOWER_MASKS[result_idx];
		input_mask |= protected_upper_bits;
		input_mask |= protected_lower_bits;

		if (input_mask != ValidityMask::ValidityBuffer::MAX_ENTRY) {
			if (!result_data) {
				result_mask.Initialize();
				result_data = (validity_t *)result_mask.GetData();
			}
			result_data[result_entry] &= input_mask;
		}
		// Now update pos, entries, and indexes for the next iteration.
		pos += window_size;

		// Windows can only go until the end of the current entry, so the mod can only wrap to 0 here.
		input_idx = (input_idx + window_size) % ValidityMask::BITS_PER_VALUE;
		result_idx = (result_idx + window_size) % ValidityMask::BITS_PER_VALUE;

		// Advance entries if the mod was 0.
		if (input_idx == 0) {
			input_entry++;
		}
		if (result_idx == 0) {
			result_entry++;
		}
	}
#endif

#ifdef DEBUG
	// verify in-range bits.
	for (idx_t i = 0; i < scan_count; i++) {
		bool original_valid = debug_original_result.RowIsValid(i);
		idx_t source_idx = input_start + i;
		bool input_valid = ValidityMask::RowIsValid(input[source_idx / ValidityMask::BITS_PER_VALUE],
		                                            source_idx % ValidityMask::BITS_PER_VALUE);
		bool result_valid = result_mask.RowIsValid(result_offset + i);
		D_ASSERT(result_valid == (original_valid && input_valid));
	}

	// verify surrounding bits weren't modified
	auto debug_final_result_data = (validity_t *)result_mask.GetData();
	validity_t debug_final_first_entry =
	    debug_final_result_data ? debug_final_result_data[debug_first_entry] : ValidityMask::ValidityBuffer::MAX_ENTRY;
	validity_t debug_final_last_entry =
	    debug_final_result_data ? debug_final_result_data[debug_last_entry] : ValidityMask::ValidityBuffer::MAX_ENTRY;

	idx_t first_bit_in_first_entry = result_offset % ValidityMask::BITS_PER_VALUE;
	idx_t last_bit_in_last_entry = (result_offset + scan_count - 1) % ValidityMask::BITS_PER_VALUE;

	// lower bits of first entry should be unchanged
	validity_t lower_mask = LOWER_MASKS[first_bit_in_first_entry];
	D_ASSERT((debug_original_first_entry & lower_mask) == (debug_final_first_entry & lower_mask));

	// upper bits of last entry should be unchanged
	validity_t upper_mask = UPPER_MASKS[ValidityMask::BITS_PER_VALUE - last_bit_in_last_entry - 1];
	D_ASSERT((debug_original_last_entry & upper_mask) == (debug_final_last_entry & upper_mask));
#endif
}

void ValidityUncompressed::AlignedScan(unsafe_array_ptr<const validity_t> input, idx_t input_start,
                                       ValidityMask &result_mask, idx_t scan_count) {
	D_ASSERT(input_start % ValidityMask::BITS_PER_VALUE == 0);

	// aligned scan: no need to do anything fancy
	// note: this is only an optimization which avoids having to do messy bitshifting in the common case
	// it is not required for correctness
	auto result_data = result_mask.GetData();
	idx_t start_offset = input_start / ValidityMask::BITS_PER_VALUE;
	idx_t entry_scan_count = ValidityEntryCount(scan_count);
	D_ASSERT(start_offset <= input.size());
	D_ASSERT(entry_scan_count <= input.size() - start_offset);
	for (idx_t i = 0; i < entry_scan_count; i++) {
		auto input_entry = input[start_offset + i];
		if (!result_data && input_entry == ValidityMask::ValidityBuffer::MAX_ENTRY) {
			continue;
		}
		if (!result_data) {
			result_mask.Initialize();
			result_data = result_mask.GetData();
		}
		result_data[i] = input_entry;
	}
}

void ValidityScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                         idx_t result_offset) {
	auto start = state.GetPositionInSegment();
	if (scan_count == 0) {
		return;
	}
	D_ASSERT(start <= segment.count && scan_count <= segment.count - start);

	static_assert(sizeof(validity_t) == sizeof(uint64_t), "validity_t should be 64-bit");
	auto &scan_state = state.scan_state->Cast<ValidityScanState>();

	auto input_data = ValidityScanData(scan_state.reader, start, scan_count);
	D_ASSERT(scan_state.block_id == segment.GetBlockHandle()->BlockId());
	auto &result_mask = FlatVector::ValidityMutable(result);
	auto input_start = start % ValidityMask::BITS_PER_VALUE;
	ValidityUncompressed::UnalignedScan(input_data, input_start + scan_count, input_start, result_mask, result_offset,
	                                    scan_count);
}

void ValidityScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	auto start = state.GetPositionInSegment();
	if (scan_count == 0) {
		return;
	}
	D_ASSERT(start <= segment.count && scan_count <= segment.count - start);

	result.Flatten();
	if (start % ValidityMask::BITS_PER_VALUE == 0) {
		auto &scan_state = state.scan_state->Cast<ValidityScanState>();

		auto input_data = ValidityScanData(scan_state.reader, start, scan_count);
		D_ASSERT(scan_state.block_id == segment.GetBlockHandle()->BlockId());
		auto &result_mask = FlatVector::ValidityMutable(result);
		ValidityUncompressed::AlignedScan(input_data, 0, result_mask, scan_count);
	} else {
		// unaligned scan: fall back to scan_partial which does bitshift tricks
		ValidityScanPartial(segment, state, scan_count, result, 0);
	}
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
void ValiditySelect(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                    const SelectionVector &sel, idx_t sel_count, idx_t result_offset, ScanVectorType) {
	auto start = state.GetPositionInSegment();
	if (sel_count == 0) {
		return;
	}
	D_ASSERT(start <= segment.count && vector_count <= segment.count - start);
	D_ASSERT(sel_count <= vector_count);

	if (result_offset == 0) {
		result.Flatten();
	}

	auto &scan_state = state.scan_state->Cast<ValidityScanState>();
	auto input_data = ValidityScanData(scan_state.reader, start, vector_count);
	auto input_start = start % ValidityMask::BITS_PER_VALUE;
	auto &result_mask = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < sel_count; i++) {
		auto selected_index = sel.get_index(i);
		D_ASSERT(selected_index < vector_count);
		auto source_idx = input_start + selected_index;
		auto entry = input_data[source_idx / ValidityMask::BITS_PER_VALUE];
		if (!ValidityMask::RowIsValid(entry, source_idx % ValidityMask::BITS_PER_VALUE)) {
			result_mask.SetInvalid(result_offset + i);
		}
	}
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
static unique_ptr<CompressionAppendState> ValidityInitAppend(ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(segment.GetBlockHandle());
	return make_uniq<CompressionAppendState>(std::move(handle));
}

unique_ptr<CompressedSegmentState> ValidityInitSegment(ColumnSegment &segment, block_id_t block_id,
                                                       optional_ptr<ColumnSegmentState> segment_state) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	if (block_id == INVALID_BLOCK) {
		auto handle = buffer_manager.Pin(segment.GetBlockHandle());
		auto buffer_ptr = handle.GetDataMutable() + segment.GetBlockOffset();
		memset(buffer_ptr, 0xFF, segment.SegmentSize());
	}
	return nullptr;
}

idx_t ValidityAppend(CompressionAppendState &append_state, ColumnSegment &segment, BaseStatistics &validity_stats,
                     UnifiedVectorFormat &data, idx_t offset, idx_t vcount) {
	auto max_tuples = segment.SegmentSize() / ValidityMask::STANDARD_MASK_SIZE * STANDARD_VECTOR_SIZE;
	idx_t append_count = MinValue<idx_t>(vcount, max_tuples - segment.count);
	if (data.validity.CannotHaveNull()) {
		// no null values: skip append
		segment.count += append_count;
		validity_stats.SetHasNoNullFast();
		return append_count;
	}

	ValidityMask mask(reinterpret_cast<validity_t *>(append_state.handle.GetDataMutable() + segment.GetBlockOffset()),
	                  max_tuples);
	for (idx_t i = 0; i < append_count; i++) {
		auto idx = data.sel->get_index(offset + i);
		if (!data.validity.RowIsValidUnsafe(idx)) {
			mask.SetInvalidUnsafe(segment.count + i);
			validity_stats.SetHasNullFast();
		} else {
			validity_stats.SetHasNoNullFast();
		}
	}
	segment.count += append_count;
	return append_count;
}

idx_t ValidityFinalizeAppend(ColumnSegment &segment, BaseStatistics &stats) {
	return ((segment.count + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE) * ValidityMask::STANDARD_MASK_SIZE;
}

void ValidityRevertAppend(ColumnSegment &segment, idx_t new_count) {
	idx_t start_bit = new_count;

	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(segment.GetBlockHandle());
	auto buffer_ptr = handle.GetDataMutable() + segment.GetBlockOffset();
	idx_t revert_start;
	if (start_bit % 8 != 0) {
		// handle sub-bit stuff (yay)
		idx_t byte_pos = start_bit / 8;
		idx_t bit_end = (byte_pos + 1) * 8;
		ValidityMask mask(reinterpret_cast<validity_t *>(buffer_ptr), segment.count);
		for (idx_t i = start_bit; i < bit_end && i < segment.count; i++) {
			mask.SetValid(i);
		}
		revert_start = bit_end / 8;
	} else {
		revert_start = start_bit / 8;
	}
	// for the rest, we just memset
	memset(buffer_ptr + revert_start, 0xFF, segment.SegmentSize() - revert_start);
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction ValidityUncompressed::GetFunction(PhysicalType data_type) {
	D_ASSERT(data_type == PhysicalType::BIT);
	CompressionFunction result(CompressionType::COMPRESSION_UNCOMPRESSED, data_type, ValidityInitAnalyze,
	                           ValidityAnalyze, ValidityFinalAnalyze, UncompressedFunctions::InitCompression,
	                           UncompressedFunctions::Compress, UncompressedFunctions::FinalizeCompress,
	                           ValidityInitScan, ValidityScan, ValidityScanPartial, UncompressedFunctions::EmptySkip,
	                           ValidityInitSegment, ValidityInitAppend, ValidityAppend, ValidityFinalizeAppend,
	                           ValidityRevertAppend);
	result.select = ValiditySelect;
	return result;
}

} // namespace duckdb
