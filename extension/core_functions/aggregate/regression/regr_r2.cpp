// REGR_R2(y, x)
// Returns the coefficient of determination for non-null pairs in a group.
// It is computed for non-null pairs using the following formula:
// null                 if var_pop(x) = 0, else
// 1                    if var_pop(y) = 0 and var_pop(x) <> 0, else
// power(corr(y,x), 2)

#include "core_functions/aggregate/algebraic/corr.hpp"
#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/regression_functions.hpp"

namespace duckdb {

namespace {
struct RegrR2State {
	static constexpr const char *STATE_NAMES[] = {"corr", "var_pop_x", "var_pop_y"};
	using STATE_TYPE = StructStateType<CorrState, StddevState, StddevState>;

	CorrState corr;
	StddevState var_pop_x;
	StddevState var_pop_y;
};

struct RegrR2Operation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &y, const B_TYPE &x, AggregateBinaryInput &idata) {
		CorrOperation::Operation<A_TYPE, B_TYPE, CorrState, OP>(state.corr, y, x, idata);
		STDDevBaseOperation::Execute<A_TYPE, StddevState>(state.var_pop_x, x);
		STDDevBaseOperation::Execute<A_TYPE, StddevState>(state.var_pop_y, y);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		CorrOperation::Combine<CorrState, OP>(source.corr, target.corr, aggr_input_data);
		STDDevBaseOperation::Combine<StddevState, OP>(source.var_pop_x, target.var_pop_x, aggr_input_data);
		STDDevBaseOperation::Combine<StddevState, OP>(source.var_pop_y, target.var_pop_y, aggr_input_data);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		auto var_pop_x = state.var_pop_x.count > 1 ? (state.var_pop_x.dsquared / state.var_pop_x.count) : 0;
		if (var_pop_x == 0) {
			finalize_data.ReturnNull();
			return;
		}
		auto var_pop_y = state.var_pop_y.count > 1 ? (state.var_pop_y.dsquared / state.var_pop_y.count) : 0;
		if (var_pop_y == 0) {
			target = 1;
			return;
		}
		CorrOperation::Finalize<T, CorrState>(state.corr, target, finalize_data);
		target = pow(target, 2);
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction RegrR2Fun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<RegrR2State, double, double, double, RegrR2Operation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

} // namespace duckdb
