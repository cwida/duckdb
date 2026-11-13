#include "duckdb/function/window/window_constant_aggregator.hpp"

#include "duckdb/common/clustered_aggregate.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/window/window_aggregate_states.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowConstantAggregatorGlobalState
//===--------------------------------------------------------------------===//

class WindowConstantAggregatorGlobalState : public WindowAggregatorGlobalState {
public:
	WindowConstantAggregatorGlobalState(ClientContext &context, const WindowConstantAggregator &aggregator, idx_t count,
	                                    const ValidityMask &partition_mask);

	void Finalize(const FrameStats &stats);

	~WindowConstantAggregatorGlobalState() override {
		statef.Destroy();
	}

	//! Reused result state container for the window functions
	WindowAggregateStates statef;
	//! Aggregate results
	unique_ptr<Vector> results;
};

WindowConstantAggregatorGlobalState::WindowConstantAggregatorGlobalState(ClientContext &client,
                                                                         const WindowConstantAggregator &aggregator,
                                                                         idx_t group_count,
                                                                         const ValidityMask &partition_mask)
    : WindowAggregatorGlobalState(client, aggregator, STANDARD_VECTOR_SIZE), statef(client, aggr) {
	BuildPartitionOffsets(group_count, partition_mask);

	//	Initialise the vector for caching the results
	results = make_uniq<Vector>(aggregator.result_type, partition_offsets.size() - 1);

	//	Initialise the final states
	statef.Initialize(partition_offsets.size() - 1);
}

//===--------------------------------------------------------------------===//
// WindowConstantAggregatorLocalState
//===--------------------------------------------------------------------===//
class WindowConstantAggregatorLocalState : public WindowAggregatorLocalState {
public:
	WindowConstantAggregatorLocalState(ExecutionContext &context, const WindowConstantAggregatorGlobalState &gstate);
	~WindowConstantAggregatorLocalState() override {
	}

	void Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	          optional_ptr<SelectionVector> filter_sel, idx_t filtered);
	void Combine(WindowConstantAggregatorGlobalState &gstate);

public:
	//! The global state we are sharing
	const WindowConstantAggregatorGlobalState &gstate;
	//! Reusable chunk for sinking
	DataChunk inputs;
	//! Chunk for referencing the input columns
	DataChunk payload_chunk;
	//! A vector of pointers to "state", used for intermediate window segment aggregation
	Vector statep;
	//! Reused result state container for the window functions
	WindowAggregateStates statef;
	//! The current result partition being read
	idx_t partition;
	//! Shared SV for evaluation
	SelectionVector matches;
};

WindowConstantAggregatorLocalState::WindowConstantAggregatorLocalState(
    ExecutionContext &context, const WindowConstantAggregatorGlobalState &gstate)
    : WindowAggregatorLocalState(context), gstate(gstate), statep(Value::POINTER(0), count_t(1)),
      statef(context.client, gstate.statef.aggr), partition(0) {
	matches.Initialize();

	//	Start the aggregates
	auto &partition_offsets = gstate.partition_offsets;
	auto &aggregator = gstate.aggregator;
	statef.Initialize(partition_offsets.size() - 1);

	// Set up shared buffer
	inputs.Initialize(context.client, aggregator.arg_types);
	payload_chunk.InitializeEmpty(inputs.GetTypes());

	gstate.locals++;
}

