//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/aggregate_operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "duckdb/common/operator/comparison_operators.hpp"

namespace duckdb {

struct Min {
	template <class T>
	static inline T Operation(T left, T right) {
		return LessThan::Operation(left, right) ? left : right;
	}
};

struct Max {
	template <class T>
	static inline T Operation(T left, T right) {
		return GreaterThan::Operation(left, right) ? left : right;
	}
};

// Clang on AArch64 lowers this to fmax, which it can vectorize as a reduction
#if defined(__clang__) && defined(__aarch64__)
#if __has_builtin(__builtin_elementwise_maximum)
#define DUCKDB_NATIVE_FLOAT_MAX
#endif
#endif

//! NaN is ignored unless both inputs are NaN
template <class T>
inline T MinFloatingPoint(T left, T right) {
	if (std::isnan(left)) {
		return right;
	}
	if (std::isnan(right)) {
		return left;
	}
	return right > left ? left : right;
}

//! NaN propagates, since NaN sorts greater than everything (IEEE 754-2019 maximum)
template <class T>
inline T MaxFloatingPoint(T left, T right) {
#ifdef DUCKDB_NATIVE_FLOAT_MAX
	return __builtin_elementwise_maximum(left, right);
#else
	if (std::isnan(left)) {
		return left;
	}
	if (std::isnan(right)) {
		return right;
	}
	return right < left ? left : right;
#endif
}

template <>
inline float Min::Operation(float left, float right) {
	return MinFloatingPoint<float>(left, right);
}

template <>
inline double Min::Operation(double left, double right) {
	return MinFloatingPoint<double>(left, right);
}

template <>
inline float Max::Operation(float left, float right) {
	return MaxFloatingPoint<float>(left, right);
}

template <>
inline double Max::Operation(double left, double right) {
	return MaxFloatingPoint<double>(left, right);
}

struct LogicalAnd {
	template <class T>
	static inline T Operation(T left, T right) {
		return left && right;
	}
};

struct LogicalOr {
	template <class T>
	static inline T Operation(T left, T right) {
		return left || right;
	}
};

struct BitAnd {
	template <class T>
	static inline T Operation(T left, T right) {
		return left & right;
	}
};

struct BitOr {
	template <class T>
	static inline T Operation(T left, T right) {
		return left | right;
	}
};

struct BitXor {
	template <class T>
	static inline T Operation(T left, T right) {
		return left ^ right;
	}
};

} // namespace duckdb
