#include "duckdb/execution/operator/aggregate/physical_streaming_window.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

PhysicalStreamingWindow::PhysicalStreamingWindow(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                 vector<unique_ptr<Expression>> select_list,
                                                 idx_t estimated_cardinality, optional_idx partition_limit)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::STREAMING_WINDOW, std::move(types), estimated_cardinality),
      select_list(std::move(select_list)), partition_limit(partition_limit) {
}

class StreamingWindowGlobalState : public GlobalOperatorState {
public:
	explicit StreamingWindowGlobalState(ClientContext &client);

	//! The single local state
	unique_ptr<OperatorState> local_state;
};

class StreamingWindowState : public OperatorState {
public:
	//	Fixed size
	static constexpr int64_t MAX_BUFFER = 2048;

	class AggregateState : public WindowExecutorStreamingState {
	public:
		AggregateState(ClientContext &client, BoundWindowExpression &wexpr, Allocator &allocator)
		    : wexpr(wexpr), arena_allocator(BufferAllocator::Get((client))), executor(client), filter_executor(client),
		      statev(LogicalType::POINTER, data_ptr_cast(&state_ptr), 1ULL), hashes(LogicalType::HASH),
		      addresses(LogicalType::POINTER) {
			D_ASSERT(wexpr.GetExpressionType() == ExpressionType::WINDOW_AGGREGATE);
			auto &aggregate = *wexpr.AggregateFunction();
			bind_data = wexpr.BindInfo().get();
			dtor = aggregate.GetCallbacks().GetStateDestructorCallback();
			AggregateStateInput state_input(aggregate, bind_data);
			state.resize(aggregate.GetCallbacks().GetStateSizeCallback()(state_input));
			state_ptr = state.data();
			aggregate.GetCallbacks().GetStateInitCallback()(state_input, &state_ptr, 1);
			for (auto &child : wexpr.GetChildren()) {
				arg_types.push_back(child->GetReturnType());
				executor.AddExpression(*child);
			}
			if (!arg_types.empty()) {
				arg_chunk.Initialize(allocator, arg_types);
				arg_cursor.Initialize(allocator, arg_types);
			}
			if (wexpr.Filter()) {
				filter_executor.AddExpression(*wexpr.Filter());
				filter_sel.Initialize();
			}
			if (wexpr.Distinct()) {
				distinct = make_uniq<GroupedAggregateHashTable>(client, allocator, arg_types);
				distinct_args.Initialize(allocator, arg_types);
				distinct_sel.Initialize();
			}
		}

		~AggregateState() override {
			if (dtor) {
				AggregateInputData aggr_input_data(*wexpr.AggregateFunction(), bind_data, arena_allocator);
				state_ptr = state.data();
				dtor(statev, aggr_input_data, 1);
			}
		}

		void Execute(ExecutionContext &context, DataChunk &input, Vector &result);

		//! The aggregate expression
		BoundWindowExpression &wexpr;
		//! The allocator to use for aggregate data structures
		ArenaAllocator arena_allocator;
		//! Reusable executor for the children
		ExpressionExecutor executor;
		//! Shared executor for FILTER clauses
		ExpressionExecutor filter_executor;
		//! The single aggregate state we update row-by-row
		vector<data_t> state;
		//! The pointer to the state stored in the state vector
		data_ptr_t state_ptr = nullptr;
		//! The state vector for the single state
		Vector statev;
		//! The aggregate binding data (if any)
		FunctionData *bind_data = nullptr;
		//! The aggregate state destructor (if any)
		aggregate_destructor_t dtor = nullptr;
		//! The inputs rows that pass the FILTER
		SelectionVector filter_sel;
		//! The number of unfiltered rows so far for COUNT(*)
		int64_t unfiltered = 0;
		//! Argument types
		vector<LogicalType> arg_types;
		//! Argument value buffer
		DataChunk arg_chunk;
		//! Argument cursor (a one element slice of arg_chunk)
		DataChunk arg_cursor;

		//! Hash table for accumulating the distinct values
		unique_ptr<GroupedAggregateHashTable> distinct;
		//! Filtered arguments for checking distinctness
		DataChunk distinct_args;
		//! Reusable hash vector
		Vector hashes;
		//! Rows that produced new distinct values
		SelectionVector distinct_sel;
		//! Pointers to groups in the hash table.
		Vector addresses;
	};

