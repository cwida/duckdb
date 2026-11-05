//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/chimp/leading_zero_buffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array_ptr.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#ifdef DEBUG
#include "duckdb/common/vector.hpp"
#include "duckdb/common/assert.hpp"
#endif

namespace duckdb {

//! This class is in charge of storing the leading_zero_bits, which are of a fixed size
//! These are packed together so that the rest of the data can be byte-aligned
//! The leading zero bit data is read from left to right

struct LeadingZeroBufferConstants {
	static constexpr uint32_t MASKS[8] = {
	    7,        // 0b 00000000 00000000 00000000 00000111,
	    56,       // 0b 00000000 00000000 00000000 00111000,
	    448,      // 0b 00000000 00000000 00000001 11000000,
	    3584,     // 0b 00000000 00000000 00001110 00000000,
	    28672,    // 0b 00000000 00000000 01110000 00000000,
	    229376,   // 0b 00000000 00000011 10000000 00000000,
	    1835008,  // 0b 00000000 00011100 00000000 00000000,
	    14680064, // 0b 00000000 11100000 00000000 00000000,
	};

	// We're not using the last byte (the most significant) of the 4 bytes we're accessing
	static constexpr uint8_t SHIFTS[8] = {0, 3, 6, 9, 12, 15, 18, 21};
};

template <bool EMPTY>
class LeadingZeroBuffer {
public:
	static constexpr uint32_t CHIMP_GROUP_SIZE = 1024;
	static constexpr uint32_t LEADING_ZERO_BITS_SIZE = 3;
	static constexpr uint32_t LEADING_ZERO_BLOCK_SIZE = 8;
	static constexpr uint32_t LEADING_ZERO_BLOCK_BIT_SIZE = LEADING_ZERO_BLOCK_SIZE * LEADING_ZERO_BITS_SIZE;
	static constexpr uint32_t MAX_LEADING_ZERO_BLOCKS = CHIMP_GROUP_SIZE / LEADING_ZERO_BLOCK_SIZE;
	static constexpr uint32_t MAX_BITS_USED_BY_ZERO_BLOCKS = MAX_LEADING_ZERO_BLOCKS * LEADING_ZERO_BLOCK_BIT_SIZE;
	static constexpr uint32_t MAX_BYTES_USED_BY_ZERO_BLOCKS = MAX_BITS_USED_BY_ZERO_BLOCKS / 8;
	static constexpr uint32_t BUFFER_SIZE = MAX_BYTES_USED_BY_ZERO_BLOCKS;

public:
	explicit LeadingZeroBuffer(unsafe_array_ptr<const uint8_t> buffer)
	    : current(0), counter(0), read_buffer(buffer), write_buffer(nullptr) {
	}
	void SetBuffer(uint8_t *buffer) {
		// Set the internal buffer, when inserting this should be BUFFER_SIZE bytes in length
		// This buffer does not need to be zero-initialized for inserting
		write_buffer = buffer;
		this->counter = 0;
	}
	void Flush() {
		if ((counter & 7) != 0) {
			FlushBuffer();
		}
	}

	uint64_t BitsWritten() const {
		return counter * 3ULL;
	}

	// Reset the counter, but don't replace the buffer
	void Reset() {
		this->counter = 0;
		current = 0;
#ifdef DEBUG
		flags.clear();
#endif
	}

public:
#ifdef DEBUG
	uint8_t ExtractValue(uint32_t value, uint8_t index) {
		return NumericCast<uint8_t>((value & LeadingZeroBufferConstants::MASKS[index]) >>
		                            LeadingZeroBufferConstants::SHIFTS[index]);
	}
#endif

	inline uint64_t BlockIndex() const {
		return ((counter >> 3ULL) * (LEADING_ZERO_BLOCK_BIT_SIZE / 8ULL));
	}

	void FlushBuffer() {
		if (EMPTY) {
			return;
		}
		const auto buffer_idx = BlockIndex();
		memcpy((void *)(write_buffer + buffer_idx), (uint8_t *)&current, 3);
#ifdef DEBUG
		// Verify that the bits are copied correctly

		uint32_t temp_value = 0;
		memcpy(reinterpret_cast<uint8_t *>(&temp_value), (void *)(write_buffer + buffer_idx), 3);
		for (idx_t i = 0; i < flags.size(); i++) {
			D_ASSERT(flags[i] == ExtractValue(temp_value, i));
		}
		flags.clear();
#endif
	}

	void Insert(const uint8_t &value) {
		if (!EMPTY) {
#ifdef DEBUG
			flags.push_back(value);
#endif
			current |= (value & 7) << LeadingZeroBufferConstants::SHIFTS[counter & 7];
#ifdef DEBUG
			// Verify that the bits are serialized correctly
			D_ASSERT(flags[counter & 7] == ExtractValue(current, counter & 7));
#endif

			if ((counter & (LEADING_ZERO_BLOCK_SIZE - 1)) == 7) {
				FlushBuffer();
				current = 0;
			}
		}
		counter++;
	}

	inline uint8_t Extract() {
		const auto buffer_idx = BlockIndex();
		uint32_t temp = 0;
		memcpy(&temp, read_buffer.data() + buffer_idx, 3);

		const uint8_t result = UnsafeNumericCast<uint8_t>((temp & LeadingZeroBufferConstants::MASKS[counter & 7]) >>
		                                                  LeadingZeroBufferConstants::SHIFTS[counter & 7]);
		counter++;
		return result;
	}
	idx_t GetCount() const {
		return counter;
	}
	idx_t BlockCount() const {
		return (counter >> 3) + ((counter & 7) != 0);
	}

private:
private:
	uint32_t current;
	uint32_t counter = 0; // block_index * 8
	unsafe_array_ptr<const uint8_t> read_buffer;
	uint8_t *write_buffer;
#ifdef DEBUG
	vector<uint8_t> flags;
#endif
};

} // namespace duckdb
