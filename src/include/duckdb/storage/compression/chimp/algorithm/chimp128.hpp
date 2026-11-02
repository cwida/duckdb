//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/chimp/algorithm/chimp128.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/chimp/algorithm/chimp_utils.hpp"
#include "duckdb/storage/compression/chimp/algorithm/leading_zero_buffer.hpp"
#include "duckdb/storage/compression/chimp/algorithm/flag_buffer.hpp"
#include "duckdb/storage/compression/chimp/algorithm/ring_buffer.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/storage/compression/chimp/algorithm/packed_data.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/bit_utils.hpp"

#include "duckdb/storage/compression/chimp/algorithm/bit_reader.hpp"
#include "duckdb/storage/compression/chimp/algorithm/output_bit_stream.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Compression
//===--------------------------------------------------------------------===//

template <class CHIMP_TYPE, bool EMPTY>
struct Chimp128CompressionState {
	Chimp128CompressionState() : ring_buffer(), previous_leading_zeros(NumericLimits<uint8_t>::Maximum()) {
		previous_value = 0;
	}

	inline void SetLeadingZeros(int32_t value = NumericLimits<uint8_t>::Maximum()) {
		this->previous_leading_zeros = value;
	}

	void Flush() {
		leading_zero_buffer.Flush();
	}

	// Reset the state
	void Reset() {
		first = true;
		ring_buffer.Reset();
		SetLeadingZeros();
		leading_zero_buffer.Reset();
		flag_buffer.Reset();
		packed_data_buffer.Reset();
		previous_value = 0;
	}

	CHIMP_TYPE BitsWritten() const {
		return output.BitsWritten() + leading_zero_buffer.BitsWritten() + flag_buffer.BitsWritten() +
		       (packed_data_buffer.index * 16);
	}

	OutputBitStream<EMPTY> output; // The stream to write to
	LeadingZeroBuffer<EMPTY> leading_zero_buffer;
	FlagBuffer<EMPTY> flag_buffer;
	PackedDataBuffer<EMPTY> packed_data_buffer;
	RingBuffer<CHIMP_TYPE> ring_buffer; //! The ring buffer that holds the previous values
	uint8_t previous_leading_zeros;     //! The leading zeros of the reference value
	CHIMP_TYPE previous_value = 0;
	bool first = true;
};

template <class CHIMP_TYPE, bool EMPTY>
class Chimp128Compression {
public:
	using State = Chimp128CompressionState<CHIMP_TYPE, EMPTY>;

	//! The amount of bits needed to store an index between 0-127
	static constexpr uint8_t INDEX_BITS_SIZE = 7;
	static constexpr uint8_t BIT_SIZE = sizeof(CHIMP_TYPE) * 8;

	static constexpr uint8_t TRAILING_ZERO_THRESHOLD = SignificantBits<CHIMP_TYPE>::size + INDEX_BITS_SIZE;

	static void Store(CHIMP_TYPE in, State &state) {
		if (state.first) {
			WriteFirst(in, state);
		} else {
			CompressValue(in, state);
		}
	}

	//! Write the content of the bit buffer to the stream
	static void Flush(State &state) {
		if (!EMPTY) {
			state.output.Flush();
		}
	}

	static void WriteFirst(CHIMP_TYPE in, State &state) {
		state.ring_buffer.template Insert<true>(in);
		state.output.template WriteValue<CHIMP_TYPE, BIT_SIZE>(in);
		state.previous_value = in;
		state.first = false;
	}

