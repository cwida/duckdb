#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

namespace {

//! Online central moments (Terriberry / Pébay, DOI: 10.2172/1028931), the higher-moment extension of Welford's
//! method used by var_samp: M2 and M3 are exactly zero for constant input instead of rounding noise of power sums
struct SkewState {
	static constexpr const char *STATE_NAMES[] = {"n", "mean", "m2", "m3"};
	using STATE_TYPE = StructStateType<idx_t, double, double, double>;

	idx_t n;
	double mean; //  M1
	double m2;   //  M2 = sum((x - mean)^2)
	double m3;   //  M3 = sum((x - mean)^3)
};

struct SkewnessOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		// a block of identical values has mean "input" and zero central moments
		STATE constant;
		constant.n = count;
		constant.mean = static_cast<double>(input);
		constant.m2 = 0;
		constant.m3 = 0;
		Combine<STATE, OP>(constant, state, unary_input.input);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		const double n1 = static_cast<double>(state.n);
		state.n++;
		const double n = static_cast<double>(state.n);
		const double delta = static_cast<double>(input) - state.mean;
		const double delta_n = delta / n;
		const double term1 = delta * delta_n * n1;
		state.mean += delta_n;
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
		const auto count = target.n + source.n;
		D_ASSERT(count >= target.n); // This is a check that we are not overflowing
		const double na = static_cast<double>(target.n);
		const double nb = static_cast<double>(source.n);
		const double n = static_cast<double>(count);
		const double delta = source.mean - target.mean;
		const double delta_n = delta / n;
		target.m3 += source.m3 + delta * delta_n * delta_n * na * nb * (na - nb) +
		             3 * delta_n * (na * source.m2 - nb * target.m2);
		target.m2 += source.m2 + delta * delta_n * na * nb;
		target.mean += delta_n * nb;
		target.n = count;
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(STATE &state, TARGET_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (state.n <= 2) {
			finalize_data.ReturnNull();
			return;
		}
		// skewness is undefined for zero variance (M2 is exactly 0 for constant input)
		if (state.m2 <= 0) {
			finalize_data.ReturnNull();
			return;
		}
		const double n = static_cast<double>(state.n);
		// g1 = (M3 / n) / (M2 / n)^1.5 = sqrt(n) * M3 / M2^1.5, with the bias correction sqrt(n (n - 1)) / (n - 2)
		const double g1 = std::sqrt(n) * (state.m3 / state.m2) / std::sqrt(state.m2);
		target = std::sqrt(n * (n - 1)) / (n - 2) * g1;
		if (!Value::DoubleIsFinite(target)) {
			throw OutOfRangeException("SKEW is out of range!");
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction SkewnessFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<SkewState, double, double, SkewnessOperation>(LogicalType::DOUBLE,
	                                                                                           LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("x");
	fun.SetFallible(); // finalize throws when the skewness is not finite
	return fun;
}

} // namespace duckdb
