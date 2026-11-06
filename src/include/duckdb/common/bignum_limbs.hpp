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
	static constexpr idx_t BYTES = sizeof(uint64_t);

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

	//! The number of leading zero bits of a non-zero limb
	static inline idx_t LeadingZeros(uint64_t value) {
		D_ASSERT(value != 0);
#if defined(__GNUC__) || defined(__clang__)
		return static_cast<idx_t>(__builtin_clzll(value));
#else
		return CountZeros<uint64_t>::Leading(value);
#endif
	}

	//! The number of bytes needed to hold a magnitude, at least one
	static inline idx_t ByteCount(uint64_t magnitude) {
		if (magnitude == 0) {
			return 1;
		}
		return (64 - LeadingZeros(magnitude) + 7) / 8;
	}
};

} // namespace duckdb