	static void CompressValue(CHIMP_TYPE in, State &state) {
		auto key = state.ring_buffer.Key(in);
		CHIMP_TYPE xor_result;
		uint8_t previous_index;
		uint32_t trailing_zeros = 0;
		bool trailing_zeros_exceed_threshold = false;
		const CHIMP_TYPE reference_index = state.ring_buffer.IndexOf(key);

		// Find the reference value to use when compressing the current value
		if (((int64_t)state.ring_buffer.Size() - (int64_t)reference_index) < (int64_t)ChimpConstants::BUFFER_SIZE) {
			// The reference index is within 128 values, we can use it
			auto current_index = state.ring_buffer.IndexOf(key);
			if (current_index > state.ring_buffer.Size()) {
				current_index = 0;
			}
			auto reference_value = state.ring_buffer.Value(current_index % ChimpConstants::BUFFER_SIZE);
			CHIMP_TYPE tempxor_result = (CHIMP_TYPE)in ^ reference_value;
			trailing_zeros = CountZeros<CHIMP_TYPE>::Trailing(tempxor_result);
			trailing_zeros_exceed_threshold = trailing_zeros > TRAILING_ZERO_THRESHOLD;
			if (trailing_zeros_exceed_threshold) {
				previous_index = current_index % ChimpConstants::BUFFER_SIZE;
				xor_result = tempxor_result;
			} else {
				previous_index = state.ring_buffer.Size() % ChimpConstants::BUFFER_SIZE;
				xor_result = (CHIMP_TYPE)in ^ state.ring_buffer.Value(previous_index);
			}
		} else {
			// Reference index is not in range, use the directly previous value
			previous_index = state.ring_buffer.Size() % ChimpConstants::BUFFER_SIZE;
			xor_result = (CHIMP_TYPE)in ^ state.ring_buffer.Value(previous_index);
		}

		// Compress the value
		if (xor_result == 0) {
			state.flag_buffer.Insert(ChimpConstants::Flags::VALUE_IDENTICAL);
			state.output.template WriteValue<uint8_t, INDEX_BITS_SIZE>(previous_index);
			state.SetLeadingZeros();
		} else {
			// Values are not identical
			auto leading_zeros_raw = CountZeros<CHIMP_TYPE>::Leading(xor_result);
			uint8_t leading_zeros = ChimpConstants::Compression::LEADING_ROUND[leading_zeros_raw];

			if (trailing_zeros_exceed_threshold) {
				state.flag_buffer.Insert(ChimpConstants::Flags::TRAILING_EXCEEDS_THRESHOLD);
				uint32_t significant_bits = BIT_SIZE - leading_zeros - trailing_zeros;
				auto result = PackedDataUtils<CHIMP_TYPE>::Pack(
				    reference_index, ChimpConstants::Compression::LEADING_REPRESENTATION[leading_zeros],
				    significant_bits);
				state.packed_data_buffer.Insert(result & 0xFFFF);
				state.output.template WriteValue<CHIMP_TYPE>(xor_result >> trailing_zeros, significant_bits);
				state.SetLeadingZeros();
			} else if (leading_zeros == state.previous_leading_zeros) {
				state.flag_buffer.Insert(ChimpConstants::Flags::LEADING_ZERO_EQUALITY);
				int32_t significant_bits = BIT_SIZE - leading_zeros;
				state.output.template WriteValue<CHIMP_TYPE>(xor_result, significant_bits);
			} else {
				state.flag_buffer.Insert(ChimpConstants::Flags::LEADING_ZERO_LOAD);
				const int32_t significant_bits = BIT_SIZE - leading_zeros;
				state.leading_zero_buffer.Insert(ChimpConstants::Compression::LEADING_REPRESENTATION[leading_zeros]);
				state.output.template WriteValue<CHIMP_TYPE>(xor_result, significant_bits);
				state.SetLeadingZeros(leading_zeros);
			}
		}
		state.previous_value = in;
		state.ring_buffer.Insert(in);
	}
};

//===--------------------------------------------------------------------===//
// Decompression
//===--------------------------------------------------------------------===//

template <class CHIMP_TYPE>
struct Chimp128DecompressionState {
public:
	Chimp128DecompressionState() : reference_value(0), first(true) {
		ResetZeros();
	}

	void Reset() {
		ResetZeros();
		reference_value = 0;
		ring_buffer.Reset();
		first = true;
	}

	inline void ResetZeros() {
		leading_zeros = NumericLimits<uint8_t>::Maximum();
		trailing_zeros = 0;
	}

