#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/function/function_set.hpp"
#include <unordered_map>

namespace duckdb {

template <class T>
struct EntropyState {
	using DistinctMap = unordered_map<T, idx_t>;

	idx_t count;
	DistinctMap *distinct;

	EntropyState &operator=(const EntropyState &other) = delete;

	EntropyState &Assign(const EntropyState &other) {
		if (this == &other) {
			return *this;
		}
		if (distinct) {
			delete distinct;
		}
		distinct = new DistinctMap(*other.distinct);
		count = other.count;
		return *this;
	}
};

struct EntropyFunctionBase {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->distinct = nullptr;
		state->count = 0;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!source.distinct) {
			return;
		}
		if (!target->distinct) {
			target->Assign(source);
			return;
		}
		for (auto &val : *source.distinct) {
			auto value = val.first;
			(*target->distinct)[value] += val.second;
		}
		target->count += source.count;
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		double count = state->count;
		if (state->distinct) {
			double entropy = 0;
			for (auto &val : *state->distinct) {
				entropy += (val.second / count) * log2(count / val.second);
			}
			target[idx] = entropy;
		} else {
			target[idx] = 0;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->distinct) {
			delete state->distinct;
		}
	}
};

struct EntropyFunction : EntropyFunctionBase {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->distinct) {
			state->distinct = new unordered_map<INPUT_TYPE, idx_t>();
		}
		(*state->distinct)[input[idx]]++;
		state->count++;
	}
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
		}
	}
};

struct EntropyFunctionString : EntropyFunctionBase {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->distinct) {
			state->distinct = new unordered_map<string, idx_t>();
		}
		auto value = input[idx].GetString();
		(*state->distinct)[value]++;
		state->count++;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
		}
	}
};

AggregateFunction GetEntropyFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::UINT16:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<uint16_t>, uint16_t, double, EntropyFunction>(
		    LogicalType::UTINYINT, LogicalType::DOUBLE);
	case PhysicalType::UINT32:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<uint32_t>, uint32_t, double, EntropyFunction>(
		    LogicalType::UINTEGER, LogicalType::DOUBLE);
	case PhysicalType::UINT64:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<uint64_t>, uint64_t, double, EntropyFunction>(
		    LogicalType::UBIGINT, LogicalType::DOUBLE);
	case PhysicalType::INT16:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<int16_t>, int16_t, double, EntropyFunction>(
		    LogicalType::TINYINT, LogicalType::DOUBLE);
	case PhysicalType::INT32:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<int32_t>, int32_t, double, EntropyFunction>(
		    LogicalType::INTEGER, LogicalType::DOUBLE);
	case PhysicalType::INT64:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<int64_t>, int64_t, double, EntropyFunction>(
		    LogicalType::BIGINT, LogicalType::DOUBLE);
	case PhysicalType::FLOAT:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<float>, float, double, EntropyFunction>(
		    LogicalType::FLOAT, LogicalType::DOUBLE);
	case PhysicalType::DOUBLE:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<double>, double, double, EntropyFunction>(
		    LogicalType::DOUBLE, LogicalType::DOUBLE);
	case PhysicalType::VARCHAR:
		return AggregateFunction::UnaryAggregateDestructor<EntropyState<string>, string_t, double,
		                                                   EntropyFunctionString>(LogicalType::VARCHAR,
		                                                                          LogicalType::DOUBLE);

	default:
		throw InternalException("Unimplemented approximate_count aggregate");
	}
}

void EntropyFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet entropy("entropy");
	entropy.AddFunction(GetEntropyFunction(PhysicalType::UINT16));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::UINT32));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::UINT64));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::FLOAT));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::INT16));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::INT32));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::INT64));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::DOUBLE));
	entropy.AddFunction(GetEntropyFunction(PhysicalType::VARCHAR));
	entropy.AddFunction(
	    AggregateFunction::UnaryAggregateDestructor<EntropyState<int64_t>, int64_t, double, EntropyFunction>(
	        LogicalType::TIMESTAMP, LogicalType::DOUBLE));
	set.AddFunction(entropy);
}

} // namespace duckdb
