#include "duckdb/main/context.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/operator/projection/physical_unnest.hpp"

#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"

namespace duckdb {

class UnnestOperatorState : public OperatorState {
public:
	UnnestOperatorState(ClientContext &context, const vector<unique_ptr<Expression>> &select_list)
	    : current_row(0), list_position(0), first_fetch(true), input_sel(STANDARD_VECTOR_SIZE), executor(context) {
		// for each UNNEST in the select_list, we add the child expression to the expression executor
		// and set the return type in the list_data chunk, which will contain the evaluated expression results
		vector<LogicalType> list_data_types;
		for (auto &exp : select_list) {
			D_ASSERT(exp->GetExpressionType() == ExpressionType::BOUND_UNNEST);
			auto &bue = exp->Cast<BoundUnnestExpression>();
			list_data_types.push_back(bue.Child()->GetReturnType());
			executor.AddExpression(*bue.Child().get());

			unnest_sels.emplace_back(STANDARD_VECTOR_SIZE);
			null_sels.emplace_back(STANDARD_VECTOR_SIZE);
		}
		null_counts.resize(list_data_types.size());

		auto &allocator = Allocator::Get(context);
		list_data.Initialize(allocator, list_data_types);

		list_vector_data.resize(list_data.ColumnCount());
		list_child_data.resize(list_data.ColumnCount());
	}

	idx_t current_row;
	idx_t list_position;
	unsafe_vector<idx_t> unnest_lengths;
	bool first_fetch;
	SelectionVector input_sel;
	vector<SelectionVector> unnest_sels;
	vector<SelectionVector> null_sels;
	vector<idx_t> null_counts;

