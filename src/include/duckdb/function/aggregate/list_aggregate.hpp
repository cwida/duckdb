//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate/list_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

//! The state of the "list" aggregate - shared by aggregates that buffer their input in a linked list
struct ListAggState {
	using STATE_TYPE = StateListType<StateReturnType>;

	LinkedList linked_list;
};

struct ListFunction {
	static bool IgnoreNull() {
		return false;
	}

	static LogicalType GetElementType(AggregateInputData &aggr_input_data) {
		return ListType::GetChildType(aggr_input_data.function.GetReturnType());
	}
};

//! Appends the i-th input row to the i-th state's linked list.
//! When IGNORE_NULLS is set, NULL input rows are not appended.
template <bool IGNORE_NULLS = false>
inline void ListUpdateFunction(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                               Vector &state_vector, idx_t count) {
	D_ASSERT(input_count >= 1); // see AggregateFunction::UnaryScatterUpdate
	auto &input = inputs[0];
	RecursiveUnifiedVectorFormat input_data;
	Vector::RecursiveToUnifiedFormat(input, input_data);

	auto states = state_vector.Values<ListAggState *>();

	ListSegmentFunctions functions;
	GetSegmentDataFunctions(functions, input.GetType());

	for (idx_t i = 0; i < count; i++) {
		if (IGNORE_NULLS) {
			const auto idx = input_data.unified.sel->get_index(i);
			if (!input_data.unified.validity.RowIsValid(idx)) {
				continue;
			}
		}
		auto &state = *states[i].GetValue();
		aggr_input_data.allocator.AlignNext();
		functions.AppendRows(aggr_input_data.allocator, state.linked_list, input_data, i, 1);
	}
}

//! Clustered variant of ListUpdateFunction - appends the rows of each run to that run's state.
//! Contiguous runs are appended in a single batch; scattered runs are appended row by row.
template <bool IGNORE_NULLS = false>
inline void ListClusterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                              const ClusteredAggr &clustered, idx_t count) {
	D_ASSERT(input_count >= 1); // see AggregateFunction::UnaryScatterUpdate
	auto &input = inputs[0];
	RecursiveUnifiedVectorFormat input_data;
	Vector::RecursiveToUnifiedFormat(input, input_data);

	ListSegmentFunctions functions;
	GetSegmentDataFunctions(functions, input.GetType());

	for (auto &run : clustered.runs()) {
		auto &state = *reinterpret_cast<ListAggState *>(run.state);
		auto run_sel = run.sel;

		if (!IGNORE_NULLS && !run_sel) {
			// contiguous run covering [0, run.count) without NULL filtering - append in a single batch
			aggr_input_data.allocator.AlignNext();
			functions.AppendRows(aggr_input_data.allocator, state.linked_list, input_data, 0, run.count);
			continue;
		}

		// scattered run and/or NULL filtering - append the rows one by one
		for (idx_t k = 0; k < run.count; k++) {
			idx_t entry_idx = run_sel ? run_sel[k] : k;
			if (IGNORE_NULLS) {
				const auto idx = input_data.unified.sel->get_index(entry_idx);
				if (!input_data.unified.validity.RowIsValid(idx)) {
					continue;
				}
			}
			aggr_input_data.allocator.AlignNext();
			functions.AppendRows(aggr_input_data.allocator, state.linked_list, input_data, entry_idx, 1);
		}
	}
}

inline void ListAbsorbState(ListAggState &source, ListAggState &target) {
	if (source.linked_list.total_capacity == 0) {
		// NULL, no need to append. This can happen with a filtered aggregate.
		return;
	}
	if (target.linked_list.total_capacity == 0) {
		target.linked_list = source.linked_list;
		return;
	}

	// Append the linked list.
	target.linked_list.last_segment->next = source.linked_list.first_segment;
	target.linked_list.last_segment = source.linked_list.last_segment;
	target.linked_list.total_capacity += source.linked_list.total_capacity;
}

inline void ListAbsorbFunction(Vector &states_vector, Vector &combined, AggregateInputData &aggr_input_data,
                               idx_t count) {
	D_ASSERT(aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE);

	auto states = states_vector.Values<ListAggState *>();
	auto combined_ptr = FlatVector::GetDataMutable<ListAggState *>(combined);
	for (idx_t i = 0; i < count; i++) {
		ListAbsorbState(*states[i].GetValue(), *combined_ptr[i]);
	}
}

//! OP provides GetElementType(aggr_input_data), returning the type of the values stored in the linked list
template <class OP>
void ListCombineFunction(Vector &states_vector, Vector &combined, AggregateInputData &aggr_input_data, idx_t count) {
	//	Can we use destructive combining?
	if (aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE &&
	    !aggr_input_data.combine_multiplicities) {
		ListAbsorbFunction(states_vector, combined, aggr_input_data, count);
		return;
	}

	auto states = states_vector.Values<ListAggState *>();
	auto combined_ptr = FlatVector::GetDataMutable<ListAggState *>(combined);

	auto element_type = OP::GetElementType(aggr_input_data);
	ListSegmentFunctions functions;
	GetSegmentDataFunctions(functions, element_type);
	UnifiedVectorFormat multiplicities;
	const int64_t *multiplicity_data = nullptr;
	if (aggr_input_data.combine_multiplicities) {
		aggr_input_data.combine_multiplicities->ToUnifiedFormat(multiplicities);
		multiplicity_data = UnifiedVectorFormat::GetData<int64_t>(multiplicities);
	}

	for (idx_t i = 0; i < count; i++) {
		auto &source = *states[i].GetValue();
		auto &target = *combined_ptr[i];
		idx_t multiplicity = 1;
		if (multiplicity_data) {
			const auto multiplicity_idx = multiplicities.sel->get_index(i);
			if (!multiplicities.validity.RowIsValid(multiplicity_idx)) {
				continue;
			}
			const auto signed_multiplicity = multiplicity_data[multiplicity_idx];
			if (signed_multiplicity < 0) {
				InvalidInputException::Throw("combine_aggr multiplicity must be non-negative");
			}
			multiplicity = static_cast<idx_t>(signed_multiplicity);
		}
		if (multiplicity == 0 || source.linked_list.total_capacity == 0) {
			continue;
		}
		if (multiplicity == 1 && aggr_input_data.combine_type == AggregateCombineType::ALLOW_DESTRUCTIVE) {
			ListAbsorbState(source, target);
			continue;
		}

		const auto entry_count = source.linked_list.total_capacity;
		Vector input(element_type, entry_count);
		functions.BuildListVector(source.linked_list, input, 0);

		RecursiveUnifiedVectorFormat input_data;
		Vector::RecursiveToUnifiedFormat(input, input_data);

		for (idx_t repeat_idx = 0; repeat_idx < multiplicity; repeat_idx++) {
			functions.AppendListEntry(aggr_input_data.allocator, target.linked_list, input_data,
			                          list_entry_t(0, entry_count));
		}
	}
}

} // namespace duckdb
