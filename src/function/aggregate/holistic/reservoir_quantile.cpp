#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/reservoir_sample.hpp"
#include "duckdb/function/aggregate/holistic_functions.hpp"
#include "duckdb/planner/expression.hpp"

#include <algorithm>
#include <queue>
#include <stdlib.h>

namespace duckdb {

struct ReservoirQuantileState {
	data_ptr_t v;
	idx_t len;
	idx_t pos;
	BaseReservoirSampling *r_samp;
};

template <class STATE, class T>
void ReplaceElement(T &input, STATE *state) {
	((T *)state->v)[state->r_samp->min_entry] = input;
	state->r_samp->ReplaceElement();
}

template <class STATE, class T>
void FillReservoir(STATE *state, idx_t sample_size, T element) {
	if (state->pos < sample_size) {
		((T *)state->v)[state->pos++] = element;
		state->r_samp->InitializeReservoir(state->pos, state->len);
	} else {
		D_ASSERT(state->r_samp->next_index >= state->r_samp->current_count);
		if (state->r_samp->next_index == state->r_samp->current_count) {
			ReplaceElement<STATE, T>(element, state);
		}
	}
}

struct ReservoirQuantileBindData : public FunctionData {
	ReservoirQuantileBindData(float quantile_p, int32_t sample_size_p)
	    : quantile(quantile_p), sample_size(sample_size_p) {
	}

	unique_ptr<FunctionData> Copy() override {
		return make_unique<ReservoirQuantileBindData>(quantile, sample_size);
	}

	bool Equals(FunctionData &other_p) override {
		auto &other = (ReservoirQuantileBindData &)other_p;
		return quantile == other.quantile;
	}

	float quantile;
	int32_t sample_size;
};

template <class T>
struct ReservoirQuantileOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->v = nullptr;
		state->len = 0;
		state->pos = 0;
		state->r_samp = nullptr;
	}

	static void ResizeState(ReservoirQuantileState *state, idx_t new_len) {
		if (new_len <= state->len) {
			return;
		}
		state->v = (data_ptr_t)realloc(state->v, new_len * sizeof(T));
		if (!state->v) {
			throw InternalException("Memory allocation failure");
		}
		state->len = new_len;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data_p, INPUT_TYPE *data, ValidityMask &mask, idx_t idx) {
		auto bind_data = (ReservoirQuantileBindData *)bind_data_p;
		D_ASSERT(bind_data);
		if (state->pos == 0) {
			ResizeState(state, bind_data->sample_size);
		}
		if (!state->r_samp) {
			state->r_samp = new BaseReservoirSampling();
		}
		D_ASSERT(state->v);
		FillReservoir<STATE, T>(state, bind_data->sample_size, data[idx]);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (source.pos == 0) {
			return;
		}
		if (target->pos == 0) {
			ResizeState(target, source.len);
		}
		if (!target->r_samp) {
			target->r_samp = new BaseReservoirSampling();
		}
		for (idx_t src_idx = 0; src_idx < source.pos; src_idx++) {
			FillReservoir<STATE, T>(target, target->len, ((T *)source.v)[src_idx]);
		}
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(Vector &result, FunctionData *bind_data_p, STATE *state, TARGET_TYPE *target,
	                     ValidityMask &mask, idx_t idx) {
		if (state->pos == 0) {
			mask.SetInvalid(idx);
			return;
		}
		D_ASSERT(state->v);
		D_ASSERT(bind_data_p);
		auto bind_data = (ReservoirQuantileBindData *)bind_data_p;
		auto v_t = (T *)state->v;
		auto offset = (idx_t)((double)(state->pos - 1) * bind_data->quantile);
		std::nth_element(v_t, v_t + offset, v_t + state->pos);
		target[idx] = v_t[offset];
	}

	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->v) {
			free(state->v);
			state->v = nullptr;
		}
		if (state->r_samp) {
			delete state->r_samp;
			state->r_samp = nullptr;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

AggregateFunction GetReservoirQuantileAggregateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT16:
		return AggregateFunction::UnaryAggregateDestructor<ReservoirQuantileState, int16_t, int16_t,
		                                                   ReservoirQuantileOperation<int16_t>>(LogicalType::SMALLINT,
		                                                                                        LogicalType::SMALLINT);

	case PhysicalType::INT32:
		return AggregateFunction::UnaryAggregateDestructor<ReservoirQuantileState, int32_t, int32_t,
		                                                   ReservoirQuantileOperation<int32_t>>(LogicalType::INTEGER,
		                                                                                        LogicalType::INTEGER);

	case PhysicalType::INT64:
		return AggregateFunction::UnaryAggregateDestructor<ReservoirQuantileState, int64_t, int64_t,
		                                                   ReservoirQuantileOperation<int64_t>>(LogicalType::BIGINT,
		                                                                                        LogicalType::BIGINT);

	case PhysicalType::INT128:
		return AggregateFunction::UnaryAggregateDestructor<ReservoirQuantileState, hugeint_t, hugeint_t,
		                                                   ReservoirQuantileOperation<hugeint_t>>(LogicalType::HUGEINT,
		                                                                                          LogicalType::HUGEINT);
	case PhysicalType::DOUBLE:
		return AggregateFunction::UnaryAggregateDestructor<ReservoirQuantileState, double, double,
		                                                   ReservoirQuantileOperation<double>>(LogicalType::DOUBLE,
		                                                                                       LogicalType::DOUBLE);
	default:
		throw InternalException("Unimplemented quantile aggregate");
	}
}

