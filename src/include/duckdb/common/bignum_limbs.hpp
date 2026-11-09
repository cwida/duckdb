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

//! Helpers that treat the big-endian magnitude bytes of a BIGNUM blob as 64-bit limbs. The blob format itself is
//! untouched: limbs are loaded with memcpy (the string_t payload is unaligned) and byte swapped on little-endian hosts.
struct BignumLimb {
	//! Bytes of a limb
	static constexpr idx_t BYTES = sizeof(uint64_t);

	//! The multiplication, division and decimal conversion kernels handle a magnitude as base 2^64 digits with 128-bit
	//! products where the compiler has a 128-bit integer type. MSVC and 32-bit targets have none and keep base 2^32
	//! digits with 64-bit products.
#if ((__GNUC__ >= 5) || defined(__clang__)) && defined(__SIZEOF_INT128__)
	using digit_t = uint64_t;
	__extension__ typedef unsigned __int128 twodigit_t;

	//! Returns the low 64 bits of a * b + add, and its high 64 bits in "high"; never overflows 128 bits
	static inline uint64_t MultiplyAdd(uint64_t a, uint64_t b, uint64_t add, uint64_t &high) {
		const twodigit_t product = static_cast<twodigit_t>(a) * b + add;
		high = static_cast<uint64_t>(product >> 64);
		return static_cast<uint64_t>(product);
	}

	//! Divides high * 2^64 + low by a divisor larger than high, so that the quotient fits a digit; sets the remainder
	static inline uint64_t DivRem(uint64_t high, uint64_t low, uint64_t divisor, uint64_t &remainder) {
		D_ASSERT(high < divisor);
#ifdef _MSC_VER
		// clang-cl has the 128-bit type but links no runtime support for dividing it
		return DivRemPortable(high, low, divisor, remainder);
#else
		const twodigit_t numerator = (static_cast<twodigit_t>(high) << 64) | low;
		const auto quotient = static_cast<uint64_t>(numerator / divisor);
		remainder = static_cast<uint64_t>(numerator - static_cast<twodigit_t>(quotient) * divisor);
		return quotient;
#endif
	}
#else
	using digit_t = uint32_t;
	using twodigit_t = uint64_t;

	//! Divides high * 2^32 + low by a divisor larger than high, so that the quotient fits a digit; sets the remainder
	static inline uint32_t DivRem(uint32_t high, uint32_t low, uint32_t divisor, uint32_t &remainder) {
		D_ASSERT(high < divisor);
		const twodigit_t numerator = (static_cast<twodigit_t>(high) << 32) | low;
		const auto quotient = static_cast<uint32_t>(numerator / divisor);
		remainder = static_cast<uint32_t>(numerator - static_cast<twodigit_t>(quotient) * divisor);
		return quotient;
	}

	//! Returns the low 64 bits of a * b + add, and its high 64 bits in "high"; never overflows 128 bits
	static inline uint64_t MultiplyAdd(uint64_t a, uint64_t b, uint64_t add, uint64_t &high) {
		// portable 32-bit split
		const uint64_t a_low = a & 0xFFFFFFFFULL;
		const uint64_t a_high = a >> 32;
		const uint64_t b_low = b & 0xFFFFFFFFULL;
		const uint64_t b_high = b >> 32;
		const uint64_t p0 = a_low * b_low;
		const uint64_t p1 = a_low * b_high;
		const uint64_t p2 = a_high * b_low;
		const uint64_t p3 = a_high * b_high;
		const uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
		uint64_t low = (p0 & 0xFFFFFFFFULL) | (middle << 32);
		high = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
		low += add;
		high += low < add ? 1 : 0;
		return low;
	}
#endif

	//! Bytes and bits of a multiplication / division digit
	static constexpr idx_t DIGIT_BYTES = sizeof(digit_t);
	static constexpr idx_t DIGIT_BITS = DIGIT_BYTES * 8;