	inline void SetLeadingZeros(uint8_t value) {
		leading_zeros = value;
	}

	inline void SetTrailingZeros(uint8_t value) {
		D_ASSERT(value <= sizeof(CHIMP_TYPE) * 8);
		trailing_zeros = value;
	}

	uint8_t LeadingZeros() const {
		return leading_zeros;
	}
	uint8_t TrailingZeros() const {
		return trailing_zeros;
	}

	uint8_t leading_zeros;
	uint8_t trailing_zeros;
	CHIMP_TYPE reference_value = 0;
	RingBuffer<CHIMP_TYPE> ring_buffer;

	bool first;
};

template <class CHIMP_TYPE>
struct Chimp128Decompression {
public:
	using DecompressState = Chimp128DecompressionState<CHIMP_TYPE>;

	static constexpr uint8_t INDEX_BITS_SIZE = 7;
	static constexpr uint8_t BIT_SIZE = sizeof(CHIMP_TYPE) * 8;

	static inline void UnpackPackedData(uint16_t packed_data, UnpackedData &dest) {
		return PackedDataUtils<CHIMP_TYPE>::Unpack(packed_data, dest);
	}

	static inline CHIMP_TYPE Load(ChimpConstants::Flags flag, uint8_t leading_zeros[], uint32_t &leading_zero_index,
	                              UnpackedData unpacked_data[], uint32_t &unpacked_index, DecompressState &state,
	                              BitReader &input) {
		if (DUCKDB_UNLIKELY(state.first)) {
			return LoadFirst(state, input);
		} else {
			return DecompressValue(flag, leading_zeros, leading_zero_index, unpacked_data, unpacked_index, state,
			                       input);
		}
	}

	static inline CHIMP_TYPE LoadFirst(DecompressState &state, BitReader &input) {
		CHIMP_TYPE result = input.ReadValue<CHIMP_TYPE, sizeof(CHIMP_TYPE) * 8>();
		state.ring_buffer.template InsertScan<true>(result);
		state.first = false;
		state.reference_value = result;
		return result;
	}

	static inline CHIMP_TYPE DecompressValue(ChimpConstants::Flags flag, uint8_t leading_zeros[],
	                                         uint32_t &leading_zero_index, UnpackedData unpacked_data[],
	                                         uint32_t &unpacked_index, DecompressState &state, BitReader &input) {
		CHIMP_TYPE result;
		switch (flag) {
		case ChimpConstants::Flags::VALUE_IDENTICAL: {
			//! Value is identical to previous value
			auto index = input.ReadValue<uint8_t, 7>();
			result = UnsafeNumericCast<CHIMP_TYPE>(state.ring_buffer.Value(index));
			break;
		}
		case ChimpConstants::Flags::TRAILING_EXCEEDS_THRESHOLD: {
			const UnpackedData &unpacked = unpacked_data[unpacked_index++];
			state.leading_zeros = unpacked.leading_zero;
			state.trailing_zeros = BIT_SIZE - unpacked.significant_bits - state.leading_zeros;
			result = input.ReadValue<CHIMP_TYPE>(unpacked.significant_bits);
			result <<= state.trailing_zeros;
			result ^= state.ring_buffer.Value(unpacked.index);
			break;
		}
		case ChimpConstants::Flags::LEADING_ZERO_EQUALITY: {
			result = input.ReadValue<CHIMP_TYPE>(BIT_SIZE - state.leading_zeros);
			result ^= state.reference_value;
			break;
		}
		case ChimpConstants::Flags::LEADING_ZERO_LOAD: {
			state.leading_zeros = leading_zeros[leading_zero_index++];
			D_ASSERT(state.leading_zeros <= BIT_SIZE);
			result = input.ReadValue<CHIMP_TYPE>(BIT_SIZE - state.leading_zeros);
			result ^= state.reference_value;
			break;
		}
		default:
			throw InternalException("Chimp compression flag with value %d not recognized", flag);
		}
		state.reference_value = result;
		state.ring_buffer.InsertScan(result);
		return result;
	}
};

} // namespace duckdb
