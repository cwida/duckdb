#include "duckdb/common/bitpacking.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/bitpacking.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include <functional>
#include <type_traits>
#include <variant>

namespace duckdb {

constexpr const idx_t BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
static constexpr const idx_t BITPACKING_METADATA_GROUP_SIZE = STANDARD_VECTOR_SIZE > 512 ? STANDARD_VECTOR_SIZE : 2048;
//! Scan tuning limit in values, independent of the on-disk algorithm group size.
static constexpr idx_t BITPACKING_SCAN_BATCH_SIZE = 128;
static_assert(BITPACKING_SCAN_BATCH_SIZE % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0,
              "Bitpacking scan batches must contain complete algorithm groups");

typedef struct {
	BitpackingMode mode;
	uint32_t offset;
} bitpacking_metadata_t;

typedef uint32_t bitpacking_metadata_encoded_t;

static bitpacking_metadata_encoded_t EncodeMeta(bitpacking_metadata_t metadata) {
	D_ASSERT(metadata.offset <= 0x00FFFFFF); // max uint24_t
	bitpacking_metadata_encoded_t encoded_value = metadata.offset;
	encoded_value |= UnsafeNumericCast<bitpacking_metadata_encoded_t>((uint8_t)metadata.mode << 24);
	return encoded_value;
}

static bitpacking_metadata_t DecodeMeta(bitpacking_metadata_encoded_t metadata_encoded) {
	bitpacking_metadata_t metadata;
	metadata.mode = static_cast<BitpackingMode>((metadata_encoded >> 24) & 0xFF);
	metadata.offset = metadata_encoded & 0x00FFFFFF;
	return metadata;
}

[[noreturn]] static void ThrowBitpackingUnknownMode() {
	throw DataCorruptionException("Corrupted bitpacking segment: unknown bitpacking mode");
}

static void ValidateBitpackingMode(BitpackingMode mode) {
	switch (mode) {
	case BitpackingMode::CONSTANT:
	case BitpackingMode::CONSTANT_DELTA:
	case BitpackingMode::FOR:
	case BitpackingMode::DELTA_FOR:
		return;
	default:
		ThrowBitpackingUnknownMode();
	}
}

[[noreturn]] static void ThrowBitpackingWidthOutOfRange() {
	throw DataCorruptionException("Corrupted bitpacking segment: bit width exceeds the physical type width");
}

[[noreturn]] static void ThrowBitpackingGroupOffsetsInvalid() {
	throw DataCorruptionException(
	    "Corrupted bitpacking segment: group offsets do not describe a range in the data region");
}

[[noreturn]] static void ThrowBitpackingGroupIndexOutOfRange() {
	throw DataCorruptionException("Corrupted bitpacking segment: group index exceeds the metadata table");
}

[[noreturn]] static void ThrowBitpackingReadPastEnd() {
	throw DataCorruptionException("Corrupted bitpacking segment: read exceeds the segment row count");
}

[[noreturn]] static void ThrowBitpackingInvalidBoolean() {
	throw DataCorruptionException("Corrupted bitpacking segment: BOOL group contains values other than 0 and 1");
}

//! BOOL is bitpacked as int8_t - verify from the group header that the group can only decode to 0 and 1. The values of
//! DELTA_FOR groups also depend on the deltas, so these are verified after decoding (VerifyBooleanValues).
template <class T>
static void ValidateBooleanGroup(BitpackingMode mode, bitpacking_width_t width, T frame_of_reference, T &constant,
                                 idx_t count) {
	auto first = static_cast<int64_t>(frame_of_reference);
	int64_t last;
	switch (mode) {
	case BitpackingMode::CONSTANT:
		if (constant == NumericLimits<T>::Minimum()) {
			// groups that only contain NULLs store the initial maximum as their constant
			constant = 0;
		}
		first = static_cast<int64_t>(constant);
		last = first;
		break;
	case BitpackingMode::FOR:
		if (width > 1) {
			ThrowBitpackingInvalidBoolean();
		}
		last = first + width;
		break;
	case BitpackingMode::CONSTANT_DELTA:
		last = first + static_cast<int64_t>(count - 1) * static_cast<int64_t>(constant);
		break;
	case BitpackingMode::DELTA_FOR:
		// the deltas between 0 and 1 values are in [-1, 1]
		if (width > 2 || first < -1 || first + (int64_t(1) << width) - 1 > 1) {
			ThrowBitpackingInvalidBoolean();
		}
		return;
	default:
		ThrowBitpackingInvalidBoolean();
	}
	if (first < 0 || first > 1 || last < 0 || last > 1) {
		ThrowBitpackingInvalidBoolean();
	}
}

template <class T>
static void VerifyBooleanValues(const T *values, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		if (values[i] != 0 && values[i] != 1) {
			ThrowBitpackingInvalidBoolean();
		}
	}
}

template <class T, class T_U = typename MakeUnsigned<T>::type>
static bitpacking_width_t ValidateBitpackingWidth(T stored_width) {
	if (static_cast<T_U>(stored_width) > sizeof(T) * 8) {
		ThrowBitpackingWidthOutOfRange();
	}
	return static_cast<bitpacking_width_t>(stored_width);
}

struct EmptyBitpackingWriter {
	template <class T>
	static void WriteConstant(T constant, idx_t count, void *data_ptr, bool all_invalid) {
	}
	template <class T, class T_S = typename MakeSigned<T>::type>
	static void WriteConstantDelta(T_S constant, T frame_of_reference, idx_t count, T *values, bool *validity,
	                               void *data_ptr) {
	}
	template <class T, class T_S = typename MakeSigned<T>::type>
	static void WriteDeltaFor(T *values, bool *validity, bitpacking_width_t width, T frame_of_reference,
	                          T_S delta_offset, T *original_values, idx_t count, void *data_ptr) {
	}
	template <class T>
	static void WriteFor(T *values, bool *validity, bitpacking_width_t width, T frame_of_reference, idx_t count,
	                     void *data_ptr) {
	}
};

