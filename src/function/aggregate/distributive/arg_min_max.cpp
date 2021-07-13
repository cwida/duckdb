#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

#include <utility>

namespace duckdb {

template <class T, class T2>
struct ArgMinMaxState {
	T arg;
	T2 value;
	bool is_initialized;
};

struct ArgMinMaxOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->is_initialized = false;
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_initialized) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = state->arg;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct ArgMinOperation : ArgMinMaxOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, A_TYPE *x_data, B_TYPE *y_data, ValidityMask &amask,
	                      ValidityMask &bmask, idx_t xidx, idx_t yidx) {
		if (!state->is_initialized) {
			state->value = y_data[yidx];
			state->arg = x_data[xidx];
			state->is_initialized = true;
		} else {
			if (y_data[yidx] < state->value) {
				state->value = y_data[yidx];
				state->arg = x_data[xidx];
			}
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!source.is_initialized) {
			return;
		}
		if (!target->is_initialized) {
			target->is_initialized = true;
			target->value = source.value;
			target->arg = source.arg;
			return;
		}
		if (source.value < target->value) {
			target->value = source.value;
			target->arg = source.arg;
		}
	}
};

struct ArgMaxOperation : ArgMinMaxOperation {
	template <class A_TYPE, class B_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, A_TYPE *x_data, B_TYPE *y_data, ValidityMask &amask,
	                      ValidityMask &bmask, idx_t xidx, idx_t yidx) {
		if (!state->is_initialized) {
			state->value = y_data[yidx];
			state->arg = x_data[xidx];
			state->is_initialized = true;
		} else {
			if (state->value < y_data[yidx]) {
				state->value = y_data[yidx];
				state->arg = x_data[xidx];
			}
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!source.is_initialized) {
			return;
		}
		if (!target->is_initialized) {
			target->is_initialized = true;
			target->value = source.value;
			target->arg = source.arg;
			return;
		}
		if (target->value < source.value) {
			target->value = source.value;
			target->arg = source.arg;
		}
	}
};

template <class OP, class T>
AggregateFunction GetArgMinMaxFunctionArg2(LogicalTypeId arg_2, const LogicalType &arg) {
	switch (arg_2) {
	case LogicalTypeId::INTEGER:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, int>, T, int, T, OP>(arg, LogicalType::INTEGER,
		                                                                                 arg);
	case LogicalTypeId::BIGINT:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, uint64_t>, T, uint64_t, T, OP>(
		    arg, LogicalType::BIGINT, arg);
	case LogicalTypeId::DOUBLE:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, double>, T, double, T, OP>(arg, LogicalType::DOUBLE,
		                                                                                       arg);
	case LogicalTypeId::VARCHAR:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, string_t>, T, string_t, T, OP>(
		    arg, LogicalType::VARCHAR, arg);
	case LogicalTypeId::DATE:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, uint64_t>, T, uint64_t, T, OP>(
		    arg, LogicalType::DATE, arg);
	case LogicalTypeId::TIMESTAMP:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, uint64_t>, T, uint64_t, T, OP>(
		    arg, LogicalType::TIMESTAMP, arg);
	case LogicalTypeId::BLOB:
		return AggregateFunction::BinaryAggregate<ArgMinMaxState<T, string_t>, T, string_t, T, OP>(
		    arg, LogicalType::BLOB, arg);
	default:
		throw InternalException("Unimplemented arg_min/arg_max aggregate");
	}
}

