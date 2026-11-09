#include "duckdb/common/operator/add.hpp"
#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

namespace {

//! Online central moments (Terriberry / Pébay, DOI: 10.2172/1028931), the higher-moment extension of Welford's
//! method used by var_samp: M2..M4 are exactly zero for constant input instead of rounding noise of power sums
struct KurtosisState {
	static constexpr const char *STATE_NAMES[] = {"n", "mean", "m2", "m3", "m4"};
	using STATE_TYPE = StructStateType<idx_t, double, double, double, double>;

	idx_t n;
	double mean; //  M1
	double m2;   //  M2 = sum((x - mean)^2)
	double m3;   //  M3 = sum((x - mean)^3)
	double m4;   //  M4 = sum((x - mean)^4)
};

struct KurtosisFlagBiasCorrection {};

struct KurtosisFlagNoBiasCorrection {};

template <class KURTOSIS_FLAG>
struct KurtosisOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		// a block of identical values has mean "input" and zero central moments
		STATE constant;
		constant.n = count;
		constant.mean = static_cast<double>(input);
		constant.m2 = 0;
		constant.m3 = 0;
		constant.m4 = 0;
		Combine<STATE, OP>(constant, state, unary_input.input);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		const double n1 = static_cast<double>(state.n);
		state.n++;
		const double n = static_cast<double>(state.n);
		const double delta = static_cast<double>(input) - state.mean;
		const double delta_n = delta / n;
		const double delta_n2 = delta_n * delta_n;
		const double term1 = delta * delta_n * n1;
		state.mean += delta_n;
		state.m4 += term1 * delta_n2 * (n * n - 3 * n + 3) + 6 * delta_n2 * state.m2 - 4 * delta_n * state.m3;
		state.m3 += term1 * delta_n * (n - 2) - 3 * delta_n * state.m2;
		state.m2 += term1;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.n == 0) {
			return;
		}
		if (target.n == 0) {
			target = source;
			return;
		}
		idx_t count;
		if (!TryAddOperator::Operation(target.n, source.n, count)) {
			throw OutOfRangeException("Overflow in aggregate state combine");
		}
		const double na = static_cast<double>(target.n);
		const double nb = static_cast<double>(source.n);
		const double n = static_cast<double>(count);
		const double delta = source.mean - target.mean;
		const double delta_n = delta / n;
		const double delta_n2 = delta_n * delta_n;
		target.m4 += source.m4 + delta * delta_n * delta_n2 * na * nb * (na * na - na * nb + nb * nb) +
		             6 * delta_n2 * (na * na * source.m2 + nb * nb * target.m2) +
		             4 * delta_n * (na * source.m3 - nb * target.m3);
		target.m3 +=
		    source.m3 + delta * delta_n2 * na * nb * (na - nb) + 3 * delta_n * (na * source.m2 - nb * target.m2);
		target.m2 += source.m2 + delta * delta_n * na * nb;
		target.mean += delta_n * nb;
		target.n = count;
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(STATE &state, TARGET_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (state.n <= 1) {
			finalize_data.ReturnNull();
			return;
		}
		if (std::is_same<KURTOSIS_FLAG, KurtosisFlagBiasCorrection>::value && state.n <= 3) {
			finalize_data.ReturnNull();
			return;
		}
		// kurtosis is undefined for zero variance (M2 is exactly 0 for constant input)
		if (state.m2 <= 0) {
			finalize_data.ReturnNull();
			return;
		}
		const double n = static_cast<double>(state.n);
		// m4 / m2^2 with the population moments m4 = M4 / n and m2 = M2 / n
		const double m4_over_m2_sqr = n * (state.m4 / state.m2) / state.m2;
		if (std::is_same<KURTOSIS_FLAG, KurtosisFlagNoBiasCorrection>::value) {
			target = m4_over_m2_sqr - 3;
		} else {
			target = (n - 1) * ((n + 1) * m4_over_m2_sqr - 3 * (n - 1)) / ((n - 2) * (n - 3));
		}
		if (!Value::DoubleIsFinite(target)) {
			throw OutOfRangeException("Kurtosis is out of range!");
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction KurtosisFun::GetFunction() {
	auto result =
	    AggregateFunction::UnaryAggregate<KurtosisState, double, double, KurtosisOperation<KurtosisFlagBiasCorrection>>(
	        LogicalType::DOUBLE, LogicalType::DOUBLE);
	result.GetSignature().GetParameter(0).SetName("x");
	result.SetFallible();
	return result;
}

AggregateFunction KurtosisPopFun::GetFunction() {
	auto result = AggregateFunction::UnaryAggregate<KurtosisState, double, double,
	                                                KurtosisOperation<KurtosisFlagNoBiasCorrection>>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	result.GetSignature().GetParameter(0).SetName("x");
	result.SetFallible();
	return result;
}

} // namespace duckdb
