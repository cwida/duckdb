#include "duckdb/storage/compression/roaring/roaring.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"
#include "duckdb/common/bitpacking.hpp"

/*
Data layout per segment:

                Offsets
+--------------------------------------+
|   +------------------------------+   |
|   |   uint64_t metadata_offset   |   |
|   +------------------------------+   |
+--------------------------------------+

                [Container Data]+
+------------------------------------------------------+
|              Uncompressed Array Container            |
|   +----------------------------------------------+   |
|   |   uint16_t values[]                          |   |
|   +----------------------------------------------+   |
|                                                      |
|               Compressed Array Container             |
|   +----------------------------------------------+   |
|   |   uint8_t counts[COMPRESSED_SEGMENT_COUNT]   |   |
|   |   uint8_t values[]                           |   |
|   +----------------------------------------------+   |
+------------------------------------------------------+
|                    Bitset Container                  |
|   +----------------------------------------------+   |
|   |   uint32_t page_offset[]                     |   |
|   |   uint64_t uncompressed_size[]               |   |
|   |   uint64_t compressed_size[]                 |   |
|   +----------------------------------------------+   |
|                                                      |
+------------------------------------------------------+
|               Uncompressed Run Container             |
|   +----------------------------------------------+   |
|   |   (uint16_t, uint16_t) runs[]                |   |
|   +----------------------------------------------+   |
|                                                      |
|                Compressed Run Container              |
|   +----------------------------------------------+   |
|   |   uint8_t counts[COMPRESSED_SEGMENT_COUNT]   |   |
|   |   (uint8_t, uint8_t) runs[]                  |   |
|   +----------------------------------------------+   |
+------------------------------------------------------+

              Container Metadata
+--------------------------------------------+
|             Container Types                |
|   +------------------------------------+   |
|   |   uint8_t:1 is_run                 |   |
|   |   uint8_t:1 is_inverted            |   |
|   +------------------------------------+   |
|                                            |
|            Run Container Sizes             |
|   +------------------------------------+   |
|   |   uint8_t:7 size                   |   |
|   +------------------------------------+   |
|                                            |
|        Array/Bitset Container Sizes        |
|   +------------------------------------+   |
|   |   uint8_t:8 size                   |   |
|   +------------------------------------+   |
+--------------------------------------------+
*/

