//! AVG(y)-REGR_SLOPE(y,x)*AVG(x)

#include "core_functions/aggregate/regression_functions.hpp"
#include "core_functions/aggregate/regression/regr_slope.hpp"
#include "core_functions/aggregate/algebraic_functions.hpp"

namespace duckdb {

namespace {
struct RegrInterceptState {
	static constexpr const char *STATE_NAMES[] = {"count", "sum_x", "sum_y", "slope"};
	using STATE_TYPE = StructStateType<uint64_t, double, double, RegrSlopeState>;

	uint64_t count;
	double sum_x;
	double sum_y;
	RegrSlopeState slope;
};

struct RegrInterceptOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &y, const B_TYPE &x, AggregateBinaryInput &idata) {
		state.count++;
		state.sum_x += x;
		state.sum_y += y;
		RegrSlopeOperation::Operation<A_TYPE, B_TYPE, RegrSlopeState, OP>(state.slope, y, x, idata);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		target.count += source.count;
		target.sum_x += source.sum_x;
		target.sum_y += source.sum_y;
		RegrSlopeOperation::Combine<RegrSlopeState, OP>(source.slope, target.slope, aggr_input_data);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
			return;
		}
		RegrSlopeOperation::Finalize<T, RegrSlopeState>(state.slope, target, finalize_data);
		if (Value::IsNan(target)) {
			finalize_data.ReturnNull();
			return;
		}
		auto x_avg = state.sum_x / state.count;
		auto y_avg = state.sum_y / state.count;
		target = y_avg - target * x_avg;
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction RegrInterceptFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<RegrInterceptState, double, double, double, RegrInterceptOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

} // namespace duckdb