template <class OP>
void GetArgMinMaxFunction(LogicalTypeId arg_1, AggregateFunctionSet &fun) {
	switch (arg_1) {
	case LogicalTypeId::INTEGER:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::INTEGER, LogicalType::INTEGER));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::BIGINT, LogicalType::INTEGER));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::DOUBLE, LogicalType::INTEGER));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::VARCHAR, LogicalType::INTEGER));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::DATE, LogicalType::INTEGER));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::TIMESTAMP, LogicalType::INTEGER));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int>(LogicalTypeId::BLOB, LogicalType::INTEGER));
		break;
	case LogicalTypeId::BIGINT:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::INTEGER, LogicalType::BIGINT));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::BIGINT, LogicalType::BIGINT));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::DOUBLE, LogicalType::BIGINT));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::VARCHAR, LogicalType::BIGINT));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::DATE, LogicalType::BIGINT));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::TIMESTAMP, LogicalType::BIGINT));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::BLOB, LogicalType::BIGINT));
		break;
	case LogicalTypeId::DOUBLE:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::INTEGER, LogicalType::DOUBLE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::BIGINT, LogicalType::DOUBLE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::DOUBLE, LogicalType::DOUBLE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::VARCHAR, LogicalType::DOUBLE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::DATE, LogicalType::DOUBLE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::TIMESTAMP, LogicalType::DOUBLE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, double>(LogicalTypeId::BLOB, LogicalType::DOUBLE));
		break;
	case LogicalTypeId::VARCHAR:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::INTEGER, LogicalType::VARCHAR));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::BIGINT, LogicalType::VARCHAR));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::DOUBLE, LogicalType::VARCHAR));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::VARCHAR, LogicalType::VARCHAR));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::DATE, LogicalType::VARCHAR));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::TIMESTAMP, LogicalType::VARCHAR));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::BLOB, LogicalType::VARCHAR));
		break;
	case LogicalTypeId::DATE:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::INTEGER, LogicalType::DATE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::BIGINT, LogicalType::DATE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::DOUBLE, LogicalType::DATE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::VARCHAR, LogicalType::DATE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::DATE, LogicalType::DATE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::TIMESTAMP, LogicalType::DATE));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::BLOB, LogicalType::DATE));
		break;
	case LogicalTypeId::TIMESTAMP:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::INTEGER, LogicalType::TIMESTAMP));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::BIGINT, LogicalType::TIMESTAMP));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::DOUBLE, LogicalType::TIMESTAMP));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::VARCHAR, LogicalType::TIMESTAMP));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::DATE, LogicalType::TIMESTAMP));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::TIMESTAMP, LogicalType::TIMESTAMP));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, int64_t>(LogicalTypeId::BLOB, LogicalType::TIMESTAMP));
		break;
	case LogicalTypeId::BLOB:
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::INTEGER, LogicalType::BLOB));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::BIGINT, LogicalType::BLOB));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::DOUBLE, LogicalType::BLOB));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::VARCHAR, LogicalType::BLOB));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::DATE, LogicalType::BLOB));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::TIMESTAMP, LogicalType::BLOB));
		fun.AddFunction(GetArgMinMaxFunctionArg2<OP, string_t>(LogicalTypeId::BLOB, LogicalType::BLOB));
		break;
	default:
		throw InternalException("Unimplemented arg_min/arg_max aggregate");
	}
}
void ArgMinFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet fun("argmin");
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::INTEGER, fun);
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::BIGINT, fun);
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::DOUBLE, fun);
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::VARCHAR, fun);
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::DATE, fun);
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::TIMESTAMP, fun);
	GetArgMinMaxFunction<ArgMinOperation>(LogicalTypeId::BLOB, fun);
	set.AddFunction(fun);

	//! Add min_by alias
	fun.name = "min_by";
	set.AddFunction(fun);

	//! Add arg_min alias
	fun.name = "arg_min";
	set.AddFunction(fun);
}

void ArgMaxFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet fun("argmax");
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::INTEGER, fun);
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::BIGINT, fun);
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::DOUBLE, fun);
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::VARCHAR, fun);
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::DATE, fun);
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::TIMESTAMP, fun);
	GetArgMinMaxFunction<ArgMaxOperation>(LogicalTypeId::BLOB, fun);
	set.AddFunction(fun);

	//! Add max_by alias
	fun.name = "max_by";
	set.AddFunction(fun);

	//! Add arg_max alias
	fun.name = "arg_max";
	set.AddFunction(fun);
}

} // namespace duckdb
