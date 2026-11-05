//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/numeric_binary_operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/limits.hpp"
#include <cmath>

namespace duckdb {

struct DivideOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		D_ASSERT(right != 0); // this should be checked before!
		return left / right;
	}
};

struct TryDivideOperator {
	template <class T>
	static inline bool Operation(T left, T right, T &result) {
		D_ASSERT(right != 0); // this should be checked before!
		// with a non-zero divisor, division only overflows for the minimum value divided by -1
		if constexpr (NumericLimits<T>::IsSigned()) {
			if (left == NumericLimits<T>::Minimum() && right == T(-1)) {
				return false;
			}
		}
		result = DivideOperator::Operation<T, T, T>(left, right);
		return true;
	}
};

struct ModuloOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		D_ASSERT(right != 0);
		return left % right;
	}
};

template <>
float DivideOperator::Operation(float left, float right);
template <>
double DivideOperator::Operation(double left, double right);
template <>
hugeint_t DivideOperator::Operation(hugeint_t left, hugeint_t right);
template <>
interval_t DivideOperator::Operation(interval_t left, int64_t right);

template <>
float ModuloOperator::Operation(float left, float right);
template <>
double ModuloOperator::Operation(double left, double right);
template <>
hugeint_t ModuloOperator::Operation(hugeint_t left, hugeint_t right);

} // namespace duckdb