	ExpressionExecutor executor;
	DataChunk list_data;
	vector<UnifiedVectorFormat> list_vector_data;
	vector<UnifiedVectorFormat> list_child_data;

public:
	//! Reset the fields of the unnest operator state
	void ResetState();
	//! Prepare the input for the next unnest
	void PrepareInput(DataChunk &input, const vector<unique_ptr<Expression>> &select_list);
};

void UnnestOperatorState::ResetState() {
	current_row = 0;
	list_position = 0;
	first_fetch = true;
}

PhysicalUnnest::PhysicalUnnest(PhysicalPlan &physical_plan, vector<LogicalType> types,
                               vector<unique_ptr<Expression>> select_list, idx_t estimated_cardinality,
                               PhysicalOperatorType type)
    : PhysicalOperator(physical_plan, type, std::move(types), estimated_cardinality),
      select_list(std::move(select_list)) {
	D_ASSERT(!this->select_list.empty());
}

void UnnestOperatorState::PrepareInput(DataChunk &input, const vector<unique_ptr<Expression>> &select_list) {
	list_data.Reset();
	// execute the expressions inside each UNNEST in the select_list to get the list data
	// execution results (lists) are kept in list_data chunk
	executor.Execute(input, list_data);

	// verify incoming lists
	list_data.Verify(executor.GetContext().GetDatabase());
	D_ASSERT(input.size() == list_data.size());
	D_ASSERT(list_data.ColumnCount() == select_list.size());
	D_ASSERT(list_vector_data.size() == list_data.ColumnCount());
	D_ASSERT(list_child_data.size() == list_data.ColumnCount());

	// get the UnifiedVectorFormat of each list_data vector (LIST vectors for the different UNNESTs)
	// both for the vector itself and its child vector
	for (idx_t col_idx = 0; col_idx < list_data.ColumnCount(); col_idx++) {
		const auto &list_vector = list_data.data[col_idx];
		list_vector.ToUnifiedFormat(list_vector_data[col_idx]);

		if (list_vector.GetType() == LogicalType::SQLNULL) {
			// UNNEST(NULL): SQLNULL vectors don't have child vectors, but we need to point to the child vector of
			// each vector, so we just get the UnifiedVectorFormat of the vector itself
			const auto &child_vector = list_vector;
			child_vector.ToUnifiedFormat(list_child_data[col_idx]);
		} else {
			const auto &child_vector = ListVector::GetChild(list_vector);
			child_vector.ToUnifiedFormat(list_child_data[col_idx]);
		}
	}
	// get the unnest lengths
	if (list_data.size() > unnest_lengths.size()) {
		unnest_lengths.resize(list_data.size());
	}
	for (idx_t r = 0; r < list_data.size(); r++) {
		unnest_lengths[r] = 0;
	}
	for (idx_t col_idx = 0; col_idx < list_data.ColumnCount(); col_idx++) {
		auto &vector_data = list_vector_data[col_idx];
		for (idx_t r = 0; r < list_data.size(); r++) {
			auto current_idx = vector_data.sel->get_index(r);
			if (!vector_data.validity.RowIsValid(current_idx)) {
				continue;
			}
			// check if this list is longer than the current unnest length
			auto list_data_entries = UnifiedVectorFormat::GetData<list_entry_t>(vector_data);
			auto list_entry = list_data_entries[current_idx];
			if (list_entry.length > unnest_lengths[r]) {
				unnest_lengths[r] = list_entry.length;
			}
		}
	}

	first_fetch = false;
}

unique_ptr<OperatorState> PhysicalUnnest::GetOperatorState(ExecutionContext &context) const {
	return PhysicalUnnest::GetState(context, select_list);
}

unique_ptr<OperatorState> PhysicalUnnest::GetState(ExecutionContext &context,
                                                   const vector<unique_ptr<Expression>> &select_list) {
	return make_uniq<UnnestOperatorState>(context.client, select_list);
}

OperatorResultType PhysicalUnnest::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                   OperatorState &state_p,
                                                   const vector<unique_ptr<Expression>> &select_list,
                                                   bool include_input) {
	auto &state = state_p.Cast<UnnestOperatorState>();

	do {
		// prepare the input data by executing any expressions and getting the
		// UnifiedVectorFormat of each LIST vector (list_vector_data) and its child vector (list_child_data)
		if (state.first_fetch) {
			state.PrepareInput(input, select_list);
		}

		// finished with all rows of this input chunk, reset
		if (state.current_row >= input.size()) {
			state.ResetState();
			return OperatorResultType::NEED_MORE_INPUT;
		}

		// we essentially create two different SelectionVectors to slice
		// one is for the input (if include_input is set)
		// the other is for the list we are unnesting
		// construct these
		idx_t result_length = 0;
		idx_t unnest_list_count = 0;
		auto initial_row = state.current_row;
		for (idx_t col_idx = 0; col_idx < state.list_data.ColumnCount(); col_idx++) {
			state.null_counts[col_idx] = 0;
		}
		while (result_length < STANDARD_VECTOR_SIZE && state.current_row < input.size()) {
			auto current_row_length = MinValue<idx_t>(STANDARD_VECTOR_SIZE - result_length,
			                                          state.unnest_lengths[state.current_row] - state.list_position);

			if (current_row_length > 0) {
				// set up the selection vectors
				if (include_input) {
					for (idx_t r = 0; r < current_row_length; r++) {
						state.input_sel.set_index(result_length + r, state.current_row);
					}
				}
				for (idx_t col_idx = 0; col_idx < state.list_data.ColumnCount(); col_idx++) {
					auto &vector_data = state.list_vector_data[col_idx];
					auto current_idx = vector_data.sel->get_index(state.current_row);
					idx_t list_length = 0;
					idx_t list_offset = 0;
					if (vector_data.validity.RowIsValid(current_idx)) {
						auto list_data = UnifiedVectorFormat::GetData<list_entry_t>(vector_data);
						auto list_entry = list_data[current_idx];
						list_length = list_entry.length;
						list_offset = list_entry.offset;
					}
					// unnest any entries we can
					idx_t unnest_length = MinValue<idx_t>(
					    list_length - MinValue<idx_t>(list_length, state.list_position), current_row_length);
					auto &unnest_sel = state.unnest_sels[col_idx];
					for (idx_t r = 0; r < unnest_length; r++) {
						unnest_sel.set_index(result_length + r, list_offset + state.list_position + r);
					}
					// for any remaining entries (if any) - set them in the null selection
					auto &null_sel = state.null_sels[col_idx];
					for (idx_t r = unnest_length; r < current_row_length; r++) {
						// we unnest the first row in the child list
						// this is chosen arbitrarily - we will override it with `NULL` afterwards
						// FIXME if the child list has a `NULL` entry we can directly unnest that and avoid having
						// to override it - this is a potential optimization we could do in the future
						unnest_sel.set_index(result_length + r, 0);
						null_sel.set_index(state.null_counts[col_idx]++, result_length + r);
					}
				}

				// move to the next row
				result_length += current_row_length;
				state.list_position += current_row_length;
			}
			unnest_list_count++;
			if (state.list_position == state.unnest_lengths[state.current_row]) {
				state.current_row++;
				state.list_position = 0;
			}
		}
		idx_t col_offset = 0;
		chunk.SetChildCardinality(result_length);
		if (result_length == 0) {
			// nothing to unnest - skip column processing entirely
			continue;
		}
		if (include_input) {
			for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
				if (unnest_list_count == 1) {
					// everything belongs to the same row - we can do a constant reference
					ConstantVector::Reference(chunk.data[col_idx], count_t(result_length), input.data[col_idx],
					                          initial_row, input.size());
				} else {
					// input values come from different rows - we need to slice
					chunk.data[col_idx].Slice(input.data[col_idx], state.input_sel, result_length);
				}
			}
			col_offset = input.ColumnCount();
		}
		for (idx_t col_idx = 0; col_idx < state.list_data.ColumnCount(); col_idx++) {
			const auto &list_vector = state.list_data.data[col_idx];
			auto &result_vector = chunk.data[col_offset + col_idx];
			if (state.list_data.data[col_idx].GetType() == LogicalType::SQLNULL ||
			    ListType::GetChildType(state.list_data.data[col_idx].GetType()) == LogicalType::SQLNULL ||
			    ListVector::GetListSize(list_vector) == 0) {
				// UNNEST(NULL) or UNNEST([])
				// we cannot slice empty lists - but if our child list is empty we can only return NULL anyway
				ConstantVector::SetNull(result_vector, count_t(result_length));
				continue;
			}
			const auto &child_vector = ListVector::GetChild(list_vector);
			result_vector.Slice(child_vector, state.unnest_sels[col_idx], result_length);
			if (state.null_counts[col_idx] > 0) {
				// we have NULL values that we need to set - flatten
				result_vector.Flatten();
				auto &null_sel = state.null_sels[col_idx];
				for (idx_t idx = 0; idx < state.null_counts[col_idx]; idx++) {
					auto null_index = null_sel.get_index(idx);
					FlatVector::SetNull(result_vector, null_index, true);
				}
			}
		}
	} while (chunk.size() == 0);
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorResultType PhysicalUnnest::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                           GlobalOperatorState &, OperatorState &state) const {
	return ExecuteInternal(context, input, chunk, state, select_list);
}

} // namespace duckdb