//===--------------------------------------------------------------------===//
// WindowConstantAggregator
//===--------------------------------------------------------------------===//
bool WindowConstantAggregator::CanAggregate(const BoundWindowExpression &wexpr) {
	if (!wexpr.AggregateFunction()) {
		return false;
	}

	// The function must be able to be used as an aggregate
	if (!wexpr.AggregateFunction()->CanAggregate()) {
		return false;
	}

	// window exclusion cannot be handled by constant aggregates
	if (wexpr.WindowExclude() != WindowExcludeMode::NO_OTHER) {
		return false;
	}

	// 	DISTINCT aggregation cannot be handled by constant aggregation
	if (wexpr.Distinct()) {
		return false;
	}

	//	COUNT(*) is already handled efficiently by segment trees.
	if (wexpr.GetChildren().empty()) {
		return false;
	}

	/*
	    The default framing option is RANGE UNBOUNDED PRECEDING, which
	    is the same as RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT
	    ROW; it sets the frame to be all rows from the partition start
	    up through the current row's last peer (a row that the window's
	    ORDER BY clause considers equivalent to the current row; all
	    rows are peers if there is no ORDER BY). In general, UNBOUNDED
	    PRECEDING means that the frame starts with the first row of the
	    partition, and similarly UNBOUNDED FOLLOWING means that the
	    frame ends with the last row of the partition, regardless of
	    RANGE, ROWS or GROUPS mode. In ROWS mode, CURRENT ROW means that
	    the frame starts or ends with the current row; but in RANGE or
	    GROUPS mode it means that the frame starts or ends with the
	    current row's first or last peer in the ORDER BY ordering. The
	    offset PRECEDING and offset FOLLOWING options vary in meaning
	    depending on the frame mode.
	*/
	switch (wexpr.WindowStart()) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
		if (!wexpr.OrderBy().empty()) {
			return false;
		}
		break;
	default:
		return false;
	}

	switch (wexpr.WindowEnd()) {
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		break;
	case WindowBoundary::CURRENT_ROW_RANGE:
		if (!wexpr.OrderBy().empty()) {
			return false;
		}
		break;
	default:
		return false;
	}

	return true;
}

BoundWindowExpression &WindowConstantAggregator::RebindAggregate(ClientContext &context, BoundWindowExpression &wexpr) {
	FunctionBinder::BindSortedAggregate(context, wexpr);

	return wexpr;
}

WindowConstantAggregator::WindowConstantAggregator(BoundWindowExpression &wexpr, WindowSharedExpressions &shared,
                                                   ClientContext &context)
    : WindowAggregator(RebindAggregate(context, wexpr)) {
	// We only need these values for Sink
	for (auto &child : wexpr.GetChildren()) {
		child_idx.emplace_back(shared.RegisterSink(child));
	}
}

unique_ptr<GlobalSinkState> WindowConstantAggregator::GetGlobalState(ClientContext &context, idx_t group_count,
                                                                     const ValidityMask &partition_mask) const {
	return make_uniq<WindowConstantAggregatorGlobalState>(context, *this, group_count, partition_mask);
}

void WindowConstantAggregator::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                    idx_t input_idx, optional_ptr<SelectionVector> filter_sel, idx_t filtered,
                                    OperatorSinkInput &sink) {
	auto &lastate = sink.local_state.Cast<WindowConstantAggregatorLocalState>();

	lastate.Sink(context, sink_chunk, coll_chunk, input_idx, filter_sel, filtered);
}

