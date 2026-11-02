#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"
#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"

namespace duckdb {

void RowOperations::InitializeStates(TupleDataLayout &layout, Vector &addresses, const SelectionVector &sel,
                                     idx_t count) {
	if (count == 0) {
		return;
	}
	auto pointers = FlatVector::GetData<data_ptr_t>(addresses);
	auto &offsets = layout.GetOffsets();
	static constexpr idx_t INITIALIZE_BATCH_SIZE = 128;
	array<data_ptr_t, INITIALIZE_BATCH_SIZE> state_pointers;

	for (idx_t start = 0; start < count; start += INITIALIZE_BATCH_SIZE) {
		const auto batch_count = MinValue<idx_t>(INITIALIZE_BATCH_SIZE, count - start);
		auto aggr_idx = layout.ColumnCount();
		for (const auto &aggr : layout.GetAggregates()) {
			AggregateStateInput state_input(aggr.function, aggr.GetFunctionData());
			const auto offset = offsets[aggr_idx++];
			if (sel.IsSet()) {
				for (idx_t i = 0; i < batch_count; ++i) {
					state_pointers[i] = pointers[sel.get_index_unsafe(start + i)] + offset;
				}
			} else {
				for (idx_t i = 0; i < batch_count; ++i) {
					state_pointers[i] = pointers[start + i] + offset;
				}
			}
			aggr.function.GetStateInitCallback()(state_input, state_pointers.data(), batch_count);
		}
	}
}

void RowOperations::DestroyStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses) {
	auto count = addresses.size();
	if (count == 0) {
		return;
	}
	//	Move to the first aggregate state
	VectorOperations::AddInPlace(addresses, UnsafeNumericCast<int64_t>(layout.GetAggrOffset()));
	for (const auto &aggr : layout.GetAggregates()) {
		if (aggr.function.HasStateDestructorCallback()) {
			AggregateInputData aggr_input_data(aggr, state.allocator);
			aggr.function.GetStateDestructorCallback()(addresses, aggr_input_data, count);
		}
		// Move to the next aggregate state
		VectorOperations::AddInPlace(addresses, UnsafeNumericCast<int64_t>(aggr.payload_size));
	}
}

void RowOperations::UpdateStates(RowOperationsState &state, AggregateObject &aggr, Vector &addresses,
                                 DataChunk &payload, idx_t arg_idx, optional_ptr<const ClusteredAggr> clustered) {
	auto count = addresses.size();
	AggregateInputData aggr_input_data(aggr, state.allocator);
	auto cluster_update = aggr.function.GetStateClusterUpdateCallback();
	aggr_input_data.clustered = cluster_update ? clustered : nullptr;
	auto inputs = aggr.child_count ? payload.data.data() + arg_idx : nullptr;
	if (clustered && cluster_update) {
		cluster_update(inputs, aggr_input_data, aggr.child_count, *clustered, count);
		return;
	}
	aggr.function.GetStateUpdateCallback()(inputs, aggr_input_data, aggr.child_count, addresses, count);
}

void RowOperations::UpdateFilteredStates(RowOperationsState &state, AggregateFilterData &filter_data,
                                         AggregateObject &aggr, Vector &addresses, DataChunk &payload, idx_t arg_idx) {
	idx_t count = filter_data.ApplyFilter(payload);
	if (count == 0) {
		return;
	}

	Vector filtered_addresses(addresses, filter_data.true_sel, count);
	filtered_addresses.Flatten();

	UpdateStates(state, aggr, filtered_addresses, filter_data.filtered_payload, arg_idx);
}

