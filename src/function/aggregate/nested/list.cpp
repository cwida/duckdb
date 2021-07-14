#include "duckdb/function/aggregate/nested_functions.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/common/pair.hpp"

namespace duckdb {

struct ListAggState {
	Vector *list_vector;
};

struct ListFunction {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->list_vector = nullptr;
	}

	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->list_vector) {
			delete state->list_vector;
		}
	}
	static bool IgnoreNull() {
		return false;
	}
};

static void ListUpdateFunction(Vector inputs[], FunctionData *, idx_t input_count, Vector &state_vector, idx_t count) {
	D_ASSERT(input_count == 1);

	auto &input = inputs[0];
	VectorData sdata;
	state_vector.Orrify(count, sdata);

	auto list_vector_type = LogicalType::LIST(input.GetType());

	auto states = (ListAggState **)sdata.data;
	if (input.GetVectorType() == VectorType::SEQUENCE_VECTOR) {
		input.Normalify(count);
	}
	for (idx_t i = 0; i < count; i++) {
		auto state = states[sdata.sel->get_index(i)];
		if (!state->list_vector) {
			state->list_vector = new Vector(list_vector_type);
		}
		ListVector::Append(*state->list_vector, input, i + 1, i);
	}
}

static void ListCombineFunction(Vector &state, Vector &combined, idx_t count) {
	VectorData sdata;
	state.Orrify(count, sdata);
	auto states_ptr = (ListAggState **)sdata.data;

	auto combined_ptr = FlatVector::GetData<ListAggState *>(combined);

	for (idx_t i = 0; i < count; i++) {
		auto state = states_ptr[sdata.sel->get_index(i)];
		D_ASSERT(state->list_vector);
		if (!combined_ptr[i]->list_vector) {
			combined_ptr[i]->list_vector = new Vector(state->list_vector->GetType());
		}
		ListVector::Append(*combined_ptr[i]->list_vector, ListVector::GetEntry(*state->list_vector),
		                   ListVector::GetListSize(*state->list_vector));
	}
}

static void ListFinalize(Vector &state_vector, FunctionData *, Vector &result, idx_t count) {
	VectorData sdata;
	state_vector.Orrify(count, sdata);
	auto states = (ListAggState **)sdata.data;

	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);

	auto &mask = (result.GetVectorType() == VectorType::CONSTANT_VECTOR) ? ConstantVector::Validity(result)
	                                                                     : FlatVector::Validity(result);
	size_t total_len = 0;
	ListVector::SetListSize(result, total_len);
	for (idx_t i = 0; i < count; i++) {
		auto state = states[sdata.sel->get_index(i)];
		if (!state->list_vector) {
			mask.SetInvalid(i);
			continue;
		}
		D_ASSERT(state->list_vector);
		auto list_struct_data = FlatVector::GetData<list_entry_t>(result);
		auto &state_lv = *state->list_vector;
		auto state_lv_count = ListVector::GetListSize(state_lv);
		list_struct_data[i].length = state_lv_count;
		list_struct_data[i].offset = total_len;
		total_len += state_lv_count;
	}

	for (idx_t i = 0; i < count; i++) {
		auto state = states[sdata.sel->get_index(i)];
		if (!state->list_vector) {
			continue;
		}
		auto &list_vec = *state->list_vector;
		auto &list_vec_to_append = ListVector::GetEntry(list_vec);
		ListVector::Append(result, list_vec_to_append, ListVector::GetListSize(list_vec));
	}

	result.Verify(count);
}

unique_ptr<FunctionData> ListBindFunction(ClientContext &context, AggregateFunction &function,
                                          vector<unique_ptr<Expression>> &arguments) {
	D_ASSERT(arguments.size() == 1);
	function.return_type = LogicalType::LIST(arguments[0]->return_type);
	return make_unique<ListBindData>(); // TODO atm this is not used anywhere but it might not be required after all
	                                    // except for sanity checking
}

void ListFun::RegisterFunction(BuiltinFunctions &set) {
	auto agg = AggregateFunction(
	    "list", {LogicalType::ANY}, LogicalTypeId::LIST, AggregateFunction::StateSize<ListAggState>,
	    AggregateFunction::StateInitialize<ListAggState, ListFunction>, ListUpdateFunction, ListCombineFunction,
	    ListFinalize, nullptr, ListBindFunction, AggregateFunction::StateDestroy<ListAggState, ListFunction>);
	set.AddFunction(agg);
	agg.name = "array_agg";
	set.AddFunction(agg);
}

} // namespace duckdb