	//! DivRem in two base 2^32 steps (Hacker's Delight divlu), for compilers without 128-bit division
	static inline uint64_t DivRemPortable(uint64_t high, uint64_t low, uint64_t divisor, uint64_t &remainder) {
		D_ASSERT(high < divisor);
		constexpr uint64_t HALF_BASE = static_cast<uint64_t>(1) << 32;
		constexpr uint64_t HALF_MASK = HALF_BASE - 1;
		// normalize so that the divisor has its top bit set; the quotient is unchanged, the remainder is scaled
		const idx_t shift = LeadingZeros(divisor);
		if (shift > 0) {
			divisor <<= shift;
			high = (high << shift) | (low >> (64 - shift));
			low <<= shift;
		}
		const uint64_t divisor_high = divisor >> 32;
		const uint64_t divisor_low = divisor & HALF_MASK;
		const uint64_t low_high = low >> 32;
		const uint64_t low_low = low & HALF_MASK;
		// the high half of the quotient: an estimate from the top halves that is corrected down until it is exact
		uint64_t q_high = high / divisor_high;
		uint64_t rhat = high - q_high * divisor_high;
		while (q_high >= HALF_BASE || q_high * divisor_low > ((rhat << 32) | low_high)) {
			q_high--;
			rhat += divisor_high;
			if (rhat >= HALF_BASE) {
				break;
			}
		}
		// the partial remainder is below the divisor, so the wrapping arithmetic yields the exact value
		const uint64_t middle = (high << 32) + low_high - q_high * divisor;
		uint64_t q_low = middle / divisor_high;
		rhat = middle - q_low * divisor_high;
		while (q_low >= HALF_BASE || q_low * divisor_low > ((rhat << 32) | low_low)) {
			q_low--;
			rhat += divisor_high;
			if (rhat >= HALF_BASE) {
				break;
			}
		}
		remainder = ((middle << 32) + low_low - q_low * divisor) >> shift;
		return (q_high << 32) | q_low;
	}

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

	//! Loads 8 magnitude bytes as a limb of the absolute value: the complement of a negative value is undone
	static inline uint64_t LoadMagnitude(const_data_ptr_t ptr, bool is_negative) {
		const uint64_t limb = LoadBE(ptr);
		return is_negative ? ~limb : limb;
	}

	//! Loads the count (1..8) most significant magnitude bytes of a value as a partial limb of the absolute value
	static inline uint64_t LoadMagnitudePartial(const_data_ptr_t ptr, idx_t count, bool is_negative) {
		const uint64_t limb = LoadBEPartial(ptr, count);
		if (!is_negative) {
			return limb;
		}
		return ~limb & (~uint64_t(0) >> (64 - 8 * count));
	}

	//! Stores a limb of the absolute value as 8 magnitude bytes, complemented for a negative value
	static inline void StoreMagnitude(data_ptr_t ptr, uint64_t value, bool is_negative) {
		StoreBE(ptr, is_negative ? ~value : value);
	}

	//! Stores the low count (1..8) bytes of a limb of the absolute value as magnitude bytes, complemented for a
	//! negative value
	static inline void StoreMagnitudePartial(data_ptr_t ptr, idx_t count, uint64_t value, bool is_negative) {
		StoreBEPartial(ptr, count, is_negative ? ~value : value);
	}

	//! Loads the 8 magnitude bytes ending just before end_idx as one limb holding the absolute value: bytes before
	//! the start of the value read as zero, bytes of a negative value are complemented
	static inline uint64_t LoadAbsolute(const_data_ptr_t data, bool is_negative, int64_t end_idx) {
		if (end_idx <= 0) {
			return 0;
		}
		if (end_idx >= static_cast<int64_t>(BYTES)) {
			return LoadMagnitude(data + end_idx - BYTES, is_negative);
		}
		// a partial limb at the most significant end of the value: only the low end_idx bytes are real
		return LoadMagnitudePartial(data, static_cast<idx_t>(end_idx), is_negative);
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
		if (byte_count > BYTES) {
			StoreMagnitudePartial(out, byte_count - BYTES, hi, is_negative);
			StoreMagnitude(out + byte_count - BYTES, lo, is_negative);
		} else {
			StoreMagnitudePartial(out, byte_count, lo, is_negative);
		}
	}
};

//! Base 2^64 (or 2^32, see BignumLimb) digits of the BIGNUM multiplication and division kernels
using bignum_digit_t = BignumLimb::digit_t;
using bignum_twodigit_t = BignumLimb::twodigit_t;

} // namespace duckdb