template <class T, class T_S = typename MakeSigned<T>::type>
struct BitpackingState {
public:
	BitpackingState() : compression_buffer_idx(0), total_size(0), data_ptr(nullptr) {
		compression_buffer_internal[0] = T(0);
		compression_buffer = &compression_buffer_internal[1];
		Reset();
	}

	// Extra val for delta encoding
	T compression_buffer_internal[BITPACKING_METADATA_GROUP_SIZE + 1];
	T *compression_buffer;
	T_S delta_buffer[BITPACKING_METADATA_GROUP_SIZE];
	bool compression_buffer_validity[BITPACKING_METADATA_GROUP_SIZE];
	idx_t compression_buffer_idx;
	idx_t total_size;

	// Used to pass CompressionState ptr through the Bitpacking writer
	void *data_ptr;

	// Stats on current compression buffer
	T minimum;
	T maximum;
	T min_max_diff;
	T_S minimum_delta;
	T_S maximum_delta;
	T_S min_max_delta_diff;
	T_S delta_offset;
	bool all_valid;
	bool all_invalid;

	bool has_valid;
	bool has_invalid;

	bool can_do_delta;
	bool can_do_for;

	// Used to force a specific mode, useful in testing
	BitpackingMode mode = BitpackingMode::AUTO;

public:
	void Reset() {
		minimum = NumericLimits<T>::Maximum();
		minimum_delta = NumericLimits<T_S>::Maximum();
		maximum = NumericLimits<T>::Minimum();
		maximum_delta = NumericLimits<T_S>::Minimum();
		delta_offset = 0;
		all_valid = true;
		all_invalid = true;
		has_valid = false;
		has_invalid = false;
		can_do_delta = false;
		can_do_for = false;
		compression_buffer_idx = 0;
		min_max_diff = 0;
		min_max_delta_diff = 0;
	}

	void CalculateFORStats() {
		can_do_for = TrySubtractOperator::Operation(maximum, minimum, min_max_diff);
	}

	void CalculateDeltaStats() {
		// TODO: currently we dont support delta compression of values above NumericLimits<T_S>::Maximum(),
		// 		 we could support this with some clever subtract trickery?
		if (maximum > static_cast<T>(NumericLimits<T_S>::Maximum())) {
			return;
		}

		// Don't delta encoding 1 value makes no sense
		if (compression_buffer_idx < 2) {
			return;
		}

		// TODO: handle NULLS here?
		// Currently we cannot handle nulls because we would need an additional step of patching for this.
		// we could for example copy the last value on a null insert. This would help a bit, but not be optimal for
		// large deltas since theres suddenly a zero then. Ideally we would insert a value that leads to a delta within
		// the current domain of deltas however we dont know that domain here yet
		if (!all_valid) {
			return;
		}

		// Note: since we dont allow any values over NumericLimits<T_S>::Maximum(), all subtractions for unsigned types
		// are guaranteed not to overflow
		bool can_do_all = true;
		if (NumericLimits<T>::IsSigned()) {
			T_S bogus;
			can_do_all = TrySubtractOperator::Operation(static_cast<T_S>(minimum), static_cast<T_S>(maximum), bogus) &&
			             TrySubtractOperator::Operation(static_cast<T_S>(maximum), static_cast<T_S>(minimum), bogus);
		}

		// Calculate delta's
		// compression_buffer pointer points one element ahead of the internal buffer making the use of signed index
		// integer (-1) possible
		D_ASSERT(compression_buffer_idx <= NumericLimits<int64_t>::Maximum());
		if (can_do_all) {
			for (int64_t i = 0; i < static_cast<int64_t>(compression_buffer_idx); i++) {
				delta_buffer[i] = static_cast<T_S>(compression_buffer[i]) - static_cast<T_S>(compression_buffer[i - 1]);
			}
		} else {
			for (int64_t i = 0; i < static_cast<int64_t>(compression_buffer_idx); i++) {
				auto success =
				    TrySubtractOperator::Operation(static_cast<T_S>(compression_buffer[i]),
				                                   static_cast<T_S>(compression_buffer[i - 1]), delta_buffer[i]);
				if (!success) {
					return;
				}
			}
		}

		can_do_delta = true;

		for (idx_t i = 1; i < compression_buffer_idx; i++) {
			maximum_delta = MaxValue<T_S>(maximum_delta, delta_buffer[i]);
			minimum_delta = MinValue<T_S>(minimum_delta, delta_buffer[i]);
		}

		// Since we can set the first value arbitrarily, we want to pick one from the current domain, note that
		// we will store the original first value - this offset as the  delta_offset to be able to decode this again.
		delta_buffer[0] = minimum_delta;

		can_do_delta = can_do_delta && TrySubtractOperator::Operation(maximum_delta, minimum_delta, min_max_delta_diff);
		can_do_delta = can_do_delta && TrySubtractOperator::Operation(static_cast<T_S>(compression_buffer[0]),
		                                                              minimum_delta, delta_offset);
	}

	template <class T_INNER, class T_U = typename MakeUnsigned<T_INNER>::type>
	void SubtractFrameOfReference(T_INNER *buffer, T_INNER frame_of_reference) {
		static_assert(NumericLimits<T_INNER>::IsIntegral(), "Integral type required.");

		for (idx_t i = 0; i < compression_buffer_idx; i++) {
			reinterpret_cast<T_U *>(buffer)[i] -= static_cast<T_U>(frame_of_reference);
		}
	}

