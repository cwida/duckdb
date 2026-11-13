#include "duckdb/main/client_context.hpp"
#include "duckdb/function/window/window_aggregate_function.hpp"

#include "duckdb/common/enums/window_aggregation_mode.hpp"
#include "duckdb/function/window/window_constant_aggregator.hpp"
#include "duckdb/function/window/window_custom_aggregator.hpp"
#include "duckdb/function/window/window_distinct_aggregator.hpp"
#include "duckdb/function/window/window_naive_aggregator.hpp"
#include "duckdb/function/window/window_segment_tree.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowAggregateExecutor
//===--------------------------------------------------------------------===//
class WindowAggregateExecutorGlobalState : public WindowExecutorGlobalState {
public:
	WindowAggregateExecutorGlobalState(ClientContext &client, const WindowAggregateExecutor &executor,
	                                   const idx_t payload_count, const ValidityMask &partition_mask,
	                                   const ValidityMask &order_mask);

	// aggregate global state
	unique_ptr<GlobalSinkState> gsink;

	// the filter reference expression.
	const Expression *filter_ref;
};

static BoundWindowExpression &SimplifyWindowedAggregate(BoundWindowExpression &wexpr, ClientContext &context) {
	// Remove redundant/irrelevant modifiers (they can be serious performance cliffs)
	if (wexpr.AggregateFunction() && Settings::Get<EnableOptimizerSetting>(context)) {
		const auto &aggr = wexpr.AggregateFunction();
		auto &arg_orders = wexpr.ArgOrdersMutable();
		if (aggr->GetDistinctDependent() != AggregateDistinctDependent::DISTINCT_DEPENDENT) {
			wexpr.DistinctMutable() = false;
		}
		if (aggr->GetOrderDependent() != AggregateOrderDependent::ORDER_DEPENDENT) {
			arg_orders.clear();
		} else if (!wexpr.Distinct()) {
			//	If the argument order is prefix of the partition ordering,
			//	then we can just use the partition ordering.
			if (BoundWindowExpression::GetSharedOrders(wexpr.OrderBy(), arg_orders) == arg_orders.size()) {
				arg_orders.clear();
			}
		}
	}

	return wexpr;
}

WindowAggregateExecutor::WindowAggregateExecutor(BoundWindowExpression &wexpr, ClientContext &client,
                                                 WindowSharedExpressions &shared)
    : WindowExecutor(SimplifyWindowedAggregate(wexpr, client), shared),
      mode(Settings::Get<DebugWindowModeSetting>(client)) {
	// Force naive for SEPARATE mode or for (currently!) unsupported functionality
	if (!Settings::Get<EnableOptimizerSetting>(client) || mode == WindowAggregationMode::SEPARATE) {
		if (!WindowNaiveAggregator::CanAggregate(wexpr)) {
			throw InvalidInputException("Cannot use non-aggregate window function with naive window executor!");
		}
		aggregator = make_uniq<WindowNaiveAggregator>(*this, shared);
	} else if (WindowDistinctAggregator::CanAggregate(wexpr)) {
		// build a merge sort tree
		// see https://dl.acm.org/doi/pdf/10.1145/3514221.3526184
		aggregator = make_uniq<WindowDistinctAggregator>(wexpr, shared, client);
	} else if (WindowConstantAggregator::CanAggregate(wexpr)) {
		aggregator = make_uniq<WindowConstantAggregator>(wexpr, shared, client);
	} else if (WindowCustomAggregator::CanAggregate(wexpr, mode)) {
		aggregator = make_uniq<WindowCustomAggregator>(wexpr, shared);
	} else if (WindowSegmentTree::CanAggregate(wexpr)) {
		// build a segment tree for frame-adhering aggregates
		// see http://www.vldb.org/pvldb/vol8/p1058-leis.pdf
		aggregator = make_uniq<WindowSegmentTree>(wexpr, shared);
	} else if (WindowNaiveAggregator::CanAggregate(wexpr)) {
		// No accelerator can handle this combination, so fall back to naïve.
		aggregator = make_uniq<WindowNaiveAggregator>(*this, shared);
	} else {
		// This shouldn't happen, if we get here, the binder messed up
		// Non-aggregate window functions that can't be handled by the WindowCustomAggregator due to e.g. a ORDER BY
		// clause should have been caught in the binder.
		throw InternalException("Could not create a window aggregator with the given parameters!");
	}

	// Compute the FILTER with the other eval columns.
	// Anyone who needs it can then convert it to the form they need.
	if (wexpr.Filter()) {
		const auto filter_idx = shared.RegisterSink(wexpr.Filter());
		filter_ref = make_uniq<BoundReferenceExpression>(wexpr.Filter()->GetReturnType(), filter_idx);
	}
}

