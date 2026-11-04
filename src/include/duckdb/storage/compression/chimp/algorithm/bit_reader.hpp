//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/algorithm/chimp/bit_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array_ptr.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

//! Every byte read touches at most 2 bytes (1 if it's perfectly aligned)
//! Within a byte we need to mask off the bits that we're interested in

struct BitReader {
private:
	//! Align the masks to the right
	static constexpr uint8_t MASKS[] = {
	    0,   // 0b00000000,
	    128, // 0b10000000,
	    192, // 0b11000000,
	    224, // 0b11100000,
	    240, // 0b11110000,
	    248, // 0b11111000,
	    252, // 0b11111100,
	    254, // 0b11111110,
	    255, // 0b11111111,
	    // These later masks are for the cases where index + SIZE exceeds 8
	    254, // 0b11111110,
	    252, // 0b11111100,
	    248, // 0b11111000,
	    240, // 0b11110000,
	    224, // 0b11100000,
	    192, // 0b11000000,
	    128, // 0b10000000,
	};

	static constexpr uint8_t REMAINDER_MASKS[] = {
	    0,   0, 0, 0, 0, 0, 0, 0, 0,
	    128, // 0b10000000,
	    192, // 0b11000000,
	    224, // 0b11100000,
	    240, // 0b11110000,
	    248, // 0b11111000,
	    252, // 0b11111100,
	    254, // 0b11111110,
	    255, // 0b11111111,
	};

public:
	explicit BitReader(unsafe_array_ptr<const uint8_t> input, uint8_t bit_offset = 0)
	    : input(input), index(bit_offset) {
		D_ASSERT(bit_offset < 8);
	}

	template <class T, uint8_t SIZE>
	inline T ReadValue() {
		static_assert(SIZE <= sizeof(T) * 8, "Bit width exceeds the result type");
		constexpr uint8_t BYTES = (SIZE >> 3);
		constexpr uint8_t REMAINDER = (SIZE & 7);
		return ReadBytes<T, BYTES>(REMAINDER);
	}

	template <class T>
	inline T ReadValue(const uint8_t &size) {
		D_ASSERT(size <= sizeof(T) * 8);
		const uint8_t bytes = size >> 3; // divide by 8;
		const uint8_t remainder = size & 7;
		return ReadBytes<T>(bytes, remainder);
	}

private:
	inline uint8_t BitIndex() const {
		return (index & 7);
	}
	inline idx_t ByteIndex() const {
		return (index >> 3);
	}

	inline uint8_t InnerReadByte(const uint8_t &offset) {
		const auto bit_index = BitIndex();
		const bool spill_to_next_byte = bit_index != 0;
		uint8_t result =
		    static_cast<uint8_t>(input[ByteIndex() + offset] << bit_index) |
		    ((input[ByteIndex() + offset + spill_to_next_byte] & REMAINDER_MASKS[8 + bit_index]) >> (8 - bit_index));
		return result;
	}

	//! index: 4
	//! size: 7
	//! input: [12345678][12345678]
	//! result:   [-AAAA  BBB]
	//!
	//! Result contains 4 bits from the first byte (making up the most significant bits)
	//! And 3 bits from the second byte (the least significant bits)
	inline uint8_t InnerRead(const uint8_t &size, const uint8_t &offset) {
		const uint8_t right_shift = 8 - size;
		const uint8_t bit_remainder = (8 - ((size + BitIndex()) - 8)) & 7;
		// The least significant bits are positioned at the far right of the byte

		// Create a mask given the size and index
		// Take the first byte
		// Left-shift it by index, to line up the bits we're interested in with the mask
		// Get the mask for the given size
		// Bit-wise AND the byte and the mask together
		// Right-shift this result (the most significant bits)

		// Sometimes we will need to read from the second byte
		// But to make this branchless, we will read from the current byte as a no-op if this condition is not true
		// SPILL = (index + size > 8)
		//
		// If SPILL is true:
		// The REMAINDER_MASKS gives us the mask for the bits we're interested in
		// We bit-wise AND these together (no need to shift anything because the index is essentially zero for this new
		// byte) And we then right-shift these bits in place (to the right of the previous bits)
		const bool spill_to_next_byte = (size + BitIndex() > 8);
		uint8_t result =
		    ((input[ByteIndex() + offset] << BitIndex()) & MASKS[size]) >> right_shift |
		    ((input[ByteIndex() + offset + spill_to_next_byte] & REMAINDER_MASKS[size + BitIndex()]) >> bit_remainder);
		return result;
	}

	template <class T, uint8_t BYTES>
	inline T ReadBytes(const uint8_t &remainder) {
		T result = 0;
		if (BYTES > 0) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(0));
		}
		if (BYTES > 1) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(1));
		}
		if (BYTES > 2) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(2));
		}
		if (BYTES > 3) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(3));
		}
		if (BYTES > 4) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(4));
		}
		if (BYTES > 5) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(5));
		}
		if (BYTES > 6) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(6));
		}
		if (BYTES > 7) {
			result = UnsafeNumericCast<T>(result << 8 | InnerReadByte(7));
		}
		if (remainder > 0) {
			result = UnsafeNumericCast<T>(result << remainder | InnerRead(remainder, BYTES));
		}
		index += (BYTES << 3) + remainder;
		return result;
	}

	template <class T>
	inline T ReadBytes(const uint8_t &bytes, const uint8_t &remainder) {
		T result = 0;
		for (uint8_t i = 0; i < bytes; i++) {
			result = result << 8 | InnerReadByte(i);
		}
		if (DUCKDB_LIKELY(remainder > 0)) {
			result = result << remainder | InnerRead(remainder, bytes);
		}
		index += static_cast<idx_t>(bytes << 3) + remainder;
		return result;
	}

	unsafe_array_ptr<const uint8_t> input;
	idx_t index;
};

} // namespace duckdb
