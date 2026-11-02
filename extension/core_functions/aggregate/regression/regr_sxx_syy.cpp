// REGR_SXX(y, x)
// Returns REGR_COUNT(y, x) * VAR_POP(x) for non-null pairs.
// REGR_SYY(y, x)
// Returns REGR_COUNT(y, x) * VAR_POP(y) for non-null pairs.

#include "core_functions/aggregate/regression/regr_count.hpp"
#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/regression_functions.hpp"

namespace duckdb {

namespace {
struct RegrSState {
	static constexpr const char *STATE_NAMES[] = {"count", "var_pop"};
	using STATE_TYPE = StructStateType<uint64_t, StddevState>;

	uint64_t count;
	StddevState var_pop;
};

struct RegrBaseOperation {
	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		RegrCountFunction::Combine<uint64_t, OP>(source.count, target.count, aggr_input_data);
		STDDevBaseOperation::Combine<StddevState, OP>(source.var_pop, target.var_pop, aggr_input_data);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.var_pop.count == 0) {
			finalize_data.ReturnNull();
			return;
		}
		auto var_pop = state.var_pop.count > 1 ? (state.var_pop.dsquared / state.var_pop.count) : 0;
		RegrCountFunction::Finalize<T, uint64_t>(state.count, target, finalize_data);
		target *= var_pop;
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct RegrSXXOperation : RegrBaseOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &y, const B_TYPE &x, AggregateBinaryInput &idata) {
		RegrCountFunction::Operation<A_TYPE, B_TYPE, uint64_t, OP>(state.count, y, x, idata);
		STDDevBaseOperation::Execute<A_TYPE, StddevState>(state.var_pop, x);
	}
};

struct RegrSYYOperation : RegrBaseOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &y, const B_TYPE &x, AggregateBinaryInput &idata) {
		RegrCountFunction::Operation<A_TYPE, B_TYPE, uint64_t, OP>(state.count, y, x, idata);
		STDDevBaseOperation::Execute<A_TYPE, StddevState>(state.var_pop, y);
	}
};

} // namespace

AggregateFunction RegrSXXFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<RegrSState, double, double, double, RegrSXXOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

AggregateFunction RegrSYYFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<RegrSState, double, double, double, RegrSYYOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

} // namespace duckdb
