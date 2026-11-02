// REGR_SXY(y, x)
// Returns REGR_COUNT(expr1, expr2) * COVAR_POP(expr1, expr2) for non-null pairs.

#include "core_functions/aggregate/regression/regr_count.hpp"
#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/regression_functions.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

namespace {

struct RegrSXyState {
	static constexpr const char *STATE_NAMES[] = {"count", "cov_pop"};
	using STATE_TYPE = StructStateType<uint64_t, CovarState>;

	uint64_t count;
	CovarState cov_pop;
};

struct RegrSXYOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const A_TYPE &y, const B_TYPE &x, AggregateBinaryInput &idata) {
		RegrCountFunction::Operation<A_TYPE, B_TYPE, uint64_t, OP>(state.count, y, x, idata);
		CovarOperation::Operation<A_TYPE, B_TYPE, CovarState, OP>(state.cov_pop, y, x, idata);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		CovarOperation::Combine<CovarState, OP>(source.cov_pop, target.cov_pop, aggr_input_data);
		RegrCountFunction::Combine<uint64_t, OP>(source.count, target.count, aggr_input_data);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		CovarPopOperation::Finalize<T, CovarState>(state.cov_pop, target, finalize_data);
		auto cov_pop = target;
		RegrCountFunction::Finalize<T, uint64_t>(state.count, target, finalize_data);
		target *= cov_pop;
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunction RegrSXYFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<RegrSXyState, double, double, double, RegrSXYOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

} // namespace duckdb