WindowAggregateExecutorGlobalState::WindowAggregateExecutorGlobalState(ClientContext &client,
                                                                       const WindowAggregateExecutor &executor,
                                                                       const idx_t group_count,
                                                                       const ValidityMask &partition_mask,
                                                                       const ValidityMask &order_mask)
    : WindowExecutorGlobalState(client, executor, group_count, partition_mask, order_mask),
      filter_ref(executor.filter_ref.get()) {
	gsink = executor.aggregator->GetGlobalState(client, group_count, partition_mask);
}

unique_ptr<GlobalSinkState> WindowAggregateExecutor::GetGlobalState(ClientContext &client, const idx_t payload_count,
                                                                    const ValidityMask &partition_mask,
                                                                    const ValidityMask &order_mask) const {
	return make_uniq<WindowAggregateExecutorGlobalState>(client, *this, payload_count, partition_mask, order_mask);
}

class WindowAggregateExecutorLocalState : public WindowExecutorLocalState {
public:
	WindowAggregateExecutorLocalState(ExecutionContext &context, const GlobalSinkState &gstate,
	                                  const WindowAggregator &aggregator)
	    : WindowExecutorLocalState(context, gstate.Cast<WindowAggregateExecutorGlobalState>()),
	      filter_executor(context.client) {
		auto &gastate = gstate.Cast<WindowAggregateExecutorGlobalState>();
		aggregator_state = aggregator.GetLocalState(context, *gastate.gsink);

		// evaluate the FILTER clause and stuff it into a large mask for compactness and reuse
		auto filter_ref = gastate.filter_ref;
		if (filter_ref) {
			filter_executor.AddExpression(*filter_ref);
			filter_sel.Initialize(STANDARD_VECTOR_SIZE);
		}
	}

public:
	// state of aggregator
	unique_ptr<LocalSinkState> aggregator_state;
	//! Executor for any filter clause
	ExpressionExecutor filter_executor;
	//! Result of filtering
	SelectionVector filter_sel;
};

unique_ptr<LocalSinkState> WindowAggregateExecutor::GetLocalState(ExecutionContext &context,
                                                                  const GlobalSinkState &gstate) const {
	return make_uniq<WindowAggregateExecutorLocalState>(context, gstate, *aggregator);
}

void WindowAggregateExecutor::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                   const idx_t input_idx, OperatorSinkInput &sink) const {
	auto &gastate = sink.global_state.Cast<WindowAggregateExecutorGlobalState>();
	auto &lastate = sink.local_state.Cast<WindowAggregateExecutorLocalState>();
	auto &filter_sel = lastate.filter_sel;
	auto &filter_executor = lastate.filter_executor;

	idx_t filtered = 0;
	SelectionVector *filtering = nullptr;
	if (gastate.filter_ref) {
		filtering = &filter_sel;
		filtered = filter_executor.SelectExpression(sink_chunk, filter_sel);
	}

	D_ASSERT(aggregator);
	OperatorSinkInput asink {*gastate.gsink, *lastate.aggregator_state, sink.interrupt_state};
	aggregator->Sink(context, sink_chunk, coll_chunk, input_idx, filtering, filtered, asink);

	WindowExecutor::Sink(context, sink_chunk, coll_chunk, input_idx, sink);
}