void RowOperations::UpdateStatesClustered(RowOperationsState &state, vector<AggregateObject> &aggregates,
                                          AggregateFilterDataSet *filter_set, const unsafe_vector<idx_t> *filter,
                                          Vector &addresses, DataChunk &payload, ClusteredAggr &clustered,
                                          bool skip_addresses) {
	idx_t filter_idx = 0;
	idx_t payload_idx = 0;
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggr = aggregates[aggr_idx];
		if (filter && (filter_idx >= filter->size() || aggr_idx < (*filter)[filter_idx])) {
			// Skip all the aggregates that are not in the filter
			payload_idx += aggr.child_count;
			if (!skip_addresses) {
				VectorOperations::AddInPlace(addresses, NumericCast<int64_t>(aggr.payload_size));
			}
			clustered.AdvanceStates(aggr.payload_size);
			continue;
		}
		if (filter) {
			D_ASSERT(aggr_idx == (*filter)[filter_idx]);
		}

		if (aggr.aggr_type != AggregateType::DISTINCT && aggr.filter) {
			D_ASSERT(filter_set);
			RowOperations::UpdateFilteredStates(state, filter_set->GetFilterData(aggr_idx), aggr, addresses, payload,
			                                    payload_idx);
		} else {
			UpdateStates(state, aggr, addresses, payload, payload_idx, clustered);
		}

		// Move to the next aggregate
		payload_idx += aggr.child_count;
		if (!skip_addresses) {
			VectorOperations::AddInPlace(addresses, NumericCast<int64_t>(aggr.payload_size));
		}
		clustered.AdvanceStates(aggr.payload_size);
		if (filter) {
			filter_idx++;
		}
	}
}

void RowOperations::CombineStates(RowOperationsState &state, TupleDataLayout &layout, Vector &sources,
                                  Vector &targets) {
	auto count = sources.size();
	if (count == 0) {
		return;
	}

	//	Move to the first aggregate states
	VectorOperations::AddInPlace(sources, UnsafeNumericCast<int64_t>(layout.GetAggrOffset()));
	VectorOperations::AddInPlace(targets, UnsafeNumericCast<int64_t>(layout.GetAggrOffset()));

	// Keep track of the offset
	idx_t offset = layout.GetAggrOffset();

	for (auto &aggr : layout.GetAggregates()) {
		D_ASSERT(aggr.function.HasStateCombineCallback());
		AggregateInputData aggr_input_data(aggr, state.allocator, AggregateCombineType::ALLOW_DESTRUCTIVE);
		aggr.function.GetStateCombineCallback()(sources, targets, aggr_input_data, count);

		// Move to the next aggregate states
		VectorOperations::AddInPlace(sources, UnsafeNumericCast<int64_t>(aggr.payload_size));
		VectorOperations::AddInPlace(targets, UnsafeNumericCast<int64_t>(aggr.payload_size));

		// Increment the offset
		offset += aggr.payload_size;
	}

	// Now subtract the offset to get back to the original position
	VectorOperations::AddInPlace(sources, -UnsafeNumericCast<int64_t>(offset));
	VectorOperations::AddInPlace(targets, -UnsafeNumericCast<int64_t>(offset));
}

void RowOperations::FinalizeStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses,
                                   DataChunk &result, idx_t aggr_idx) {
	// Copy the addresses
	if (!state.addresses) {
		state.addresses = make_uniq<Vector>(LogicalType::POINTER);
	}
	auto &addresses_copy = *state.addresses;
	VectorOperations::Copy(addresses, addresses_copy, result.size(), 0, 0);
	FlatVector::SetSize(addresses_copy, count_t(result.size()));

	//	Move to the first aggregate state
	VectorOperations::AddInPlace(addresses_copy, UnsafeNumericCast<int64_t>(layout.GetAggrOffset()));

	auto &aggregates = layout.GetAggregates();
	// initialize the finalize local states once - they are re-used across all finalize calls of this state
	if (state.local_states.size() < aggregates.size()) {
		state.local_states.resize(aggregates.size());
		for (idx_t i = 0; i < aggregates.size(); i++) {
			auto &callbacks = aggregates[i].function.GetCallbacks();
			if (callbacks.HasInitLocalStateFinalizeCallback()) {
				AggregateInputData aggr_input_data(aggregates[i], state.allocator);
				state.local_states[i] =
				    callbacks.GetInitLocalStateFinalizeCallback()(aggr_input_data.function, aggr_input_data.bind_data);
			}
		}
	}
	for (idx_t i = 0; i < aggregates.size(); i++) {
		auto &target = result.data[aggr_idx + i];
		auto &aggr = aggregates[i];
		AggregateFinalizeInputData finalize_input_data(aggr, state.allocator, state.local_states[i].get());
		aggr.function.GetStateFinalizeCallback()(addresses_copy, finalize_input_data, target, result.size(), 0);
		FlatVector::SetSize(target, count_t(result.size()));

		// Move to the next aggregate state
		VectorOperations::AddInPlace(addresses_copy, UnsafeNumericCast<int64_t>(aggr.payload_size));
	}
}

} // namespace duckdb
