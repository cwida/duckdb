#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/operator/aggregate_operators.hpp"

namespace duckdb {

namespace {

struct BoolState {
	using STATE_TYPE = OptionalStateType<bool>;
	bool value;
	bool is_set;
};

template <class REDUCE_OP, bool INIT_VALUE>
struct BoolAggregate {
	// No Initialize: StateInitialize falls back to memset(state, 0) = nullopt

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		state.value = REDUCE_OP::template Operation<bool>(state.is_set ? state.value : INIT_VALUE, bool(input));
		state.is_set = true;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		if (count > 0) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!source.is_set) {
			return;
		}
		target.value = REDUCE_OP::template Operation<bool>(target.is_set ? target.value : INIT_VALUE, source.value);
		target.is_set = true;
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
			return;
		}
		target = state.value;
	}

	static bool IgnoreNull() {
		return true;
	}
};

using BoolAndFunFunction = BoolAggregate<LogicalAnd, true>;
using BoolOrFunFunction = BoolAggregate<LogicalOr, false>;

} // namespace

AggregateFunction BoolOrFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<BoolState, bool, bool, BoolOrFunFunction>(
	    LogicalType(LogicalTypeId::BOOLEAN), LogicalType::BOOLEAN);
	fun.GetSignature().GetParameter(0).SetName("arg");
	fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	fun.SetDistinctDependent(AggregateDistinctDependent::NOT_DISTINCT_DEPENDENT);
	return fun;
}

AggregateFunction BoolAndFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<BoolState, bool, bool, BoolAndFunFunction>(
	    LogicalType(LogicalTypeId::BOOLEAN), LogicalType::BOOLEAN);
	fun.GetSignature().GetParameter(0).SetName("arg");
	fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	fun.SetDistinctDependent(AggregateDistinctDependent::NOT_DISTINCT_DEPENDENT);
	return fun;
}

} // namespace duckdb
