#include "duckdb/function/window/window_custom_aggregator.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/common/enums/window_aggregation_mode.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowCustomAggregator
//===--------------------------------------------------------------------===//
bool WindowCustomAggregator::CanAggregate(const BoundWindowExpression &wexpr, WindowAggregationMode mode) {
	if (!wexpr.AggregateFunction()) {
		return false;
	}

	if (!wexpr.AggregateFunction()->CanWindow()) {
		return false;
	}

	//	ORDER BY arguments are not currently supported
	if (!wexpr.ArgOrders().empty()) {
		return false;
	}

	//	DISTINCT is not currently supported
	if (wexpr.Distinct()) {
		return false;
	}

	return (mode < WindowAggregationMode::COMBINE);
}

WindowCustomAggregator::WindowCustomAggregator(const BoundWindowExpression &wexpr, WindowSharedExpressions &shared)
    : WindowAggregator(wexpr, shared) {
}

WindowCustomAggregator::~WindowCustomAggregator() {
}

class WindowCustomAggregatorLocalState : public WindowAggregatorLocalState {
public:
	WindowCustomAggregatorLocalState(ExecutionContext &context, const AggregateObject &aggr,
	                                 const WindowExcludeMode exclude_mode);
	~WindowCustomAggregatorLocalState() override;

public:
	//! The aggregate function
	const AggregateObject aggr;
	//! Data pointer that contains a single state, shared by all the custom evaluators
	vector<data_t> state;
	//! Reused result state container for the window functions
	Vector statef;
	//! The frame boundaries, used for the window functions
	SubFrames frames;
};

class WindowCustomAggregatorGlobalState : public WindowAggregatorGlobalState {
public:
	using CollectionPtr = optional_ptr<WindowCollection>;

	WindowCustomAggregatorGlobalState(ClientContext &client, const WindowCustomAggregator &aggregator,
	                                  idx_t group_count)
	    : WindowAggregatorGlobalState(client, aggregator, group_count) {
	}

	//! Traditional packed filter mask for API
	ValidityMask filter_packed;
	//! Data pointer that contains a single local state, used for global custom window execution state
	unique_ptr<LocalSinkState> glstate;
	//! The argument data
	CollectionPtr collection;
	//! Column global validity flags
	vector<bool> all_valids;
	//! Frame statistics
	FrameStats stats;
};

WindowCustomAggregatorLocalState::WindowCustomAggregatorLocalState(ExecutionContext &context,
                                                                   const AggregateObject &aggr,
                                                                   const WindowExcludeMode exclude_mode)
    : WindowAggregatorLocalState(context), aggr(aggr), state(aggr.function.GetStateSize(aggr.GetFunctionData())),
      statef(Value::POINTER(CastPointerToValue(state.data())), count_t(1)), frames(3, {0, 0}) {
	// if we have a frame-by-frame method, share the single state
	AggregateStateInput state_input(aggr.function, aggr.GetFunctionData());
	data_ptr_t state_ptr = state.data();
	aggr.function.GetStateInitCallback()(state_input, &state_ptr, 1);

	InitSubFrames(frames, exclude_mode);
}

WindowCustomAggregatorLocalState::~WindowCustomAggregatorLocalState() {
	if (aggr.function.HasStateDestructorCallback()) {
		AggregateInputData aggr_input_data(aggr, allocator);
		aggr.function.GetStateDestructorCallback()(statef, aggr_input_data, 1);
	}
}

unique_ptr<GlobalSinkState> WindowCustomAggregator::GetGlobalState(ClientContext &context, idx_t group_count,
                                                                   const ValidityMask &) const {
	return make_uniq<WindowCustomAggregatorGlobalState>(context, *this, group_count);
}