	template <class OP>
	bool Flush() {
		if (compression_buffer_idx == 0) {
			return true;
		}

		if ((all_invalid || maximum == minimum) && (mode == BitpackingMode::AUTO || mode == BitpackingMode::CONSTANT)) {
			OP::WriteConstant(maximum, compression_buffer_idx, data_ptr, all_invalid);
			total_size += sizeof(T) + sizeof(bitpacking_metadata_encoded_t);
			return true;
		}

		CalculateFORStats();
		CalculateDeltaStats();

		if (can_do_delta) {
			if (maximum_delta == minimum_delta && mode != BitpackingMode::FOR && mode != BitpackingMode::DELTA_FOR) {
				// FOR needs to be T (considering hugeint is bigger than idx_t)
				T frame_of_reference = compression_buffer[0];

				OP::WriteConstantDelta(maximum_delta, static_cast<T>(frame_of_reference), compression_buffer_idx,
				                       compression_buffer, compression_buffer_validity, data_ptr);
				total_size += sizeof(T) + sizeof(T) + sizeof(bitpacking_metadata_encoded_t);
				return true;
			}

			// Check if delta has benefit
			auto delta_required_bitwidth =
			    BitpackingPrimitives::MinimumBitWidth<T, false>(static_cast<T>(min_max_delta_diff));
			auto regular_required_bitwidth = BitpackingPrimitives::MinimumBitWidth(min_max_diff);

			//! `min_max_diff` is uninitialized if `can_do_for` isn't true
			bool prefer_for = can_do_for && delta_required_bitwidth >= regular_required_bitwidth;

			if (!prefer_for && mode != BitpackingMode::FOR) {
				SubtractFrameOfReference(delta_buffer, minimum_delta);

				OP::WriteDeltaFor(reinterpret_cast<T *>(delta_buffer), compression_buffer_validity,
				                  delta_required_bitwidth, static_cast<T>(minimum_delta), delta_offset,
				                  compression_buffer, compression_buffer_idx, data_ptr);

				// FOR (frame of reference).
				total_size += sizeof(T);
				// Aligned bitpacking width.
				total_size += AlignValue(sizeof(bitpacking_width_t));
				// Delta offset.
				total_size += sizeof(T);
				// Compressed data size.
				total_size += BitpackingPrimitives::GetRequiredSize(compression_buffer_idx, delta_required_bitwidth);

				return true;
			}
		}

		if (can_do_for) {
			auto width = BitpackingPrimitives::MinimumBitWidth<T, false>(min_max_diff);
			SubtractFrameOfReference(compression_buffer, minimum);
			OP::WriteFor(compression_buffer, compression_buffer_validity, width, minimum, compression_buffer_idx,
			             data_ptr);

			total_size += BitpackingPrimitives::GetRequiredSize(compression_buffer_idx, width);
			total_size += sizeof(T); // FOR value
			total_size += AlignValue(sizeof(bitpacking_width_t));

			return true;
		}

		return false;
	}

	template <class OP = EmptyBitpackingWriter>
	bool Update(typename VectorIterator<T>::ValueEntry val) {
		auto is_valid = val.IsValid();
		compression_buffer_validity[compression_buffer_idx] = is_valid;
		has_valid = has_valid || is_valid;
		has_invalid = has_invalid || !is_valid;
		all_valid = all_valid && is_valid;
		all_invalid = all_invalid && !is_valid;

		if (is_valid) {
			auto value = val.GetValue();
			compression_buffer[compression_buffer_idx] = value;
			minimum = MinValue<T>(minimum, value);
			maximum = MaxValue<T>(maximum, value);
		}

		compression_buffer_idx++;

		if (compression_buffer_idx == BITPACKING_METADATA_GROUP_SIZE) {
			bool success = Flush<OP>();
			Reset();
			return success;
		}
		return true;
	}
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
template <class T>
struct BitpackingAnalyzeState : public AnalyzeState {
	explicit BitpackingAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager) {};
	BitpackingState<T> state;
};

template <class T>
unique_ptr<AnalyzeState> BitpackingInitAnalyze(ColumnData &col_data, PhysicalType type) {
	auto state = make_uniq<BitpackingAnalyzeState<T>>(col_data.GetBlockManager());
	state->state.mode = Settings::Get<ForceBitpackingModeSetting>(col_data.GetDatabase());

	return std::move(state);
}

template <class T>
bool BitpackingAnalyze(AnalyzeState &state, const Vector &input) {
	// We use BITPACKING_METADATA_GROUP_SIZE tuples, which can exceed the block size.
	// In that case, we disable bitpacking.
	// we are conservative here by multiplying by 2
	auto type_size = GetTypeIdSize(input.GetType().InternalType());
	if (type_size * BITPACKING_METADATA_GROUP_SIZE * 2 > state.info.GetBlockSize()) {
		return false;
	}

	auto &analyze_state = state.Cast<BitpackingAnalyzeState<T>>();
	for (auto entry : input.Values<T>()) {
		if (!analyze_state.state.template Update<EmptyBitpackingWriter>(entry)) {
			return false;
		}
	}
	return true;
}

