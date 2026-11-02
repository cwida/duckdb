#include "duckdb/function/window/window_aggregate_states.hpp"

namespace duckdb {

WindowAggregateStates::WindowAggregateStates(ClientContext &client, const AggregateObject &aggr)
    : client(client), aggr(aggr), state_size(aggr.function.GetStateSize(aggr.GetFunctionData())),
      allocator(BufferAllocator::Get(client)) {
}

void WindowAggregateStates::Initialize(idx_t count_p) {
	// Don't leak - every Initialize must be matched with a Destroy
	D_ASSERT(count == 0);

	count = count_p;
	if (count > 0) {
		states = BufferAllocator::Get(client).Allocate(count * state_size);
	}
	auto state_ptr = states.get();

	statef = make_uniq<Vector>(LogicalType::POINTER, count);
	auto state_f_data = FlatVector::Writer<data_ptr_t>(*statef, count);
	AggregateStateInput state_input(aggr.function, aggr.GetFunctionData());
	for (idx_t i = 0; i < count; ++i, state_ptr += state_size) {
		state_f_data.WriteValue(state_ptr);
		aggr.function.GetStateInitCallback()(state_input, &state_ptr, 1);
	}

	// Prevent conversion of results to constants
	statef->SetVectorType(VectorType::FLAT_VECTOR);
}

void WindowAggregateStates::Combine(WindowAggregateStates &target) {
	AggregateInputData aggr_input_data(aggr, allocator, AggregateCombineType::ALLOW_DESTRUCTIVE);
	aggr.function.GetStateCombineCallback()(*statef, *target.statef, aggr_input_data, GetCount());
}

void WindowAggregateStates::Finalize(Vector &result) {
	AggregateFinalizeInputData aggr_input_data(aggr, allocator);
	aggr.function.GetStateFinalizeCallback()(*statef, aggr_input_data, result, GetCount(), 0);
}

void WindowAggregateStates::Destroy() {
	if (count == 0) {
		return;
	}

	AggregateInputData aggr_input_data(aggr, allocator);
	if (aggr.function.HasStateDestructorCallback()) {
		aggr.function.GetStateDestructorCallback()(*statef, aggr_input_data, GetCount());
	}

	states.Reset();
	count = 0;
}

} // namespace duckdb
