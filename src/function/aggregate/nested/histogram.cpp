#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/aggregate/nested_functions.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {
template <class T>
struct HistogramAggState {
	map<T, idx_t> *hist;
};

struct HistogramFunction {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->hist = nullptr;
	}

	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->hist) {
			delete state->hist;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

template <class T>
static void HistogramUpdateFunction(Vector inputs[], FunctionData *, idx_t input_count, Vector &state_vector,
                                    idx_t count) {
	D_ASSERT(input_count == 1);

	auto &input = inputs[0];
	VectorData sdata;
	state_vector.Orrify(count, sdata);
	VectorData input_data;
	input.Orrify(count, input_data);

	auto states = (HistogramAggState<T> **)sdata.data;
	for (idx_t i = 0; i < count; i++) {
		if (input_data.validity.RowIsValid(input_data.sel->get_index(i))) {
			auto state = states[sdata.sel->get_index(i)];
			if (!state->hist) {
				state->hist = new map<T, idx_t>();
			}
			auto value = (T *)input_data.data;
			(*state->hist)[value[input_data.sel->get_index(i)]]++;
		}
	}
}

static void HistogramUpdateFunctionString(Vector inputs[], FunctionData *, idx_t input_count, Vector &state_vector,
                                          idx_t count) {
	D_ASSERT(input_count == 1);

	auto &input = inputs[0];
	VectorData sdata;
	state_vector.Orrify(count, sdata);
	VectorData input_data;
	input.Orrify(count, input_data);

	auto states = (HistogramAggState<string> **)sdata.data;
	for (idx_t i = 0; i < count; i++) {
		if (input_data.validity.RowIsValid(input_data.sel->get_index(i))) {
			auto state = states[sdata.sel->get_index(i)];
			if (!state->hist) {
				state->hist = new map<string, idx_t>();
			}
			auto value = (string_t *)input_data.data;
			(*state->hist)[value[input_data.sel->get_index(i)].GetString()]++;
		}
	}
}

template <class T>
static void HistogramCombineFunction(Vector &state, Vector &combined, idx_t count) {
	VectorData sdata;
	state.Orrify(count, sdata);
	auto states_ptr = (HistogramAggState<T> **)sdata.data;

	auto combined_ptr = FlatVector::GetData<HistogramAggState<T> *>(combined);

	for (idx_t i = 0; i < count; i++) {
		auto state = states_ptr[sdata.sel->get_index(i)];
		if (!state->hist) {
			continue;
		}
		if (!combined_ptr[i]->hist) {
			combined_ptr[i]->hist = new map<T, idx_t>();
		}
		D_ASSERT(combined_ptr[i]->hist);
		D_ASSERT(state->hist);
		for (auto &entry : *state->hist) {
			(*combined_ptr[i]->hist)[entry.first] += entry.second;
		}
	}
}

template <class T>
static void HistogramFinalize(Vector &state_vector, FunctionData *, Vector &result, idx_t count) {
	VectorData sdata;
	state_vector.Orrify(count, sdata);
	auto states = (HistogramAggState<T> **)sdata.data;
	result.Initialize();

	idx_t old_len = 0;

	auto &mask = FlatVector::Validity(result);

	auto &child_entries = StructVector::GetEntries(result);
	auto &bucket_list = child_entries[0];
	auto &count_list = child_entries[1];

	auto &bucket_validity = FlatVector::Validity(*bucket_list);
	auto &count_validity = FlatVector::Validity(*count_list);
	for (idx_t i = 0; i < count; i++) {
		auto state = states[sdata.sel->get_index(i)];
		if (!state->hist) {
			mask.SetInvalid(i);
			bucket_validity.SetInvalid(i);
			count_validity.SetInvalid(i);
			continue;
		}
		for (auto &entry : *state->hist) {
			auto bucket_value = Value::CreateValue(entry.first);
			ListVector::PushBack(*bucket_list, bucket_value);
			auto count_value = Value::CreateValue(entry.second);
			ListVector::PushBack(*count_list, count_value);
		}
		auto list_struct_data = FlatVector::GetData<list_entry_t>(*bucket_list);
		list_struct_data[i].length = ListVector::GetListSize(*bucket_list) - old_len;
		list_struct_data[i].offset = old_len;

		list_struct_data = FlatVector::GetData<list_entry_t>(*count_list);
		list_struct_data[i].length = ListVector::GetListSize(*count_list) - old_len;
		list_struct_data[i].offset = old_len;
		old_len = list_struct_data[i].length;
	}
}

unique_ptr<FunctionData> HistogramBindFunction(ClientContext &context, AggregateFunction &function,
                                               vector<unique_ptr<Expression>> &arguments) {
	D_ASSERT(arguments.size() == 1);
	child_list_t<LogicalType> struct_children;
	struct_children.push_back({"bucket", LogicalType::LIST(arguments[0]->return_type)});
	struct_children.push_back({"count", LogicalType::LIST(LogicalType::UBIGINT)});
	auto struct_type = LogicalType::MAP(move(struct_children));

	function.return_type = struct_type;
	return make_unique<VariableReturnBindData>(function.return_type);
}

AggregateFunction GetHistogramFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::UINT16:
		return AggregateFunction("histogram", {LogicalType::USMALLINT}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<uint16_t>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<uint16_t>, HistogramFunction>,
		                         HistogramUpdateFunction<uint16_t>, HistogramCombineFunction<uint16_t>,
		                         HistogramFinalize<uint16_t>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<uint16_t>, HistogramFunction>);
	case PhysicalType::UINT32:
		return AggregateFunction("histogram", {LogicalType::UINTEGER}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<uint32_t>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<uint32_t>, HistogramFunction>,
		                         HistogramUpdateFunction<uint32_t>, HistogramCombineFunction<uint32_t>,
		                         HistogramFinalize<uint32_t>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<uint32_t>, HistogramFunction>);
	case PhysicalType::UINT64:
		return AggregateFunction("histogram", {LogicalType::UBIGINT}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<uint64_t>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<uint64_t>, HistogramFunction>,
		                         HistogramUpdateFunction<uint64_t>, HistogramCombineFunction<uint64_t>,
		                         HistogramFinalize<uint64_t>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<uint64_t>, HistogramFunction>);
	case PhysicalType::INT16:
		return AggregateFunction("histogram", {LogicalType::SMALLINT}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<int16_t>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<int16_t>, HistogramFunction>,
		                         HistogramUpdateFunction<int16_t>, HistogramCombineFunction<int16_t>,
		                         HistogramFinalize<int16_t>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<int16_t>, HistogramFunction>);
	case PhysicalType::INT32:
		return AggregateFunction("histogram", {LogicalType::INTEGER}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<int32_t>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<int32_t>, HistogramFunction>,
		                         HistogramUpdateFunction<int32_t>, HistogramCombineFunction<int32_t>,
		                         HistogramFinalize<int32_t>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<int32_t>, HistogramFunction>);
	case PhysicalType::INT64:
		return AggregateFunction("histogram", {LogicalType::BIGINT}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<int64_t>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<int64_t>, HistogramFunction>,
		                         HistogramUpdateFunction<int64_t>, HistogramCombineFunction<int64_t>,
		                         HistogramFinalize<int64_t>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<int64_t>, HistogramFunction>);
	case PhysicalType::FLOAT:
		return AggregateFunction("histogram", {LogicalType::FLOAT}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<float>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<float>, HistogramFunction>,
		                         HistogramUpdateFunction<float>, HistogramCombineFunction<float>,
		                         HistogramFinalize<float>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<float>, HistogramFunction>);
	case PhysicalType::DOUBLE:
		return AggregateFunction("histogram", {LogicalType::DOUBLE}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<double>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<double>, HistogramFunction>,
		                         HistogramUpdateFunction<double>, HistogramCombineFunction<double>,
		                         HistogramFinalize<double>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<double>, HistogramFunction>);
	case PhysicalType::VARCHAR:
		return AggregateFunction("histogram", {LogicalType::VARCHAR}, LogicalTypeId::MAP,
		                         AggregateFunction::StateSize<HistogramAggState<string>>,
		                         AggregateFunction::StateInitialize<HistogramAggState<string>, HistogramFunction>,
		                         HistogramUpdateFunctionString, HistogramCombineFunction<string>,
		                         HistogramFinalize<string>, nullptr, HistogramBindFunction,
		                         AggregateFunction::StateDestroy<HistogramAggState<string>, HistogramFunction>);

	default:
		throw InternalException("Unimplemented histogram aggregate");
	}
}

void HistogramFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet fun("histogram");
	fun.AddFunction(GetHistogramFunction(PhysicalType::UINT16));
	fun.AddFunction(GetHistogramFunction(PhysicalType::UINT32));
	fun.AddFunction(GetHistogramFunction(PhysicalType::UINT64));
	fun.AddFunction(GetHistogramFunction(PhysicalType::INT16));
	fun.AddFunction(GetHistogramFunction(PhysicalType::INT32));
	fun.AddFunction(GetHistogramFunction(PhysicalType::INT64));
	fun.AddFunction(GetHistogramFunction(PhysicalType::FLOAT));
	fun.AddFunction(GetHistogramFunction(PhysicalType::DOUBLE));
	fun.AddFunction(GetHistogramFunction(PhysicalType::VARCHAR));
	fun.AddFunction(AggregateFunction("histogram", {LogicalType::TIMESTAMP}, LogicalTypeId::MAP,
	                                  AggregateFunction::StateSize<HistogramAggState<int64_t>>,
	                                  AggregateFunction::StateInitialize<HistogramAggState<int64_t>, HistogramFunction>,
	                                  HistogramUpdateFunction<int64_t>, HistogramCombineFunction<int64_t>,
	                                  HistogramFinalize<int64_t>, nullptr, HistogramBindFunction,
	                                  AggregateFunction::StateDestroy<HistogramAggState<int64_t>, HistogramFunction>));
	set.AddFunction(fun);
}

} // namespace duckdb