static void ApplyWindowStats(const WindowBoundary &boundary, FrameDelta &delta, BaseStatistics *base, bool is_start) {
	// Avoid overflow by clamping to the frame bounds
	auto base_stats = delta;

	switch (boundary) {
	case WindowBoundary::UNBOUNDED_PRECEDING:
		if (is_start) {
			delta.end = 0;
			return;
		}
		break;
	case WindowBoundary::UNBOUNDED_FOLLOWING:
		if (!is_start) {
			delta.begin = 0;
			return;
		}
		break;
	case WindowBoundary::CURRENT_ROW_ROWS:
		delta.begin = delta.end = 0;
		return;
	case WindowBoundary::EXPR_PRECEDING_ROWS:
		if (base && base->GetStatsType() == StatisticsType::NUMERIC_STATS && NumericStats::HasMinMax(*base)) {
			//	Preceding so negative offset from current row
			base_stats.begin = NumericStats::GetMin<int64_t>(*base);
			base_stats.end = NumericStats::GetMax<int64_t>(*base);
			if (delta.begin < base_stats.end && base_stats.end < delta.end) {
				delta.begin = -base_stats.end;
			}
			if (delta.begin < base_stats.begin && base_stats.begin < delta.end) {
				delta.end = -base_stats.begin + 1;
			}
		}
		return;
	case WindowBoundary::EXPR_FOLLOWING_ROWS:
		if (base && base->GetStatsType() == StatisticsType::NUMERIC_STATS && NumericStats::HasMinMax(*base)) {
			base_stats.begin = NumericStats::GetMin<int64_t>(*base);
			base_stats.end = NumericStats::GetMax<int64_t>(*base);
			if (base_stats.end < delta.end) {
				delta.end = base_stats.end + 1;
			}
		}
		return;

	case WindowBoundary::CURRENT_ROW_RANGE:
	case WindowBoundary::EXPR_PRECEDING_RANGE:
	case WindowBoundary::EXPR_FOLLOWING_RANGE:
		return;
	case WindowBoundary::CURRENT_ROW_GROUPS:
	case WindowBoundary::EXPR_PRECEDING_GROUPS:
	case WindowBoundary::EXPR_FOLLOWING_GROUPS:
		return;
	case WindowBoundary::INVALID:
		throw InternalException(is_start ? "Unknown window start boundary" : "Unknown window end boundary");
		break;
	}

	if (is_start) {
		throw InternalException("Unsupported window start boundary");
	} else {
		throw InternalException("Unsupported window end boundary");
	}
}

SourceResultType WindowAggregateExecutor::Finalize(ExecutionContext &context, CollectionPtr collection,
                                                   OperatorSinkInput &sink) const {
	// Aggregates have no finalize callback, so this does not yield or block
	const auto result = WindowExecutor::Finalize(context, collection, sink);
	D_ASSERT(result == SourceResultType::FINISHED);
	(void)result;

	auto &gastate = sink.global_state.Cast<WindowAggregateExecutorGlobalState>();
	auto &gsink = gastate.gsink;
	D_ASSERT(aggregator);

	//	Estimate the frame statistics
	//	Default to the entire partition if we don't know anything
	FrameStats stats;
	const auto count = NumericCast<int64_t>(gastate.payload_count);

	//	First entry is the frame start
	stats[0] = FrameDelta(-count, count);
	auto base = wexpr.ExprStats().empty() ? nullptr : wexpr.ExprStats()[0].get();
	ApplyWindowStats(wexpr.WindowStart(), stats[0], base, true);

	//	Second entry is the frame end
	stats[1] = FrameDelta(-count, count);
	base = wexpr.ExprStats().empty() ? nullptr : wexpr.ExprStats()[1].get();
	ApplyWindowStats(wexpr.WindowEnd(), stats[1], base, false);

	auto &lastate = sink.local_state.Cast<WindowAggregateExecutorLocalState>();
	OperatorSinkInput asink {*gsink, *lastate.aggregator_state, sink.interrupt_state};
	return aggregator->Finalize(context, collection, stats, asink);
}

void WindowAggregateExecutor::EvaluateInternal(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                               Vector &result, idx_t row_idx, OperatorSinkInput &sink) const {
	auto &gastate = sink.global_state.Cast<WindowAggregateExecutorGlobalState>();
	auto &lastate = sink.local_state.Cast<WindowAggregateExecutorLocalState>();
	auto &gsink = gastate.gsink;
	D_ASSERT(aggregator);

	OperatorSinkInput asink {*gsink, *lastate.aggregator_state, sink.interrupt_state};
	aggregator->Evaluate(context, bounds, result, bounds.size(), row_idx, asink);
}

} // namespace duckdb