SourceResultType WindowCustomAggregator::Finalize(ExecutionContext &context, CollectionPtr collection,
                                                  const FrameStats &stats, OperatorSinkInput &sink) {
	//	Single threaded Finalize for now
	auto &gcsink = sink.global_state.Cast<WindowCustomAggregatorGlobalState>();
	lock_guard<mutex> gestate_guard(gcsink.lock);
	if (gcsink.finalized) {
		return SourceResultType::FINISHED;
	}

	WindowAggregator::Finalize(context, collection, stats, sink);

	gcsink.collection = collection;
	auto inputs = collection->inputs.get();
	const auto count = collection->size();
	auto &all_valids = gcsink.all_valids;
	for (auto col_idx : child_idx) {
		all_valids.push_back(collection->all_valids[col_idx]);
	}
	auto &filter_mask = gcsink.filter_mask;
	auto &filter_packed = gcsink.filter_packed;
	filter_mask.Pack(filter_packed, filter_mask.Capacity());
	gcsink.glstate = GetLocalState(context, gcsink);

	if (aggr.function.HasWindowInitCallback()) {
		auto &gcstate = gcsink.glstate->Cast<WindowCustomAggregatorLocalState>();
		WindowPartitionInput partition(context, inputs, count, child_idx, all_valids, filter_packed, stats,
		                               sink.interrupt_state);

		AggregateInputData aggr_input_data(aggr, gcstate.allocator);
		aggr.function.GetWindowInitCallback()(aggr_input_data, partition, gcstate.state.data());
	}

	++gcsink.finalized;
	return SourceResultType::FINISHED;
}

unique_ptr<LocalSinkState> WindowCustomAggregator::GetLocalState(ExecutionContext &context,
                                                                 const GlobalSinkState &gstate) const {
	return make_uniq<WindowCustomAggregatorLocalState>(context, aggr, exclude_mode);
}

void WindowCustomAggregator::Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count,
                                      idx_t row_idx, OperatorSinkInput &sink) const {
	auto &lcstate = sink.local_state.Cast<WindowCustomAggregatorLocalState>();
	auto &frames = lcstate.frames;
	const_data_ptr_t gstate_p = nullptr;
	auto &gcsink = sink.global_state.Cast<WindowCustomAggregatorGlobalState>();
	if (gcsink.glstate) {
		auto &gcstate = gcsink.glstate->Cast<WindowCustomAggregatorLocalState>();
		gstate_p = gcstate.state.data();
	}

	auto collection = gcsink.collection;
	auto inputs = collection->inputs.get();
	auto &all_valids = gcsink.all_valids;
	auto &filter_packed = gcsink.filter_packed;
	auto &stats = gcsink.stats;
	WindowPartitionInput partition(context, inputs, collection->size(), child_idx, all_valids, filter_packed, stats,
	                               sink.interrupt_state);

	// Batched window callback path: if the aggregate installs a batch callback,
	// collect all `count` rows' subframes into a contiguous array and hand them
	// off in a single call. This lets implementations amortise per-call
	// overhead (e.g., RPC extensions that would otherwise fire one call per
	// output row).
	if (aggr.function.HasWindowBatchCallback()) {
		vector<SubFrames> all_frames;
		all_frames.reserve(count);
		EvaluateSubFrames(bounds, exclude_mode, count, row_idx, frames, [&](idx_t i) { all_frames.push_back(frames); });
		AggregateInputData aggr_input_data(aggr, lcstate.allocator);
		aggr.function.GetWindowBatchCallback()(aggr_input_data, partition, gstate_p, lcstate.state.data(),
		                                       all_frames.data(), count, result, row_idx);
		return;
	}

	EvaluateSubFrames(bounds, exclude_mode, count, row_idx, frames, [&](idx_t i) {
		// Extract the range
		AggregateInputData aggr_input_data(aggr, lcstate.allocator);
		aggr.function.GetWindowCallback()(aggr_input_data, partition, gstate_p, lcstate.state.data(), frames, result,
		                                  i);
	});
}

} // namespace duckdb
