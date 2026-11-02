//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/algebraic/covar.hpp
//
//
//===----------------------------------------------------------------------===//
// COVAR_POP(y,x)

#pragma once

#include "duckdb/common/operator/add.hpp"
#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

struct CovarState {
	static constexpr const char *STATE_NAMES[] = {"count", "meanx", "meany", "co_moment"};
	using STATE_TYPE = StructStateType<uint64_t, double, double, double>;

	uint64_t count;
	double meanx;
	double meany;
	double co_moment;
};

struct CovarOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &y, const B_TYPE &x, AggregateBinaryInput &idata) {
		// update running mean and d^2
		const double n = static_cast<double>(++(state.count));

		const double dx = (x - state.meanx);
		const double meanx = state.meanx + dx / n;

		const double dy = (y - state.meany);
		const double meany = state.meany + dy / n;

		//  Schubert and Gertz SSDBM 2018 (4.3)
		const double C = state.co_moment + dx * (y - meany);

		state.meanx = meanx;
		state.meany = meany;
		state.co_moment = C;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (target.count == 0) {
			target = source;
		} else if (source.count > 0) {
			uint64_t count;
			if (!TryAddOperator::Operation(target.count, source.count, count)) {
				throw OutOfRangeException("Overflow in aggregate state combine");
			}
			const auto target_count = static_cast<double>(target.count);
			const auto source_count = static_cast<double>(source.count);
			const auto total_count = static_cast<double>(count);
			const auto meanx = (source_count * source.meanx + target_count * target.meanx) / total_count;
			const auto meany = (source_count * source.meany + target_count * target.meany) / total_count;

			//  Schubert and Gertz SSDBM 2018, equation 21
			const auto deltax = target.meanx - source.meanx;
			const auto deltay = target.meany - source.meany;
			target.co_moment =
			    source.co_moment + target.co_moment + deltax * deltay * source_count * target_count / total_count;
			target.meanx = meanx;
			target.meany = meany;
			target.count = count;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct CovarPopOperation : public CovarOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = state.co_moment / state.count;
		}
	}
};

struct CovarSampOperation : public CovarOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count < 2) {
			finalize_data.ReturnNull();
		} else {
			target = state.co_moment / (state.count - 1);
		}
	}
};
} // namespace duckdb
