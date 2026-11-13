#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/operator/projection/physical_pivot.hpp"

#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

PhysicalPivot::PhysicalPivot(PhysicalPlan &physical_plan, vector<LogicalType> types_p, PhysicalOperator &child,
                             BoundPivotInfo bound_pivot_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::PIVOT, std::move(types_p), child.estimated_cardinality),
      bound_pivot(std::move(bound_pivot_p)) {
	children.push_back(child);
	for (idx_t p = 0; p < bound_pivot.pivot_values.size(); p++) {
		auto entry = pivot_map.find(bound_pivot.pivot_values[p]);
		if (entry != pivot_map.end()) {
			continue;
		}
		pivot_map[bound_pivot.pivot_values[p]] = bound_pivot.group_count + p;
	}
	// extract the empty aggregate expressions
	for (auto &aggr_expr : bound_pivot.aggregates) {
		auto &aggr = aggr_expr->Cast<BoundAggregateExpression>();
		// for each aggregate, initialize an empty aggregate state and finalize it immediately
		AggregateStateInput state_input(aggr.Function(), aggr.BindInfo().get());
		auto state = make_unsafe_uniq_array<data_t>(aggr.Function().GetStateSizeCallback()(state_input));
		data_ptr_t state_ptr = state.get();
		aggr.Function().GetStateInitCallback()(state_input, &state_ptr, 1);
		Vector state_vector(Value::POINTER(CastPointerToValue(state.get())), count_t(1));
		Vector result_vector(aggr_expr->GetReturnType());
		AggregateFinalizeInputData aggr_input_data(aggr, physical_plan.ArenaRef());
		aggr.Function().GetStateFinalizeCallback()(state_vector, aggr_input_data, result_vector, 1, 0);
		empty_aggregates.push_back(result_vector.GetValue(0));
	}
}

struct PivotColumnCopy {
	explicit PivotColumnCopy(idx_t row_count) : source_sel(row_count), result_sel(row_count) {
		for (idx_t i = 0; i < row_count; i++) {
			result_sel.set_index(i, 0);
		}
	}

	//! Child-vector positions to copy into result slots 1..count.
	SelectionVector source_sel;
	//! Result slot for each output row; zero selects the empty aggregate.
	SelectionVector result_sel;
	idx_t count = 0;
};

OperatorResultType PhysicalPivot::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                          GlobalOperatorState &gstate, OperatorState &state) const {
	// copy the groups as-is
	input.Flatten();
	for (idx_t i = 0; i < bound_pivot.group_count; i++) {
		chunk.data[i].Reference(input.data[i]);
	}
	auto pivot_column_lists = FlatVector::GetData<list_entry_t>(input.data.back());
	auto &pivot_column_values = ListVector::GetChild(input.data.back());
	auto pivot_columns = FlatVector::GetData<string_t>(pivot_column_values);

	vector<PivotColumnCopy> column_copies;
	column_copies.reserve(chunk.ColumnCount() - bound_pivot.group_count);
	for (idx_t c = bound_pivot.group_count; c < chunk.ColumnCount(); c++) {
		column_copies.emplace_back(input.size());
	}

	// record source positions and output rows for each pivot column
	for (idx_t r = 0; r < input.size(); r++) {
		auto list = pivot_column_lists[r];
		for (idx_t l = 0; l < list.length; l++) {
			// figure out the column value number of this list
			auto &column_name = pivot_columns[list.offset + l];
			auto entry = pivot_map.find(column_name);
			if (entry == pivot_map.end()) {
				// column entry not found in map - that means this element is explicitly excluded from the pivot list
				continue;
			}
			auto column_idx = entry->second;
			for (idx_t aggr = 0; aggr < empty_aggregates.size(); aggr++) {
				auto pivot_value_lists = FlatVector::GetData<list_entry_t>(input.data[bound_pivot.group_count + aggr]);
				if (list.length != pivot_value_lists[r].length) {
					throw InternalException("Pivot - unaligned lists between values and columns!?");
				}
				auto &copy = column_copies[column_idx + aggr - bound_pivot.group_count];
				auto target_idx = copy.result_sel.get_index(r);
				if (target_idx == 0) {
					target_idx = ++copy.count;
					copy.result_sel.set_index(r, target_idx);
				}
				copy.source_sel.set_index(target_idx - 1, pivot_value_lists[r].offset + l);
			}
		}
	}
	// copy each column's selected values and restore output row order
	for (idx_t c = bound_pivot.group_count; c < chunk.ColumnCount(); c++) {
		auto aggr = (c - bound_pivot.group_count) % empty_aggregates.size();
		auto &copy = column_copies[c - bound_pivot.group_count];
		auto &result = chunk.data[c];
		if (copy.count == 0) {
			result.Reference(empty_aggregates[aggr], count_t(input.size()));
			continue;
		}
		// Slot zero holds the empty aggregate for rows without this pivot value.
		result.Reference(empty_aggregates[aggr], count_t(1));
		result.Flatten();
		auto &source = ListVector::GetChild(input.data[bound_pivot.group_count + aggr]);
		result.Append(source, copy.source_sel, copy.count, VectorAppendMode::ALLOW_RESIZE);
		result.Slice(copy.result_sel, input.size());
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

} // namespace duckdb
