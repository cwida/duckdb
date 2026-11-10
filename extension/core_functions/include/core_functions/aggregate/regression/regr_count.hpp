//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/regression/regr_count.hpp
//
//
//===----------------------------------------------------------------------===//
// REGR_COUNT(y, x)

#pragma once

#include "duckdb/common/operator/add.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "core_functions/aggregate/algebraic/covar.hpp"
#include "core_functions/aggregate/algebraic/stddev.hpp"

namespace duckdb {

struct RegrCountFunction {
	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!TryAddOperator::Operation(target, source, target)) {
			throw OutOfRangeException("Overflow in aggregate state combine");
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state > NumericLimits<T>::Maximum()) {
			throw OutOfRangeException("regr_count result %llu does not fit in the result type", state);
		}
		target = static_cast<T>(state);
	}
	static bool IgnoreNull() {
		return true;
	}
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &, const B_TYPE &, AggregateBinaryInput &) {
		state += 1;
	}
};

} // namespace duckdb
