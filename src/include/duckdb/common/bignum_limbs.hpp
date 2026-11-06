//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/bignum_limbs.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/common/typedefs.hpp"

#include <cstring>

namespace duckdb {

//! The multiplication and division kernels handle a BIGNUM magnitude as base 2^64 digits with 128-bit products.
//! MSVC has no 128-bit integer type, so it keeps base 2^32 digits with 64-bit products.
#if defined(_MSC_VER) && !defined(__clang__)
using bignum_digit_t = uint32_t;
using bignum_twodigit_t = uint64_t;
#else
using bignum_digit_t = uint64_t;
__extension__ typedef unsigned __int128 bignum_twodigit_t;
#endif

//! Helpers that treat the big-endian magnitude bytes of a BIGNUM blob as 64-bit limbs. The blob format itself is
//! untouched: limbs are loaded with memcpy (the string_t payload is unaligned) and byte swapped on little-endian hosts.
struct BignumLimb {
	static constexpr idx_t BYTES = sizeof(uint64_t);
	//! Bytes and bits of a multiplication / division digit
	static constexpr idx_t DIGIT_BYTES = sizeof(bignum_digit_t);
	static constexpr idx_t DIGIT_BITS = DIGIT_BYTES * 8;

	template <class T>
	static inline T ToHostOrder(T value) {
#if DUCKDB_IS_BIG_ENDIAN
		return value;
#else
		return BSwap(value);
#endif
	}

	//! Loads 8 big-endian bytes as an integer
	static inline uint64_t LoadBE(const_data_ptr_t ptr) {
		uint64_t value;
		memcpy(&value, ptr, BYTES);
		return ToHostOrder(value);
	}

	//! Stores an integer as 8 big-endian bytes
	static inline void StoreBE(data_ptr_t ptr, uint64_t value) {
		value = ToHostOrder(value);
		memcpy(ptr, &value, BYTES);
	}

	//! Loads the count (1..8) big-endian bytes at ptr as an integer, i.e. a partial limb at the most significant end
	static inline uint64_t LoadBEPartial(const_data_ptr_t ptr, idx_t count) {
		D_ASSERT(count >= 1 && count <= BYTES);
		if (count == BYTES) {
			return LoadBE(ptr);
		}
		// fixed-size loads keep this free of variable-length memcpy calls
		uint64_t value = 0;
		if (count & 4) {
			uint32_t part;
			memcpy(&part, ptr, sizeof(part));
			value = ToHostOrder(part);
			ptr += sizeof(part);
		}
		if (count & 2) {
			uint16_t part;
			memcpy(&part, ptr, sizeof(part));
			value = (value << 16) | ToHostOrder(part);
			ptr += sizeof(part);
		}
		if (count & 1) {
			value = (value << 8) | *ptr;
		}
		return value;
	}

	//! Stores the low count (1..8) bytes of an integer as big-endian bytes at ptr
	static inline void StoreBEPartial(data_ptr_t ptr, idx_t count, uint64_t value) {
		D_ASSERT(count >= 1 && count <= BYTES);
		if (count == BYTES) {
			StoreBE(ptr, value);
			return;
		}
		if (count & 4) {
			const uint32_t part = ToHostOrder(static_cast<uint32_t>(value >> (8 * (count - 4))));
			memcpy(ptr, &part, sizeof(part));
			ptr += sizeof(part);
		}
		if (count & 2) {
			const uint16_t part = ToHostOrder(static_cast<uint16_t>(value >> (8 * (count & 1))));
			memcpy(ptr, &part, sizeof(part));
			ptr += sizeof(part);
		}
		if (count & 1) {
			*ptr = static_cast<data_t>(value);
		}
	}

	//! Loads the 8 magnitude bytes ending just before end_idx as one limb holding the absolute value: bytes before
	//! the start of the value read as zero, bytes of a negative value are complemented
	static inline uint64_t LoadAbsolute(const_data_ptr_t data, bool is_negative, int64_t end_idx) {
		if (end_idx <= 0) {
			return 0;
		}
		if (end_idx >= static_cast<int64_t>(BYTES)) {
			const uint64_t limb = LoadBE(data + end_idx - BYTES);
			return is_negative ? ~limb : limb;
		}
		// a partial limb at the most significant end of the value: only the low end_idx bytes are real
		const uint64_t limb = LoadBEPartial(data, static_cast<idx_t>(end_idx));
		const uint64_t mask = (uint64_t(1) << (8 * end_idx)) - 1;
		return is_negative ? (~limb & mask) : limb;
	}

	//! The number of leading zero bits of a non-zero limb
	static inline idx_t LeadingZeros(uint64_t value) {
		D_ASSERT(value != 0);
#if defined(__GNUC__) || defined(__clang__)
		return static_cast<idx_t>(__builtin_clzll(value));
#else
		return CountZeros<uint64_t>::Leading(value);
#endif
	}
	static inline idx_t LeadingZeros(uint32_t value) {
		D_ASSERT(value != 0);
		return CountZeros<uint32_t>::Leading(value);
	}

	//! The number of bytes needed to hold a magnitude, at least one
	static inline idx_t ByteCount(uint64_t magnitude) {
		if (magnitude == 0) {
			return 1;
		}
		return (64 - LeadingZeros(magnitude) + 7) / 8;
	}

	//! The number of bytes needed to hold a two-limb magnitude, at least one
	static inline idx_t ByteCount(uint64_t hi, uint64_t lo) {
		return hi ? BYTES + ByteCount(hi) : ByteCount(lo);
	}

	//! Writes a two-limb magnitude as byte_count (1..16, see ByteCount) big-endian BIGNUM data bytes, complemented
	//! for a negative value
	static inline void StoreMagnitude(data_ptr_t out, idx_t byte_count, uint64_t hi, uint64_t lo, bool is_negative) {
		D_ASSERT(byte_count >= 1 && byte_count <= 2 * BYTES);
		const uint64_t flip = is_negative ? ~uint64_t(0) : 0;
		if (byte_count > BYTES) {
			StoreBEPartial(out, byte_count - BYTES, hi ^ flip);
			StoreBE(out + byte_count - BYTES, lo ^ flip);
		} else {
			StoreBEPartial(out, byte_count, lo ^ flip);
		}
	}
};

} // namespace duckdb