	explicit StreamingWindowState(ClientContext &client) : initialized(false), allocator(Allocator::Get(client)) {
	}

	~StreamingWindowState() override {
	}

	void Initialize(ClientContext &client, DataChunk &input, const vector<unique_ptr<Expression>> &expressions) {
		states.resize(expressions.size());

		for (idx_t expr_idx = 0; expr_idx < expressions.size(); expr_idx++) {
			auto &expr = *expressions[expr_idx];
			auto &wexpr = expr.Cast<BoundWindowExpression>();
			auto &fstate = states[expr_idx];
			if (expr.GetExpressionType() == ExpressionType::WINDOW_AGGREGATE) {
				fstate = make_uniq<AggregateState>(client, wexpr, allocator);
			} else if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasStreamingStateCallback()) {
				fstate = wexpr.WindowFunction()->GetStreamingState(client, input, wexpr);
			} else {
				throw NotImplementedException("GetStreamingState for %s",
				                              ExpressionTypeToString(expr.GetExpressionType()));
			}
			const auto offset = fstate->offset;
			if (offset < 0) {
				lead_count = MaxValue<idx_t>(idx_t(-offset), lead_count);
			}
		}
		if (lead_count) {
			delayed_capacity = lead_count + STANDARD_VECTOR_SIZE;
			delayed.Initialize(client, input.GetTypes(), delayed_capacity);
			shifted.Initialize(client, input.GetTypes(), delayed_capacity);
		}
		initialized = true;
	}

	static inline void ResetChunk(DataChunk &chunk) {
		chunk.Reset();
	}

	//	Limit a returned data chunk
	template <typename T>
	T Limit(DataChunk &output, optional_idx partition_limit, const T result) {
		if (!partition_limit.IsValid()) {
			return result;
		}

		const auto remaining = partition_limit.GetIndex() - emitted_count;
		if (remaining < output.size()) {
			output.Flatten();
			output.SetChildCardinality(MinValue(output.size(), remaining));
		}
		emitted_count += output.size();

		return output.size() ? result : T::FINISHED;
	}

public:
	//! We can't initialise until we have an input chunk
	bool initialized;
	//! Function states
	vector<unique_ptr<WindowExecutorStreamingState>> states;
	Allocator &allocator;
	//! The number of rows ahead to buffer for LEAD
	idx_t lead_count = 0;
	//! The full capacity of the delayed chunk
	idx_t delayed_capacity = 0;
	//! A buffer for delayed input
	DataChunk delayed;
	//! A buffer for shifting delayed input
	DataChunk shifted;
	//! Set when `delayed` has been flushed into the output by reference - we must defer resetting (resizing) it
	//! until the next call, by which point the referencing output chunk has been consumed.
	bool flushed_delayed = false;
	//! The number of rows emitted (for limiting)
	idx_t emitted_count = 0;
};

StreamingWindowGlobalState::StreamingWindowGlobalState(ClientContext &client) {
	local_state = make_uniq<StreamingWindowState>(client);
}

bool PhysicalStreamingWindow::IsStreamingFunction(ClientContext &client, BoundWindowExpression &wexpr) {
	if (!wexpr.Partitions().empty() || !wexpr.OrderBy().empty() || !wexpr.ArgOrders().empty() ||
	    wexpr.WindowExclude() != WindowExcludeMode::NO_OTHER) {
		return false;
	}
	if (wexpr.GetExpressionType() == ExpressionType::WINDOW_AGGREGATE) {
		// Aggregates with destructors (e.g., quantile) are too slow to repeatedly update/finalize
		if (wexpr.AggregateFunction()->HasStateDestructorCallback()) {
			return false;
		}
		// We can stream aggregates if they are "running totals"
		return wexpr.WindowStart() == WindowBoundary::UNBOUNDED_PRECEDING &&
		       wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_ROWS;
	} else if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasCanStreamCallback()) {
		return wexpr.WindowFunction()->CanStream(client, wexpr, StreamingWindowState::MAX_BUFFER);
	} else {
		return false;
	}
}

unique_ptr<GlobalOperatorState> PhysicalStreamingWindow::GetGlobalOperatorState(ClientContext &client) const {
	return make_uniq<StreamingWindowGlobalState>(client);
}