template <class T>
idx_t BitpackingFinalAnalyze(AnalyzeState &state) {
	auto &bitpacking_state = state.Cast<BitpackingAnalyzeState<T>>();
	auto flush_result = bitpacking_state.state.template Flush<EmptyBitpackingWriter>();
	if (!flush_result) {
		return DConstants::INVALID_INDEX;
	}
	return bitpacking_state.state.total_size;
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
template <class T, bool WRITE_STATISTICS, class T_S = typename MakeSigned<T>::type>
struct BitpackingCompressionState : public StandardCompressionState {
public:
	explicit BitpackingCompressionState(ColumnDataCheckpointData &checkpoint_data)
	    : StandardCompressionState(checkpoint_data, CompressionType::COMPRESSION_BITPACKING) {
		CreateEmptySegment();

		state.data_ptr = reinterpret_cast<void *>(this);
		state.mode = Settings::Get<ForceBitpackingModeSetting>(checkpoint_data.GetDatabase());
	}

	StatsWriter<T> stats_writer;
	// Pointer to the next free position in the segment.
	data_ptr_t data_ptr;
	// Ptr to next free spot for storing bitwidths and frame-of-references (growing downwards).
	data_ptr_t metadata_ptr;

	BitpackingState<T> state;

public:
	struct BitpackingWriter {
		static void WriteConstant(T constant, idx_t count, void *data_ptr, bool all_invalid) {
			auto state = reinterpret_cast<BitpackingCompressionState<T, WRITE_STATISTICS> *>(data_ptr);

			ReserveSpace(state, sizeof(T));
			WriteMetaData(state, BitpackingMode::CONSTANT);
			WriteData(state->data_ptr, constant);

			UpdateStats(state, count);
		}

		static void WriteConstantDelta(T_S constant, T frame_of_reference, idx_t count, T *values, bool *validity,
		                               void *data_ptr) {
			auto state = reinterpret_cast<BitpackingCompressionState<T, WRITE_STATISTICS> *>(data_ptr);

			ReserveSpace(state, 2 * sizeof(T));
			WriteMetaData(state, BitpackingMode::CONSTANT_DELTA);
			WriteData(state->data_ptr, frame_of_reference);
			WriteData(state->data_ptr, constant);

			UpdateStats(state, count);
		}
		static void WriteDeltaFor(T *values, bool *validity, bitpacking_width_t width, T frame_of_reference,
		                          T_S delta_offset, T *original_values, idx_t count, void *data_ptr) {
			auto state = reinterpret_cast<BitpackingCompressionState<T, WRITE_STATISTICS> *>(data_ptr);

			auto bp_size = BitpackingPrimitives::GetRequiredSize(count, width);
			ReserveSpace(state, bp_size + 3 * sizeof(T));

			WriteMetaData(state, BitpackingMode::DELTA_FOR);
			WriteData(state->data_ptr, frame_of_reference);
			WriteData(state->data_ptr, static_cast<T>(width));
			WriteData(state->data_ptr, delta_offset);

			BitpackingPrimitives::PackBuffer<T, false>(state->data_ptr, values, count, width);
			state->data_ptr += bp_size;

			UpdateStats(state, count);
		}

		static void WriteFor(T *values, bool *validity, bitpacking_width_t width, T frame_of_reference, idx_t count,
		                     void *data_ptr) {
			auto state = reinterpret_cast<BitpackingCompressionState<T, WRITE_STATISTICS> *>(data_ptr);

			auto bp_size = BitpackingPrimitives::GetRequiredSize(count, width);
			ReserveSpace(state, bp_size + 2 * sizeof(T));

			WriteMetaData(state, BitpackingMode::FOR);
			WriteData(state->data_ptr, frame_of_reference);
			WriteData(state->data_ptr, (T)width);

			BitpackingPrimitives::PackBuffer<T, false>(state->data_ptr, values, count, width);
			state->data_ptr += bp_size;

			UpdateStats(state, count);
		}

		template <class T_OUT>
		static void WriteData(data_ptr_t &ptr, T_OUT val) {
			*reinterpret_cast<T_OUT *>(ptr) = val;
			ptr += sizeof(T_OUT);
		}

		static void WriteMetaData(BitpackingCompressionState<T, WRITE_STATISTICS> *state, BitpackingMode mode) {
			bitpacking_metadata_t metadata {mode, (uint32_t)(state->data_ptr - state->handle.GetDataMutable())};
			state->metadata_ptr -= sizeof(bitpacking_metadata_encoded_t);
			Store<bitpacking_metadata_encoded_t>(EncodeMeta(metadata), state->metadata_ptr);
		}

		static void ReserveSpace(BitpackingCompressionState<T, WRITE_STATISTICS> *state, idx_t data_bytes) {
			idx_t meta_bytes = sizeof(bitpacking_metadata_encoded_t);
			state->FlushAndCreateSegmentIfFull(data_bytes, meta_bytes);
			D_ASSERT(state->CanStore(data_bytes, meta_bytes));
		}

		static void UpdateStats(BitpackingCompressionState<T, WRITE_STATISTICS> *state, idx_t count) {
			state->current_segment->count += count;

			if (WRITE_STATISTICS) {
				auto &stats_writer = state->stats_writer;
				if (state->state.has_valid) {
					stats_writer.SetHasValid();
					stats_writer.UpdateMinMax(state->state.minimum);
					stats_writer.UpdateMinMax(state->state.maximum);
				}
				if (state->state.has_invalid) {
					stats_writer.SetHasNull();
				}
			}
		}
	};

	bool CanStore(idx_t data_bytes, idx_t meta_bytes) {
		auto required_data_bytes = AlignValue<idx_t>(UnsafeNumericCast<idx_t>((data_ptr + data_bytes) - data_ptr));
		auto required_meta_bytes = info.GetBlockSize() - UnsafeNumericCast<idx_t>(metadata_ptr - data_ptr) + meta_bytes;

		return required_data_bytes + required_meta_bytes <=
		       info.GetBlockSize() - BitpackingPrimitives::BITPACKING_HEADER_SIZE;
	}

	void CreateEmptySegment() {
		CreateAndPinNewSegment();

		data_ptr = handle.GetDataMutable() + BitpackingPrimitives::BITPACKING_HEADER_SIZE;
		metadata_ptr = handle.GetDataMutable() + info.GetBlockSize();
	}

	void Append(const Vector &input) {
		for (auto entry : input.Values<T>()) {
			state.template Update<BitpackingWriter>(entry);
		}
	}

	void FlushAndCreateSegmentIfFull(idx_t required_data_bytes, idx_t required_meta_bytes) {
		if (!CanStore(required_data_bytes, required_meta_bytes)) {
			FlushSegment();
			CreateEmptySegment();
		}
	}

	void FlushSegment() {
		auto base_ptr = handle.GetDataMutable();

		// Compact the segment by moving the metadata next to the data.

		idx_t unaligned_offset = NumericCast<idx_t>(data_ptr - base_ptr);
		idx_t metadata_offset = AlignValue(unaligned_offset);
		idx_t metadata_size = NumericCast<idx_t>(base_ptr + info.GetBlockSize() - metadata_ptr);
		idx_t total_segment_size = metadata_offset + metadata_size;

		// Asserting things are still sane here
		if (!CanStore(0, 0)) {
			InternalException::Throw("Error in bitpacking size calculation");
		}

		if (unaligned_offset != metadata_offset) {
			// zero initialize any padding bits
			memset(base_ptr + unaligned_offset, 0, metadata_offset - unaligned_offset);
		}
		memmove(base_ptr + metadata_offset, metadata_ptr, metadata_size);

		// Store the offset of the metadata of the first group (which is at the highest address).
		Store<idx_t>(metadata_offset + metadata_size, base_ptr);
		FlushCurrentSegment(stats_writer, total_segment_size);
	}

	void Finalize() {
		state.template Flush<BitpackingCompressionState<T, WRITE_STATISTICS, T_S>::BitpackingWriter>();
		FlushSegment();
		current_segment.reset();
	}
};

template <class T, bool WRITE_STATISTICS>
unique_ptr<CompressionState> BitpackingInitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                       unique_ptr<AnalyzeState> state) {
	return make_uniq<BitpackingCompressionState<T, WRITE_STATISTICS>>(checkpoint_data);
}