namespace duckdb {

namespace roaring {

// Set all the bits from start (inclusive) to end (exclusive) to 0
void SetInvalidRange(ValidityMask &result, idx_t start, idx_t end) {
	if (end <= start) {
		throw InternalException("SetInvalidRange called with end (%d) <= start (%d)", end, start);
	}
	D_ASSERT(result.Capacity() >= end);
	result.EnsureWritable();
	auto result_data = unsafe_array_ptr<validity_t>(result.GetData(), ValidityMask::EntryCount(result.Capacity()));

#ifdef DEBUG
	ValidityMask copy_for_verification(result.Capacity());
	copy_for_verification.EnsureWritable();
	auto copy_data = unsafe_array_ptr<validity_t>(copy_for_verification.GetData(), result_data.size());
	for (idx_t i = 0; i < result_data.size(); i++) {
		copy_data[i] = result_data[i];
	}
#endif
	idx_t index = start;

	if ((index % ValidityMask::BITS_PER_VALUE) != 0) {
		// Adjust the high bits of the first entry

		// +======================================+
		// |xxxxxxxxxxxxxxxxxxxxxxxxx|            |
		// +======================================+
		//
		// 'x': bits to set to 0 in the result

		idx_t right_bits = index % ValidityMask::BITS_PER_VALUE;
		idx_t bits_to_set = ValidityMask::BITS_PER_VALUE - right_bits;
		idx_t left_bits = 0;
		if (index + bits_to_set > end) {
			// Limit the amount of bits to set
			left_bits = (index + bits_to_set) - end;
			bits_to_set = end - index;
		}

		// Prepare the mask
		validity_t mask = ValidityUncompressed::LOWER_MASKS[right_bits];
		if (left_bits) {
			// Mask off the part that we don't want to touch (if the range doesn't fully cover the bits)
			mask |= ValidityUncompressed::UPPER_MASKS[left_bits];
		}

		idx_t entry_idx = index / ValidityMask::BITS_PER_VALUE;
		index += bits_to_set;
		result_data[entry_idx] &= mask;
	}

	idx_t remaining_bits = end - index;
	idx_t full_entries = remaining_bits / ValidityMask::BITS_PER_VALUE;
	idx_t entry_idx = index / ValidityMask::BITS_PER_VALUE;
	// Set all the entries that are fully covered by the range to 0
	for (idx_t i = 0; i < full_entries; i++) {
		result_data[entry_idx + i] = (validity_t)0;
	}

	if ((remaining_bits % ValidityMask::BITS_PER_VALUE) != 0) {
		// The last entry touched by the range is only partially covered

		// +======================================+
		// |                         |xxxxxxxxxxxx|
		// +======================================+
		//
		// 'x': bits to set to 0 in the result

		idx_t bits_to_set = end % ValidityMask::BITS_PER_VALUE;
		idx_t left_bits = ValidityMask::BITS_PER_VALUE - bits_to_set;
		validity_t mask = ValidityUncompressed::UPPER_MASKS[left_bits];
		idx_t entry_idx = end / ValidityMask::BITS_PER_VALUE;
		result_data[entry_idx] &= mask;
	}

#ifdef DEBUG
	D_ASSERT(end <= result.Capacity());
	for (idx_t i = 0; i < result.Capacity(); i++) {
		if (i >= start && i < end) {
			D_ASSERT(!result.RowIsValidUnsafe(i));
		} else {
			// Ensure no others bits are touched by this method
			D_ASSERT(copy_for_verification.RowIsValidUnsafe(i) == result.RowIsValidUnsafe(i));
		}
	}
#endif
}

unique_ptr<AnalyzeState> RoaringInitAnalyze(ColumnData &col_data, PhysicalType type) {
	// check if the storage version we are writing to supports roaring
	const auto storage_version = col_data.GetStorageManager().GetStorageVersion();
	// before: serialization version 4 and ser version 7
	if (StorageManager::IsPriorToVersion(StorageVersion::V1_2_0, storage_version) ||
	    (type == PhysicalType::BOOL && StorageManager::IsPriorToVersion(StorageVersion::V1_5_0, storage_version))) {
		// compatibility mode with old versions - disable roaring
		return nullptr;
	}
	auto state = make_uniq<RoaringAnalyzeState>(col_data.GetBlockManager());
	return std::move(state);
}
template <PhysicalType TYPE>
bool RoaringAnalyze(AnalyzeState &state, const Vector &input) {
	auto &analyze_state = state.Cast<RoaringAnalyzeState>();
	analyze_state.Analyze<TYPE>(input);
	return true;
}

idx_t RoaringFinalAnalyze(AnalyzeState &state) {
	auto &roaring_state = state.Cast<RoaringAnalyzeState>();
	roaring_state.FlushContainer();
	roaring_state.FlushSegment();

	constexpr const double ROARING_COMPRESS_PENALTY = 2.0;
	return LossyNumericCast<idx_t>((double)roaring_state.total_size * ROARING_COMPRESS_PENALTY);
}

unique_ptr<CompressionState> RoaringInitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                    unique_ptr<AnalyzeState> state) {
	return make_uniq<RoaringCompressState>(checkpoint_data, std::move(state));
}

template <PhysicalType TYPE>
void RoaringCompress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<RoaringCompressState>();
	state.Compress<TYPE>(scan_vector);
}

void RoaringFinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<RoaringCompressState>();
	state.Finalize();
}