void StreamingWindowState::AggregateState::Execute(ExecutionContext &context, DataChunk &input, Vector &result) {
	//	Establish the aggregation environment
	const idx_t count = input.size();
	auto &aggregate = *wexpr.AggregateFunction();
	auto &aggr_state = *this;
	auto &statev = aggr_state.statev;

	// Compute the FILTER mask (if any)
	ValidityMask filter_mask;
	auto filtered = count;
	auto &filter_sel = aggr_state.filter_sel;
	if (wexpr.Filter()) {
		filtered = filter_executor.SelectExpression(input, filter_sel);
		if (filtered < count) {
			filter_mask.Initialize(count);
			filter_mask.SetAllInvalid(count);
			for (idx_t f = 0; f < filtered; ++f) {
				filter_mask.SetValid(filter_sel.get_index(f));
			}
		}
	}

	// Check for COUNT(*)
	if (wexpr.GetChildren().empty()) {
		D_ASSERT(GetTypeIdSize(result.GetType().InternalType()) == sizeof(int64_t));
		auto data = FlatVector::Writer<int64_t>(result, count);
		auto &unfiltered = aggr_state.unfiltered;
		for (idx_t i = 0; i < count; ++i) {
			unfiltered += int64_t(filter_mask.RowIsValid(i));
			data.WriteValue(unfiltered);
		}
		return;
	}

	// Compute the arguments
	auto &arg_chunk = aggr_state.arg_chunk;
	arg_chunk.Reset();
	executor.Execute(input, arg_chunk);
	arg_chunk.Flatten();

	// Update the distinct hash table
	ValidityMask distinct_mask;
	if (aggr_state.distinct) {
		auto &distinct_args = aggr_state.distinct_args;
		distinct_args.Reference(arg_chunk);
		if (wexpr.Filter()) {
			distinct_args.Slice(filter_sel, filtered);
		}
		idx_t distinct = 0;
		auto &distinct_sel = aggr_state.distinct_sel;
		if (filtered) {
			// FindOrCreateGroups assumes non-empty input
			auto &hashes = aggr_state.hashes;
			distinct_args.Hash(hashes);

			auto &addresses = aggr_state.addresses;
			distinct = aggr_state.distinct->FindOrCreateGroups(distinct_args, hashes, addresses, distinct_sel);
		}

		//	Translate the distinct selection from filtered row numbers
		//	back to input row numbers. We need to produce output for all input rows,
		//	so we filter out
		if (distinct < filtered) {
			distinct_mask.Initialize(count);
			distinct_mask.SetAllInvalid(count);
			for (idx_t d = 0; d < distinct; ++d) {
				const auto f = distinct_sel.get_index(d);
				distinct_mask.SetValid(filter_sel.get_index(f));
			}
		}
	}

	// Iterate through them using a single SV
	sel_t s = 0;
	SelectionVector sel(&s, 1);
	auto &arg_cursor = aggr_state.arg_cursor;
	arg_cursor.Reset();
	// This doesn't work for STRUCTs because the SV
	// is not copied to the children when you slice
	vector<column_t> structs;
	for (column_t col_idx = 0; col_idx < arg_chunk.ColumnCount(); ++col_idx) {
		const auto &col_vec = arg_cursor.data[col_idx];
		if (col_vec.GetType().InternalType() == PhysicalType::STRUCT) {
			structs.emplace_back(col_idx);
		} else {
			arg_cursor.data[col_idx].Slice(arg_chunk.data[col_idx], sel, 1);
		}
	}

	// Update the state and finalize it one row at a time.
	AggregateFinalizeInputData aggr_input_data(*wexpr.AggregateFunction(), wexpr.BindInfo().get(),
	                                           aggr_state.arena_allocator);
	for (idx_t i = 0; i < count; ++i) {
		sel.set_index(0, i);
		for (const auto struct_idx : structs) {
			arg_cursor.data[struct_idx].Slice(arg_chunk.data[struct_idx], sel, 1);
		}
		if (filter_mask.RowIsValid(i) && distinct_mask.RowIsValid(i)) {
			aggregate.GetStateUpdateCallback()(arg_cursor.data.data(), aggr_input_data, arg_cursor.ColumnCount(),
			                                   statev, 1);
		}
		aggregate.GetStateFinalizeCallback()(statev, aggr_input_data, result, 1, i);
	}
}