template <class T, bool WRITE_STATISTICS>
void BitpackingCompress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<BitpackingCompressionState<T, WRITE_STATISTICS>>();
	state.Append(scan_vector);
}

template <class T, bool WRITE_STATISTICS>
void BitpackingFinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<BitpackingCompressionState<T, WRITE_STATISTICS>>();
	state.Finalize();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
template <class T, class T_U = typename MakeUnsigned<T>::type>
static void ApplyFrameOfReference(unsafe_array_ptr<T> values, T frame_of_reference) {
	if (!frame_of_reference) {
		return;
	}

	// The frame of reference is read from disk, so add it with defined wrapping for every T value.
	auto unsigned_values = reinterpret_cast<T_U *>(values.data());
	for (idx_t i = 0; i < values.size(); i++) {
		unsigned_values[i] += static_cast<T_U>(frame_of_reference);
	}
}

// Based on https://github.com/lemire/FastPFor (Apache License 2.0)
template <class T, class T_U = typename MakeUnsigned<T>::type>
static T DeltaDecode(unsafe_array_ptr<T> values, T previous_value) {
	D_ASSERT(!values.empty());

	// Use unsigned arithmetic to avoid signed overflow on corrupt data.
	auto udata = reinterpret_cast<T_U *>(values.data());
	T_U carry = static_cast<T_U>(previous_value);

	auto count = values.size();
	const size_t UnrollQty = 4;
	const size_t sz0 = (count / UnrollQty) * UnrollQty;
	size_t i = 0;
	for (; i < sz0; i += UnrollQty) {
		carry = udata[i] += carry;
		carry = udata[i + 1] += carry;
		carry = udata[i + 2] += carry;
		carry = udata[i + 3] += carry;
	}
	for (; i < count; ++i) {
		carry = udata[i] += carry;
	}

	return values[count - 1];
}

template <class T, class T_S = typename MakeSigned<T>::type>
struct BitpackingScanState : public SegmentScanState {
private:
	//! Alignment type for UnPackGroup<T>, 64-bit and hugeint payloads are read through a uint32_t pointer.
	using T_PACKED = std::conditional_t<(sizeof(T) <= sizeof(uint32_t)), T, uint32_t>;

public:
	//! No metadata group has been loaded, the scan is positioned at the first row.
	struct Initial {};

	//! The scan has reached the end of the segment, with no metadata group loaded.
	struct Finished {};

	//! Scan position, header values, and packed payload for one metadata group, published only after validation.
	struct CurrentGroup {
		//! Index of this metadata group.
		idx_t index;
		//! Current row offset within this metadata group.
		idx_t offset;
		//! Group row count derived from the segment row count read from disk.
		idx_t count;
		//! Group descriptor decoded and validated from metadata read from disk.
		bitpacking_metadata_t metadata;
		//! Width read from disk, validated for T before use.
		bitpacking_width_t width;
		//! Frame of reference read from disk, arithmetic must accept every bit pattern of T.
		T frame_of_reference;
		//! Constant read from disk, arithmetic must accept every bit pattern of T.
		T constant;
		//! Delta offset initialized from disk, arithmetic must accept every bit pattern of T.
		T delta_offset;
		//! Packed payload read from disk and validated by the group reader, absent when no payload bytes are stored.
		optional<unsafe_array_ptr<const uint8_t>> payload;
		//! Algorithm group count derived from the segment row count read from disk.
		idx_t algorithm_group_count;
		//! Algorithm group byte size derived from the validated width read from disk.
		idx_t algorithm_group_size;

		idx_t Remaining() const {
			D_ASSERT(offset <= count);
			return count - offset;
		}

		bool AtEnd() const {
			return offset == count;
		}

		void Advance(idx_t advance_count) {
			D_ASSERT(advance_count <= Remaining());
			offset += advance_count;
		}

		//! Unpack complete groups, starting with the group containing offset.
		//! An unaligned offset only permits one group; its padding may extend beyond count.
		void UnpackAlgorithmGroups(unsafe_array_ptr<T> target) const {
			D_ASSERT(!AtEnd());
			D_ASSERT(metadata.mode == BitpackingMode::FOR || metadata.mode == BitpackingMode::DELTA_FOR);
			D_ASSERT(target.size() > 0);
			D_ASSERT(target.size() % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0);
			auto groups = target.size() / BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
			auto algorithm_group_index = offset / BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
			D_ASSERT(offset % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0 || groups == 1);
			D_ASSERT(algorithm_group_index < algorithm_group_count);
			D_ASSERT(groups <= algorithm_group_count - algorithm_group_index);
			if (width == 0) {
				std::fill(target.data(), target.data() + target.size(), T(0));
				return;
			}
			if (!payload) {
				InternalException::Throw("Bitpacking algorithm group has no payload");
			}
			auto algorithm_group_offset = algorithm_group_index * algorithm_group_size;
			auto packed_groups = payload->SubArray(algorithm_group_offset, groups * algorithm_group_size);
			// FOR residuals are non-negative, so they never need sign extension.
			BitpackingPrimitives::UnPackBuffer<T>(data_ptr_cast(target.data()), packed_groups.data(), target.size(),
			                                      width, true);
		}
	};

	static idx_t GetMetadataTableStart(const CompressionSegmentReader &reader, idx_t group_count) {
		// The metadata end is read from disk and determines the start of the reverse metadata table.
		return reader.Get<idx_t>(0) - group_count * sizeof(bitpacking_metadata_encoded_t);
	}