unique_ptr<FunctionData> BindReservoirQuantile(ClientContext &context, AggregateFunction &function,
                                               vector<unique_ptr<Expression>> &arguments) {
	if (!arguments[1]->IsScalar()) {
		throw BinderException("QUANTILE can only take constant quantile parameters");
	}
	Value quantile_val = ExpressionExecutor::EvaluateScalar(*arguments[1]);
	auto quantile = quantile_val.GetValue<float>();

	if (quantile_val.is_null || quantile < 0 || quantile > 1) {
		throw BinderException("QUANTILE can only take parameters in range [0, 1]");
	}
	if (arguments.size() <= 2) {
		arguments.pop_back();
		return make_unique<ReservoirQuantileBindData>(quantile, 8192);
	}
	if (!arguments[2]->IsScalar()) {
		throw BinderException("QUANTILE can only take constant quantile parameters");
	}
	Value sample_size_val = ExpressionExecutor::EvaluateScalar(*arguments[2]);
	auto sample_size = sample_size_val.GetValue<int32_t>();

	if (sample_size_val.is_null || sample_size <= 0) {
		throw BinderException("Percentage of the sample must be bigger than 0");
	}

	// remove the quantile argument so we can use the unary aggregate
	arguments.pop_back();
	arguments.pop_back();
	return make_unique<ReservoirQuantileBindData>(quantile, sample_size);
}

unique_ptr<FunctionData> BindReservoirQuantileDecimal(ClientContext &context, AggregateFunction &function,
                                                      vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = BindReservoirQuantile(context, function, arguments);
	function = GetReservoirQuantileAggregateFunction(arguments[0]->return_type.InternalType());
	function.name = "reservoir_quantile";
	return bind_data;
}

AggregateFunction GetReservoirQuantileAggregate(PhysicalType type) {
	auto fun = GetReservoirQuantileAggregateFunction(type);
	fun.bind = BindReservoirQuantile;
	// temporarily push an argument so we can bind the actual quantile
	fun.arguments.push_back(LogicalType::FLOAT);
	return fun;
}

void ReservoirQuantileFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet reservoir_quantile("reservoir_quantile");
	reservoir_quantile.AddFunction(AggregateFunction({LogicalTypeId::DECIMAL, LogicalType::FLOAT, LogicalType::INTEGER},
	                                                 LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr,
	                                                 nullptr, nullptr, BindReservoirQuantileDecimal));
	reservoir_quantile.AddFunction(AggregateFunction({LogicalTypeId::DECIMAL, LogicalType::FLOAT},
	                                                 LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr,
	                                                 nullptr, nullptr, BindReservoirQuantileDecimal));
	reservoir_quantile.AddFunction(GetReservoirQuantileAggregate(PhysicalType::INT16));
	reservoir_quantile.AddFunction(GetReservoirQuantileAggregate(PhysicalType::INT32));
	reservoir_quantile.AddFunction(GetReservoirQuantileAggregate(PhysicalType::INT64));
	reservoir_quantile.AddFunction(GetReservoirQuantileAggregate(PhysicalType::INT128));
	reservoir_quantile.AddFunction(GetReservoirQuantileAggregate(PhysicalType::DOUBLE));

	set.AddFunction(reservoir_quantile);
}

} // namespace duckdb