void PhysicalStreamingWindow::ExecuteFunctions(ExecutionContext &context, DataChunk &output, DataChunk &delayed,
                                               GlobalOperatorState &gstate_p) const {
	auto &gstate = gstate_p.Cast<StreamingWindowGlobalState>();
	auto &state = gstate.local_state->Cast<StreamingWindowState>();

	// Compute window functions
	const column_t input_width = children[0].get().GetTypes().size();
	for (column_t expr_idx = 0; expr_idx < select_list.size(); expr_idx++) {
		column_t col_idx = input_width + expr_idx;
		auto &expr = *select_list[expr_idx];
		auto &wexpr = expr.Cast<BoundWindowExpression>();
		auto &result = output.data[col_idx];
		auto &fstate = *state.states[expr_idx];
		if (expr.GetExpressionType() == ExpressionType::WINDOW_AGGREGATE) {
			fstate.Cast<StreamingWindowState::AggregateState>().Execute(context, output, result);
		} else if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasStreamingDataCallback()) {
			wexpr.WindowFunction()->GetStreamingData(context, output, delayed, state.delayed_capacity, result, fstate);
		} else {
			throw NotImplementedException("GetStreamingData for %s", ExpressionTypeToString(expr.GetExpressionType()));
		}
		FlatVector::SetSize(result, count_t(output.size()));
	}
}

void PhysicalStreamingWindow::ExecuteInput(ExecutionContext &context, DataChunk &delayed, DataChunk &input,
                                           DataChunk &output, GlobalOperatorState &gstate_p) const {
	auto &gstate = gstate_p.Cast<StreamingWindowGlobalState>();
	auto &state = gstate.local_state->Cast<StreamingWindowState>();

	idx_t count = input.size();

	//	Handle LEAD
	if (state.lead_count > 0) {
		//	Nothing delayed yet, so just truncate and copy the delayed values
		count -= state.lead_count;
		input.Copy(delayed, count);
	}

	// Put payload columns in place (ensuring they match the new cardinality)
	for (idx_t col_idx = 0; col_idx < input.data.size(); col_idx++) {
		output.data[col_idx].Reference(input.data[col_idx]);
		FlatVector::SetSize(output.data[col_idx], count_t(count));
	}

	ExecuteFunctions(context, output, state.delayed, gstate_p);
}

void PhysicalStreamingWindow::ExecuteShifted(ExecutionContext &context, DataChunk &delayed, DataChunk &input,
                                             DataChunk &output, GlobalOperatorState &gstate_p) const {
	auto &gstate = gstate_p.Cast<StreamingWindowGlobalState>();
	auto &state = gstate.local_state->Cast<StreamingWindowState>();
	auto &shifted = state.shifted;

	idx_t out = output.size();
	idx_t in = input.size();
	idx_t delay = delayed.size();
	D_ASSERT(out <= delay);

	state.ResetChunk(shifted);
	// shifted = delayed
	delayed.Copy(shifted);
	state.ResetChunk(delayed);
	const idx_t new_delayed_count = delay - out + in;
	for (idx_t col_idx = 0; col_idx < delayed.data.size(); ++col_idx) {
		// output[0:out] = delayed[0:out]
		output.data[col_idx].Reference(shifted.data[col_idx]);
		FlatVector::SetSize(output.data[col_idx], count_t(out));
		// delayed[0:out] = delayed[out:delay-out]
		VectorOperations::Copy(shifted.data[col_idx], delayed.data[col_idx], delay, out, 0);
		// delayed[delay-out:delay-out+in] = input[0:in]
		VectorOperations::Copy(input.data[col_idx], delayed.data[col_idx], in, 0, delay - out);
		FlatVector::SetSize(delayed.data[col_idx], count_t(new_delayed_count));
	}

	ExecuteFunctions(context, output, delayed, gstate_p);
}