	explicit BitpackingScanState(BufferHandle handle_p, ColumnSegment &segment)
	    : handle(std::move(handle_p)),
	      reader(CompressionSegmentReader::FromSegment(handle, segment, "bitpacking segment")),
	      is_bool(segment.GetType().InternalType() == PhysicalType::BOOL), segment_count(segment.count.load()),
	      group_count(segment_count / BITPACKING_METADATA_GROUP_SIZE +
	                  (segment_count % BITPACKING_METADATA_GROUP_SIZE != 0)),
	      metadata_table_start(GetMetadataTableStart(reader, group_count)),
	      metadata_table(reader.GetArray<bitpacking_metadata_encoded_t>(metadata_table_start, group_count)) {
		if (metadata_table_start < BitpackingPrimitives::BITPACKING_HEADER_SIZE) {
			ThrowBitpackingGroupOffsetsInvalid();
		}
		reader = reader.GetSubReader(BitpackingPrimitives::BITPACKING_HEADER_SIZE,
		                             metadata_table_start - BitpackingPrimitives::BITPACKING_HEADER_SIZE,
		                             "bitpacking group data");
	}

	BufferHandle handle;
	//! Group data between the segment header and reverse metadata table.
	CompressionSegmentReader reader;
	//! BOOL segments are bitpacked as int8_t, but may only contain 0 and 1
	bool is_bool;
	//! Segment row count retained for consistent group bounds.
	idx_t segment_count;

	T decompression_buffer[BITPACKING_METADATA_GROUP_SIZE];

	//! Group count derived from the segment row count read from disk.
	idx_t group_count;
	//! Metadata table start derived from the metadata end read from disk.
	idx_t metadata_table_start;
	//! Reverse metadata table read from disk, bounds-checked for group_count entries.
	unsafe_array_ptr<const bitpacking_metadata_encoded_t> metadata_table;

	//! Current scan state, with group data present only while a validated metadata group is active.
	std::variant<Initial, CurrentGroup, Finished> group_state = Initial {};

public:
	//! Get a group descriptor from the reverse metadata table.
	bitpacking_metadata_t GetGroupMetadata(idx_t group_index) const {
		if (group_index >= group_count) {
			ThrowBitpackingGroupIndexOutOfRange();
		}
		return DecodeMeta(metadata_table[group_count - 1 - group_index]);
	}

	CurrentGroup &GetCurrentGroup() {
		return std::get<CurrentGroup>(group_state);
	}

	const CurrentGroup &GetCurrentGroup() const {
		return std::get<CurrentGroup>(group_state);
	}

	bool IsInitial() const {
		return std::holds_alternative<Initial>(group_state);
	}

	bool HasCurrentGroup() const {
		return std::holds_alternative<CurrentGroup>(group_state);
	}

	bool IsFinished() const {
		return std::holds_alternative<Finished>(group_state);
	}

	void Finish() {
		group_state = Finished {};
	}

	//! Loads the selected group's mode-specific header and validates its packed payload range.
	void LoadGroup(idx_t group_index) {
		auto current_group = GetGroupMetadata(group_index);

		// Group boundaries come from metadata read from disk, so load and validate them.
		auto group_start = current_group.offset;
		auto group_end =
		    group_index + 1 < group_count ? GetGroupMetadata(group_index + 1).offset : metadata_table_start;
		if (group_start < BitpackingPrimitives::BITPACKING_HEADER_SIZE || group_start > group_end ||
		    group_end > metadata_table_start) {
			ThrowBitpackingGroupOffsetsInvalid();
		}
		// Group offsets are relative to the segment start, while reader starts after the segment header.
		auto group_data_offset = group_start - BitpackingPrimitives::BITPACKING_HEADER_SIZE;
		auto group_reader = reader.GetSubReader(group_data_offset, group_end - group_start, "bitpacking group data");
		bitpacking_width_t current_width = 0;
		T current_frame_of_reference = 0;
		T current_constant = 0;
		T current_delta_offset = 0;

		// Read first value
		switch (current_group.mode) {
		case BitpackingMode::CONSTANT:
			current_constant = group_reader.template Read<T>();
			break;
		case BitpackingMode::FOR:
		case BitpackingMode::CONSTANT_DELTA:
		case BitpackingMode::DELTA_FOR:
			current_frame_of_reference = group_reader.template Read<T>();
			break;
		default:
			ThrowBitpackingUnknownMode();
		}

		// Read second value
		switch (current_group.mode) {
		case BitpackingMode::CONSTANT_DELTA:
			current_constant = group_reader.template Read<T>();
			break;
		case BitpackingMode::FOR:
		case BitpackingMode::DELTA_FOR: {
			auto stored_width = group_reader.template Read<T>();
			current_width = ValidateBitpackingWidth<T>(stored_width);
			break;
		}
		case BitpackingMode::CONSTANT:
			break;
		default:
			ThrowBitpackingUnknownMode();
		}

		// Read third value
		if (current_group.mode == BitpackingMode::DELTA_FOR) {
			current_delta_offset = group_reader.template Read<T>();
		}

		// The row count for this group comes from the segment count read from disk.
		auto group_row_count = MinValue<idx_t>(BITPACKING_METADATA_GROUP_SIZE,
		                                       segment_count - group_index * BITPACKING_METADATA_GROUP_SIZE);
		if (is_bool) {
			ValidateBooleanGroup<T>(current_group.mode, current_width, current_frame_of_reference, current_constant,
			                        group_row_count);
		}

		// The payload size comes from the group row count and width read from disk, so calculate and validate it.
		idx_t algorithm_group_count = 0;
		idx_t algorithm_group_size = 0;
		idx_t expected_payload_size = 0;
		if (current_group.mode == BitpackingMode::FOR || current_group.mode == BitpackingMode::DELTA_FOR) {
			algorithm_group_count = group_row_count / BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE +
			                        (group_row_count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE != 0);
			algorithm_group_size = BitpackingPrimitives::GetRequiredSize(
			    BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE, current_width);
			expected_payload_size = algorithm_group_count * algorithm_group_size;
		}
		optional<unsafe_array_ptr<const uint8_t>> payload;
		if (expected_payload_size > 0) {
			payload = group_reader.template ReadBytesAligned<T_PACKED>(expected_payload_size);
		}
		CurrentGroup loaded_group {group_index,
		                           0,
		                           group_row_count,
		                           current_group,
		                           current_width,
		                           current_frame_of_reference,
		                           current_constant,
		                           current_delta_offset,
		                           payload,
		                           algorithm_group_count,
		                           algorithm_group_size};

		// Publish the group only after all disk-derived values and payload bounds have been validated.
		group_state = std::move(loaded_group);
	}

