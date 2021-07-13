#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

struct KurtosisState {
	idx_t n;
	double sum;
	double sum_sqr;
	double sum_cub;
	double sum_four;
};

struct KurtosisOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->n = 0;
		state->sum = state->sum_sqr = state->sum_cub = state->sum_four = 0.0;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *data, ValidityMask &mask, idx_t idx) {
		state->n++;
		state->sum += data[idx];
		state->sum_sqr += pow(data[idx], 2);
		state->sum_cub += pow(data[idx], 3);
		state->sum_four += pow(data[idx], 4);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (source.n == 0) {
			return;
		}
		target->n += source.n;
		target->sum += source.sum;
		target->sum_sqr += source.sum_sqr;
		target->sum_cub += source.sum_cub;
		target->sum_four += source.sum_four;
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(Vector &result, FunctionData *bind_data, STATE *state, TARGET_TYPE *target, ValidityMask &mask,
	                     idx_t idx) {
		auto n = (double)state->n;
		if (n <= 3) {
			mask.SetInvalid(idx);
			return;
		}
		double temp = 1 / n;
		//! This is necessary due to linux 32 bits
		long double temp_aux = 1 / n;
		if (state->sum_sqr - state->sum * state->sum * temp == 0 ||
		    state->sum_sqr - state->sum * state->sum * temp_aux == 0) {
			mask.SetInvalid(idx);
			return;
		}
		double m4 =
		    temp * (state->sum_four - 4 * state->sum_cub * state->sum * temp +
		            6 * state->sum_sqr * state->sum * state->sum * temp * temp - 3 * pow(state->sum, 4) * pow(temp, 3));

		double m2 = temp * (state->sum_sqr - state->sum * state->sum * temp);
		if (((m2 * m2) - 3 * (n - 1)) == 0 || ((n - 2) * (n - 3)) == 0) { // LCOV_EXCL_START
			mask.SetInvalid(idx);
		} // LCOV_EXCL_STOP
		target[idx] = (n - 1) * ((n + 1) * m4 / (m2 * m2) - 3 * (n - 1)) / ((n - 2) * (n - 3));
		if (!Value::DoubleIsValid(target[idx])) {
			mask.SetInvalid(idx);
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

void KurtosisFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet function_set("kurtosis");
	function_set.AddFunction(AggregateFunction::UnaryAggregate<KurtosisState, double, double, KurtosisOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE));
	set.AddFunction(function_set);
}

} // namespace duckdb