void PhysicalStreamingWindow::ExecuteDelayed(ExecutionContext &context, DataChunk &delayed, DataChunk &input,
                                             DataChunk &output, GlobalOperatorState &gstate_p) const {
	idx_t count = delayed.size();
	// Put payload columns in place
	for (idx_t col_idx = 0; col_idx < delayed.data.size(); col_idx++) {
		output.data[col_idx].Reference(delayed.data[col_idx]);
		FlatVector::SetSize(output.data[col_idx], count_t(count));
	}

	ExecuteFunctions(context, output, input, gstate_p);
}

OperatorResultType PhysicalStreamingWindow::Execute(ExecutionContext &context, DataChunk &input, DataChunk &output,
                                                    GlobalOperatorState &gstate_p, OperatorState &) const {
	auto &gstate = gstate_p.Cast<StreamingWindowGlobalState>();
	auto &state = gstate.local_state->Cast<StreamingWindowState>();
	if (!state.initialized) {
		state.Initialize(context.client, input, select_list);
	}

	auto &delayed = state.delayed;
	if (!state.lead_count) {
		// Without LEAD nothing is ever delayed (the delayed buffer is not even initialized), so emit the input
		// directly.
		ExecuteInput(context, delayed, input, output, gstate_p);
		return state.Limit(output, partition_limit, OperatorResultType::NEED_MORE_INPUT);
	}
	// We can Reset delayed now that no one can be referencing it (the previous output has been consumed).
	if (state.flushed_delayed) {
		state.ResetChunk(delayed);
		state.flushed_delayed = false;
	} else if (!delayed.size()) {
		state.ResetChunk(delayed);
	}
	if (delayed.size() < state.lead_count) {
		//	If we don't have enough to produce a single row,
		//	then just delay more rows, return nothing
		//	and ask for more data.
		delayed.Append(input);
		return state.Limit(output, partition_limit, OperatorResultType::NEED_MORE_INPUT);
	} else if (input.size() < delayed.size()) {
		// If we can't consume all of the delayed values,
		// we need to split them instead of referencing them all
		output.SetChildCardinality(input.size());
		ExecuteShifted(context, delayed, input, output, gstate_p);
		// We delayed the unused input so ask for more
		return state.Limit(output, partition_limit, OperatorResultType::NEED_MORE_INPUT);
	} else if (delayed.size()) {
		//	We have enough delayed rows so flush them
		ExecuteDelayed(context, delayed, input, output, gstate_p);
		// delayed has been flushed into the output by reference. Defer resetting it until the next call (when the
		// output has been consumed), since resizing its buffers now would corrupt the referencing output chunk.
		state.flushed_delayed = true;
		// Come back to process the input
		return state.Limit(output, partition_limit, OperatorResultType::HAVE_MORE_OUTPUT);
	} else {
		//	No delayed rows, so emit what we can and delay the rest.
		ExecuteInput(context, delayed, input, output, gstate_p);
		return state.Limit(output, partition_limit, OperatorResultType::NEED_MORE_INPUT);
	}
}

OperatorFinalizeResultType PhysicalStreamingWindow::FinalExecute(ExecutionContext &context, DataChunk &output,
                                                                 GlobalOperatorState &gstate_p, OperatorState &) const {
	auto &gstate = gstate_p.Cast<StreamingWindowGlobalState>();
	auto &state = gstate.local_state->Cast<StreamingWindowState>();

	if (state.initialized && state.lead_count) {
		auto &delayed = state.delayed;
		//	There are no more input rows
		auto &input = state.shifted;
		state.ResetChunk(input);

		if (delayed.size() > STANDARD_VECTOR_SIZE) {
			//	More than one output buffer was delayed, so shift in what we can
			output.SetChildCardinality(STANDARD_VECTOR_SIZE);
			ExecuteShifted(context, delayed, input, output, gstate_p);
			return state.Limit(output, partition_limit, OperatorFinalizeResultType::HAVE_MORE_OUTPUT);
		}
		ExecuteDelayed(context, delayed, input, output, gstate_p);
	}

	return OperatorFinalizeResultType::FINISHED;
}

InsertionOrderPreservingMap<string> PhysicalStreamingWindow::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string projections;
	for (idx_t i = 0; i < select_list.size(); i++) {
		if (i > 0) {
			projections += "\n";
		}
		projections += select_list[i]->GetName();
	}
	result["Projections"] = projections;
	if (partition_limit.IsValid()) {
		result["Limit"] = to_string(partition_limit.GetIndex());
	}

	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