	void Skip(idx_t skip_count) {
		if (IsFinished()) {
			if (skip_count > 0) {
				ThrowBitpackingReadPastEnd();
			}
			return;
		}

		// Initial starts at group 0 offset 0, otherwise resume from the active group.
		auto active_group = std::get_if<CurrentGroup>(&group_state);
		D_ASSERT(active_group || IsInitial());
		idx_t group_index = active_group ? active_group->index : 0;
		idx_t group_offset = active_group ? active_group->offset : 0;

		idx_t skipped = 0;

		// This skips straight to the correct metadata group
		idx_t meta_groups_to_skip = (skip_count + group_offset) / BITPACKING_METADATA_GROUP_SIZE;
		if (meta_groups_to_skip) {
			idx_t target_group_index = group_index + meta_groups_to_skip;
			bool skip_lands_exactly_on_group_boundary =
			    (skip_count + group_offset) % BITPACKING_METADATA_GROUP_SIZE == 0;
			if (target_group_index > group_count ||
			    (target_group_index == group_count && !skip_lands_exactly_on_group_boundary)) {
				ThrowBitpackingReadPastEnd();
			}

			// Remove rows from the current offset to the start of the target group.
			auto skipped_group_rows = meta_groups_to_skip * BITPACKING_METADATA_GROUP_SIZE - group_offset;
			D_ASSERT(skipped_group_rows <= skip_count);
			skipped += skipped_group_rows;

			if (target_group_index == group_count) {
				// No group exists after the terminal position.
				Finish();
				D_ASSERT(skipped == skip_count);
				return;
			}
			LoadGroup(target_group_index);
		}
		if (IsInitial()) {
			// A skip that remains in the first group still needs that group loaded.
			LoadGroup(0);
		}
		auto &current_group = GetCurrentGroup();

		if (skipped > skip_count) {
			ThrowBitpackingReadPastEnd();
		}
		auto remaining_to_skip = skip_count - skipped;
		if (current_group.offset > current_group.count ||
		    remaining_to_skip > current_group.count - current_group.offset) {
			ThrowBitpackingReadPastEnd();
		}

		if (current_group.metadata.mode == BitpackingMode::CONSTANT ||
		    current_group.metadata.mode == BitpackingMode::CONSTANT_DELTA ||
		    current_group.metadata.mode == BitpackingMode::FOR) {
			// Skipping within a non-delta group advances the current group's row offset.
			current_group.Advance(remaining_to_skip);
			skipped += remaining_to_skip;
		} else {
			// DELTA_FOR must decode skipped values to retain the preceding delta.
			D_ASSERT(current_group.metadata.mode == BitpackingMode::DELTA_FOR);

			while (skipped < skip_count) {
				idx_t offset_in_compression_group =
				    current_group.offset % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
				idx_t skipping_this_algorithm_group =
				    MinValue(skip_count - skipped,
				             BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE - offset_in_compression_group);

				current_group.UnpackAlgorithmGroups(
				    unsafe_array_ptr<T>(decompression_buffer, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE));

				unsafe_array_ptr<T_S> decompression_values(reinterpret_cast<T_S *>(decompression_buffer),
				                                           BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE);
				auto skipped_values =
				    decompression_values.SubArray(offset_in_compression_group, skipping_this_algorithm_group);
				ApplyFrameOfReference<T_S>(skipped_values, static_cast<T_S>(current_group.frame_of_reference));
				// The last skipped value becomes the preceding value for the next delta decode.
				current_group.delta_offset =
				    static_cast<T>(DeltaDecode<T_S>(skipped_values, static_cast<T_S>(current_group.delta_offset)));

				current_group.Advance(skipping_this_algorithm_group);
				skipped += skipping_this_algorithm_group;
			}
		}

		D_ASSERT(skipped == skip_count);
		if (current_group.AtEnd() && current_group.index + 1 == group_count) {
			Finish();
		}
	}
};