void WindowConstantAggregatorLocalState::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                              idx_t row, optional_ptr<SelectionVector> filter_sel, idx_t filtered) {
	auto &partition_offsets = gstate.partition_offsets;
	const auto &aggr = gstate.aggr;
	const auto chunk_begin = row;
	const auto chunk_end = chunk_begin + sink_chunk.size();
	idx_t partition =
	    idx_t(std::upper_bound(partition_offsets.begin(), partition_offsets.end(), row) - partition_offsets.begin()) -
	    1;

	auto state_f_data = statef.GetData();
	auto state_p_data = ConstantVector::GetData<data_ptr_t>(statep);

	auto &child_idx = gstate.aggregator.child_idx;
	for (column_t c = 0; c < child_idx.size(); ++c) {
		payload_chunk.data[c].Reference(sink_chunk.data[child_idx[c]]);
	}

	AggregateInputData aggr_input_data(aggr, statef.allocator);
	idx_t begin = 0;
	idx_t filter_idx = 0;
	auto partition_end = partition_offsets[partition + 1];
	while (row < chunk_end) {
		if (row == partition_end) {
			++partition;
			partition_end = partition_offsets[partition + 1];
		}
		partition_end = MinValue(partition_end, chunk_end);
		auto end = partition_end - chunk_begin;

		inputs.Reset();
		if (filter_sel) {
			// 	Slice to any filtered rows in [begin, end)
			SelectionVector sel;

			//	Find the first value in [begin, end)
			for (; filter_idx < filtered; ++filter_idx) {
				auto idx = filter_sel->get_index(filter_idx);
				if (idx >= begin) {
					break;
				}
			}

			//	Find the first value in [end, filtered)
			sel.Initialize(filter_sel->data() + filter_idx, filtered - filter_idx);
			idx_t nsel = 0;
			for (; filter_idx < filtered; ++filter_idx, ++nsel) {
				auto idx = filter_sel->get_index(filter_idx);
				if (idx >= end) {
					break;
				}
			}

			if (nsel != inputs.size()) {
				inputs.Slice(payload_chunk, sel, nsel);
			}
		} else {
			//	Slice to [begin, end)
			if (begin == 0 && end == sink_chunk.size()) {
				inputs.Reference(payload_chunk);
			} else {
				//	we cannot resize non-flat vectors (e.g. dictionary vectors), so we have to slice
				for (idx_t c = 0; c < payload_chunk.ColumnCount(); ++c) {
					inputs.data[c].Slice(payload_chunk.data[c], begin, end);
				}
			}
			inputs.SetChildCardinality(end - begin);
		}

		//	Aggregate the filtered rows into a single state
		const auto count = inputs.size();
		auto state = state_f_data[partition];
		auto cluster_update = aggr.function.GetStateClusterUpdateCallback();
		if (cluster_update) {
			ClusteredAggr clustered;
			clustered.SetSingleRun(state, count);
			aggr_input_data.clustered = &clustered;
			cluster_update(inputs.data.data(), aggr_input_data, inputs.ColumnCount(), clustered, count);
			aggr_input_data.clustered = nullptr;
		} else {
			state_p_data[0] = state_f_data[partition];
			aggr.function.GetStateUpdateCallback()(inputs.data.data(), aggr_input_data, inputs.ColumnCount(), statep,
			                                       count);
		}

		//	Skip filtered rows too!
		row += end - begin;
		begin = end;
	}
}

SourceResultType WindowConstantAggregator::Finalize(ExecutionContext &context, CollectionPtr collection,
                                                    const FrameStats &stats, OperatorSinkInput &sink) {
	auto &gastate = sink.global_state.Cast<WindowConstantAggregatorGlobalState>();
	auto &lastate = sink.local_state.Cast<WindowConstantAggregatorLocalState>();

	//	Single-threaded combine
	lock_guard<mutex> finalize_guard(gastate.lock);
	lastate.statef.Combine(gastate.statef);
	lastate.statef.Destroy();

	if (!--gastate.locals) {
		gastate.statef.Finalize(*gastate.results);
	}
	return SourceResultType::FINISHED;
}

unique_ptr<LocalSinkState> WindowConstantAggregator::GetLocalState(ExecutionContext &context,
                                                                   const GlobalSinkState &gstate) const {
	return make_uniq<WindowConstantAggregatorLocalState>(context, gstate.Cast<WindowConstantAggregatorGlobalState>());
}

void WindowConstantAggregator::Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count,
                                        idx_t row_idx, OperatorSinkInput &sink) const {
	auto &gasink = sink.global_state.Cast<WindowConstantAggregatorGlobalState>();
	const auto &partition_offsets = gasink.partition_offsets;
	const auto &results = *gasink.results;

	auto begins = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
	//	Chunk up the constants and copy them one at a time
	auto &lcstate = sink.local_state.Cast<WindowConstantAggregatorLocalState>();
	idx_t matched = 0;
	idx_t target_offset = 0;
	for (idx_t i = 0; i < count; ++i) {
		const auto begin = begins[i];
		//	Find the partition containing [begin, end)
		while (partition_offsets[lcstate.partition + 1] <= begin) {
			//	Flush the previous partition's data
			if (matched) {
				VectorOperations::Copy(results, result, lcstate.matches, matched, 0, target_offset);
				target_offset += matched;
				matched = 0;
			}
			++lcstate.partition;
		}

		lcstate.matches.set_index(matched++, lcstate.partition);
	}

	//	Flush the last partition
	if (matched) {
		// Optimize constant result
		if (target_offset == 0 && matched == count) {
			VectorOperations::Copy(results, result, lcstate.matches, 1, 0, target_offset);
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		} else {
			VectorOperations::Copy(results, result, lcstate.matches, matched, 0, target_offset);
		}
	}
}

} // namespace duckdb
