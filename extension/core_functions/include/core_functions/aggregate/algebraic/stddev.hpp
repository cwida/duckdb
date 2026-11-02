//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/algebraic/stddev.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/operator/add.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include <complex>
#include <cmath>

namespace duckdb {

struct StddevState {
	static constexpr const char *STATE_NAMES[] = {"count", "mean", "dsquared"};
	using STATE_TYPE = StructStateType<uint64_t, double, double>;

	uint64_t count;  //  n
	double mean;     //  M1
	double dsquared; //  M2
};

// Streaming approximate standard deviation using Welford's
// method, DOI: 10.2307/1266577
struct STDDevBaseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, const INPUT_TYPE &input) {
		// update running mean and d^2
		state.count++;
		const double mean_differential = (input - state.mean) / state.count;
		const double new_mean = state.mean + mean_differential;
		const double dsquared_increment = (input - new_mean) * (input - state.mean);
		const double new_dsquared = state.dsquared + dsquared_increment;

		state.mean = new_mean;
		state.dsquared = new_dsquared;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		Execute(state, input);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (target.count == 0) {
			target = source;
		} else if (source.count > 0) {
			uint64_t count;
			if (!TryAddOperator::Operation(target.count, source.count, count)) {
				OutOfRangeException::Throw("Overflow in aggregate state combine");
			}
			const double target_count = static_cast<double>(target.count);
			const double source_count = static_cast<double>(source.count);
			const double total_count = static_cast<double>(count);
			const auto delta = source.mean - target.mean;
			const auto mean = std::fma(source_count / total_count, delta, target.mean);
			target.dsquared =
			    source.dsquared + target.dsquared + delta * delta * source_count * target_count / total_count;
			target.mean = mean;
			target.count = count;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct VarSampOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count <= 1) {
			finalize_data.ReturnNull();
		} else {
			target = state.dsquared / (state.count - 1);
		}
	}
};

struct VarPopOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = state.count > 1 ? (state.dsquared / state.count) : 0;
		}
	}
};

struct STDDevSampOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count <= 1) {
			finalize_data.ReturnNull();
		} else {
			target = sqrt(state.dsquared / (state.count - 1));
		}
	}
};

struct STDDevPopOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = state.count > 1 ? sqrt(state.dsquared / state.count) : 0;
		}
	}
};

struct StandardErrorOfTheMeanOperation : public STDDevBaseOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = sqrt(state.dsquared / state.count) / sqrt((state.count));
		}
	}
};
} // namespace duckdb