template <class T>
unique_ptr<SegmentScanState> BitpackingInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	auto result = make_uniq<BitpackingScanState<T>>(std::move(handle), segment);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T, class T_S = typename MakeSigned<T>::type, class T_U = typename MakeUnsigned<T>::type>
void BitpackingScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                           idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<BitpackingScanState<T>>();
	if (scan_count == 0) {
		return;
	}

	T *result_data = FlatVector::GetDataMutable<T>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	if (scan_state.IsInitial()) {
		scan_state.LoadGroup(0);
	}
	if (!scan_state.HasCurrentGroup()) {
		ThrowBitpackingReadPastEnd();
	}

	idx_t scanned = 0;
	while (scanned < scan_count) {
		auto &current_group = scan_state.GetCurrentGroup();
		if (current_group.AtEnd()) {
			D_ASSERT(current_group.index + 1 < scan_state.group_count);
			scan_state.LoadGroup(current_group.index + 1);
			continue;
		}

		idx_t offset_in_compression_group =
		    current_group.offset % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;

		if (current_group.metadata.mode == BitpackingMode::CONSTANT) {
			idx_t remaining = scan_count - scanned;
			idx_t to_scan = MinValue(remaining, current_group.Remaining());
			T *begin = result_data + result_offset + scanned;
			T *end = begin + to_scan;
			std::fill(begin, end, current_group.constant);
			scanned += to_scan;
			current_group.Advance(to_scan);
			continue;
		}
		if (current_group.metadata.mode == BitpackingMode::CONSTANT_DELTA) {
			idx_t remaining = scan_count - scanned;
			idx_t to_scan = MinValue(remaining, current_group.Remaining());
			T *target_ptr = result_data + result_offset + scanned;

			for (idx_t i = 0; i < to_scan; i++) {
				idx_t multiplier = current_group.offset + i;
				// Operands read from disk can contain any T value, so use defined wrapping.
				target_ptr[i] = static_cast<T>((static_cast<T_U>(current_group.constant) * multiplier) +
				                               static_cast<T_U>(current_group.frame_of_reference));
			}

			scanned += to_scan;
			current_group.Advance(to_scan);
			continue;
		}
		D_ASSERT(current_group.metadata.mode == BitpackingMode::FOR ||
		         current_group.metadata.mode == BitpackingMode::DELTA_FOR);

		idx_t to_scan = MinValue<idx_t>(scan_count - scanned, current_group.Remaining());
		T *current_result_ptr = result_data + result_offset + scanned;

		if (offset_in_compression_group == 0 && to_scan >= BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) {
			// Decode complete algorithm groups together, then apply the frame once to the batch.
			to_scan -= to_scan % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
			to_scan = MinValue<idx_t>(to_scan, BITPACKING_SCAN_BATCH_SIZE);
			current_group.UnpackAlgorithmGroups(unsafe_array_ptr<T>(current_result_ptr, to_scan));
		} else {
			to_scan = MinValue<idx_t>(to_scan, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE -
			                                       offset_in_compression_group);
			// Decompress compression algorithm to buffer
			current_group.UnpackAlgorithmGroups(unsafe_array_ptr<T>(
			    scan_state.decompression_buffer, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE));

			memcpy(current_result_ptr, scan_state.decompression_buffer + offset_in_compression_group,
			       to_scan * sizeof(T));
		}

		if (current_group.metadata.mode == BitpackingMode::DELTA_FOR) {
			unsafe_array_ptr<T_S> current_results(reinterpret_cast<T_S *>(current_result_ptr), to_scan);
			ApplyFrameOfReference<T_S>(current_results, static_cast<T_S>(current_group.frame_of_reference));
			current_group.delta_offset =
			    static_cast<T>(DeltaDecode<T_S>(current_results, static_cast<T_S>(current_group.delta_offset)));
			if (scan_state.is_bool) {
				VerifyBooleanValues(current_result_ptr, to_scan);
			}
		} else {
			ApplyFrameOfReference<T>(unsafe_array_ptr<T>(current_result_ptr, to_scan),
			                         current_group.frame_of_reference);
		}

		scanned += to_scan;
		current_group.Advance(to_scan);
	}

	auto &current_group = scan_state.GetCurrentGroup();
	if (current_group.AtEnd() && current_group.index + 1 == scan_state.group_count) {
		scan_state.Finish();
	}
}

template <class T>
void BitpackingScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	BitpackingScanPartial<T>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Skip
//===--------------------------------------------------------------------===//
template <class T>
void BitpackingSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	if (skip_count == 0) {
		return;
	}
	auto &scan_state = static_cast<BitpackingScanState<T> &>(*state.scan_state);
	scan_state.Skip(skip_count);
}

//===--------------------------------------------------------------------===//
// GetSegmentInfo
//===--------------------------------------------------------------------===//
template <class T>
InsertionOrderPreservingMap<string> BitpackingGetSegmentInfo(QueryContext context, ColumnSegment &segment) {
	map<BitpackingMode, idx_t> counts;
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	BitpackingScanState<T> scan_state(std::move(handle), segment);
	for (idx_t group_index = 0; group_index < scan_state.group_count; group_index++) {
		auto mode = scan_state.GetGroupMetadata(group_index).mode;
		ValidateBitpackingMode(mode);
		counts[mode]++;
	}

	InsertionOrderPreservingMap<string> result;
	for (auto &it : counts) {
		auto &mode = it.first;
		auto &count = it.second;
		result[EnumUtil::ToString(mode)] = StringUtil::Format("%d", count);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
template <class T, bool WRITE_STATISTICS = true>
CompressionFunction GetBitpackingFunction(PhysicalType data_type) {
	auto bitpacking = CompressionFunction(
	    CompressionType::COMPRESSION_BITPACKING, data_type, BitpackingInitAnalyze<T>, BitpackingAnalyze<T>,
	    BitpackingFinalAnalyze<T>, BitpackingInitCompression<T, WRITE_STATISTICS>,
	    BitpackingCompress<T, WRITE_STATISTICS>, BitpackingFinalizeCompress<T, WRITE_STATISTICS>, BitpackingInitScan<T>,
	    BitpackingScan<T>, BitpackingScanPartial<T>, BitpackingSkip<T>);
	bitpacking.get_segment_info = BitpackingGetSegmentInfo<T>;
	return bitpacking;
}

CompressionFunction BitpackingFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return GetBitpackingFunction<int8_t>(type);
	case PhysicalType::INT16:
		return GetBitpackingFunction<int16_t>(type);
	case PhysicalType::INT32:
		return GetBitpackingFunction<int32_t>(type);
	case PhysicalType::INT64:
		return GetBitpackingFunction<int64_t>(type);
	case PhysicalType::UINT8:
		return GetBitpackingFunction<uint8_t>(type);
	case PhysicalType::UINT16:
		return GetBitpackingFunction<uint16_t>(type);
	case PhysicalType::UINT32:
		return GetBitpackingFunction<uint32_t>(type);
	case PhysicalType::UINT64:
		return GetBitpackingFunction<uint64_t>(type);
	case PhysicalType::INT128:
		return GetBitpackingFunction<hugeint_t>(type);
	case PhysicalType::UINT128:
		return GetBitpackingFunction<uhugeint_t>(type);
	case PhysicalType::LIST:
		return GetBitpackingFunction<uint64_t, false>(type);
	default:
		throw InternalException("Unsupported type for Bitpacking");
	}
}

bool BitpackingFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::LIST:
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