unique_ptr<SegmentScanState> RoaringInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto result = make_uniq<RoaringScanState>(context, segment);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
void ExtractValidityMaskToData(const ValidityMask &validity, Vector &dst, idx_t offset, idx_t scan_count) {
	auto write_ptr = FlatVector::GetDataMutable<uint8_t>(dst) + offset;
	if (validity.CannotHaveNull()) {
		memset(write_ptr, 1, scan_count); // 1 is for valid
	} else if (scan_count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0) {
		// "Bit-Unpack" src's validity_mask and put it in dst's data
		BitpackingPrimitives::UnPackBuffer<uint8_t>(write_ptr, data_ptr_cast(validity.GetData()), scan_count, 1);
	} else {
		// Because UnPackBuffer writes in batches of BITPACKING_ALGORITHM_GROUP_SIZE, we create a tmp_buffer first to
		// prevent overflow in the case dst is smaller than the batch.
		auto tmp_data = make_uniq_array<uint8_t>(
		    AlignValue<idx_t, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE>(scan_count));
		BitpackingPrimitives::UnPackBuffer<uint8_t>(tmp_data.get(), data_ptr_cast(validity.GetData()), scan_count, 1);
		memcpy(write_ptr, tmp_data.get(), scan_count);
	}
}

void RoaringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                        idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<RoaringScanState>();
	auto start = state.GetPositionInSegment();
	auto &result_mask = FlatVector::ValidityMutable(result);
	scan_state.ScanPartial(start, result_mask, result_offset, scan_count);
}
void RoaringScanPartialBoolean(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                               idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<RoaringScanState>();
	auto start = state.GetPositionInSegment();

	ValidityMask mask(scan_count);
	scan_state.ScanPartial(start, mask, 0, scan_count);
	ExtractValidityMaskToData(mask, result, result_offset, scan_count);
}
void RoaringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	result.Flatten();
	RoaringScanPartial(segment, state, scan_count, result, 0);
}

void RoaringScanBoolean(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	auto &scan_state = state.scan_state->Cast<RoaringScanState>();
	auto start = state.GetPositionInSegment();

	ValidityMask mask(scan_count);
	scan_state.ScanPartial(start, mask, 0, scan_count);
	ExtractValidityMaskToData(mask, result, 0, scan_count);
}

//===--------------------------------------------------------------------===//
// Skip
//===--------------------------------------------------------------------===//
void RoaringSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	// NO OP
	// We skip inside scan instead, if the container boundary gets crossed we can avoid a bunch of work anyways
	return;
}

unique_ptr<CompressedSegmentState> RoaringInitSegment(ColumnSegment &segment, block_id_t block_id,
                                                      optional_ptr<ColumnSegmentState> segment_state) {
	// 'ValidityInitSegment' is used normally, which memsets the page to all bits set.
	return nullptr;
}

} // namespace roaring

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction GetCompressionFunction(PhysicalType data_type) {
	compression_analyze_t analyze = nullptr;
	compression_compress_data_t compress = nullptr;
	compression_scan_vector_t scan = nullptr;
	compression_scan_partial_t scan_partial = nullptr;

	switch (data_type) {
	case PhysicalType::BIT: {
		analyze = roaring::RoaringAnalyze<PhysicalType::BIT>;
		compress = roaring::RoaringCompress<PhysicalType::BIT>;
		scan = roaring::RoaringScan;
		scan_partial = roaring::RoaringScanPartial;
		break;
	}
	case PhysicalType::BOOL: {
		analyze = roaring::RoaringAnalyze<PhysicalType::BOOL>;
		compress = roaring::RoaringCompress<PhysicalType::BOOL>;
		scan = roaring::RoaringScanBoolean;
		scan_partial = roaring::RoaringScanPartialBoolean;
		break;
	}
	default:
		throw InternalException("Roaring GetCompressionFunction, type %s not handled", EnumUtil::ToString(data_type));
	}
	return CompressionFunction(CompressionType::COMPRESSION_ROARING, data_type, roaring::RoaringInitAnalyze, analyze,
	                           roaring::RoaringFinalAnalyze, roaring::RoaringInitCompression, compress,
	                           roaring::RoaringFinalizeCompress, roaring::RoaringInitScan, scan, scan_partial,
	                           roaring::RoaringSkip, roaring::RoaringInitSegment);
}

CompressionFunction RoaringCompressionFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
	case PhysicalType::BOOL:
		return GetCompressionFunction(type);
	default:
		throw InternalException("Unsupported type for Roaring");
	}
}

bool RoaringCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::BIT:
	case PhysicalType::BOOL:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
