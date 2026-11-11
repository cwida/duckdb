//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/bit_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

template <class T>
struct CountZeros {};

template <>
struct CountZeros<uint64_t> {
	// see here: https://en.wikipedia.org/wiki/De_Bruijn_sequence
	inline static idx_t Leading(const uint64_t value_in) {
		if (!value_in) {
			return 64;
		}

		uint64_t value = value_in;

		static constexpr uint64_t index64msb[] = {0,  47, 1,  56, 48, 27, 2,  60, 57, 49, 41, 37, 28, 16, 3,  61,
		                                          54, 58, 35, 52, 50, 42, 21, 44, 38, 32, 29, 23, 17, 11, 4,  62,
		                                          46, 55, 26, 59, 40, 36, 15, 53, 34, 51, 20, 43, 31, 22, 10, 45,
		                                          25, 39, 14, 33, 19, 30, 9,  24, 13, 18, 8,  12, 7,  6,  5,  63};

		constexpr uint64_t debruijn64msb = 0X03F79D71B4CB0A89;

		value |= value >> 1;
		value |= value >> 2;
		value |= value >> 4;
		value |= value >> 8;
		value |= value >> 16;
		value |= value >> 32;
		auto result = 63 - index64msb[(value * debruijn64msb) >> 58];
#ifdef __clang__
		D_ASSERT(result == static_cast<uint64_t>(__builtin_clzll(value_in)));
#endif
		return result;
	}
	inline static idx_t Trailing(uint64_t value_in) {
		if (!value_in) {
			return 64;
		}
#if defined(__GNUC__) || defined(__clang__)
		return static_cast<idx_t>(__builtin_ctzll(value_in));
#else
		uint64_t value = value_in;

		static constexpr uint64_t index64lsb[] = {63, 0,  58, 1,  59, 47, 53, 2,  60, 39, 48, 27, 54, 33, 42, 3,
		                                          61, 51, 37, 40, 49, 18, 28, 20, 55, 30, 34, 11, 43, 14, 22, 4,
		                                          62, 57, 46, 52, 38, 26, 32, 41, 50, 36, 17, 19, 29, 10, 13, 21,
		                                          56, 45, 25, 31, 35, 16, 9,  12, 44, 24, 15, 8,  23, 7,  6,  5};
		constexpr uint64_t debruijn64lsb = 0x07EDD5E59A4E28C2ULL;
		auto result = index64lsb[((value & -value) * debruijn64lsb) >> 58];
		return result;
#endif
	}
};

template <>
struct CountZeros<uint32_t> {
	inline static idx_t Leading(uint32_t value) {
		return CountZeros<uint64_t>::Leading(static_cast<uint64_t>(value)) - 32;
	}
	inline static idx_t Trailing(uint32_t value) {
		return CountZeros<uint64_t>::Trailing(static_cast<uint64_t>(value));
	}
};

template <>
struct CountZeros<hugeint_t> {
	inline static idx_t Leading(hugeint_t value) {
		const uint64_t upper = static_cast<uint64_t>(value.upper);
		const uint64_t lower = value.lower;

		if (upper) {
			return CountZeros<uint64_t>::Leading(upper);
		} else if (lower) {
			return 64 + CountZeros<uint64_t>::Leading(lower);
		} else {
			return 128;
		}
	}

	inline static idx_t Trailing(hugeint_t value) {
		const uint64_t upper = static_cast<uint64_t>(value.upper);
		const uint64_t lower = value.lower;

		if (lower) {
			return CountZeros<uint64_t>::Trailing(lower);
		} else if (upper) {
			return 64 + CountZeros<uint64_t>::Trailing(upper);
		} else {
			return 128;
		}
	}
};

template <>
struct CountZeros<uhugeint_t> {
	inline static idx_t Leading(uhugeint_t value) {
		const uint64_t upper = static_cast<uint64_t>(value.upper);
		const uint64_t lower = value.lower;

		if (upper) {
			return CountZeros<uint64_t>::Leading(upper);
		} else if (lower) {
			return 64 + CountZeros<uint64_t>::Leading(lower);
		} else {
			return 128;
		}
	}

	inline static idx_t Trailing(uhugeint_t value) {
		const uint64_t upper = static_cast<uint64_t>(value.upper);
		const uint64_t lower = value.lower;

		if (lower) {
			return CountZeros<uint64_t>::Trailing(lower);
		} else if (upper) {
			return 64 + CountZeros<uint64_t>::Trailing(upper);
		} else {
			return 128;
		}
	}
};

} // namespace duckdb
