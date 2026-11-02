#include "duckdb/function/window/window_executor.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowExecutor
//===--------------------------------------------------------------------===//
WindowExecutor::WindowExecutor(BoundWindowExpression &wexpr, WindowSharedExpressions &shared)
    : wexpr(wexpr),
      range_expr((WindowBoundariesState::HasPrecedingRange(wexpr) || WindowBoundariesState::HasFollowingRange(wexpr))
                     ? wexpr.OrderBy()[0].expression.get()
                     : nullptr) {
	if (range_expr) {
		range_idx = shared.RegisterCollection(wexpr.OrderByMutable()[0].expression, false);
	}

	boundary_start_idx = shared.RegisterEvaluate(wexpr.StartExprMutable());
	boundary_end_idx = shared.RegisterEvaluate(wexpr.EndExprMutable());

	if (wexpr.WindowFunction()) {
		if (wexpr.WindowFunction()->HasSharingCallback()) {
			wexpr.WindowFunction()->GetSharing(*this, shared);
		} else {
			//	If no one overrides, assume the arguments are only needed at evaluate time
			for (auto &child : wexpr.GetChildrenMutable()) {
				child_idx.emplace_back(shared.RegisterEvaluate(child));
			}
		}
	}
}

void WindowExecutor::Evaluate(ExecutionContext &context, idx_t row_idx, DataChunk &eval_chunk, Vector &result,
                              OperatorSinkInput &sink, idx_t count) const {
	//	Nothing to evaluate for an empty block - the bounds are empty, so skip the executors entirely
	if (count == 0) {
		FlatVector::SetSize(result, 0);
		return;
	}

	auto &lbstate = sink.local_state.Cast<WindowExecutorLocalState>();
	lbstate.state.UpdateBounds(row_idx, eval_chunk, count);

	EvaluateInternal(context, eval_chunk, lbstate.state.bounds, result, row_idx, sink);

	FlatVector::SetSize(result, count_t(count));
	result.Verify();
}

WindowExecutorGlobalState::WindowExecutorGlobalState(ClientContext &client, const WindowExecutor &executor,
                                                     const idx_t payload_count, const ValidityMask &partition_mask,
                                                     const ValidityMask &order_mask)
    : client(client), executor(executor), payload_count(payload_count), partition_mask(partition_mask),
      order_mask(order_mask) {
	for (const auto &child : executor.wexpr.GetChildren()) {
		arg_types.emplace_back(child->GetReturnType());
	}
}

WindowExecutorLocalState::WindowExecutorLocalState(ExecutionContext &context, const WindowExecutorGlobalState &gstate)
    : state(context, gstate) {
}

void WindowExecutorLocalState::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                    idx_t input_idx, OperatorSinkInput &sink) {
}

void WindowExecutorLocalState::Finalize(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink) {
}

unique_ptr<GlobalSinkState> WindowExecutor::GetGlobalState(ClientContext &client, const idx_t payload_count,
                                                           const ValidityMask &partition_mask,
                                                           const ValidityMask &order_mask) const {
	if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasGlobalCallback()) {
		return wexpr.WindowFunction()->GetGlobalState(client, *this, payload_count, partition_mask, order_mask);
	}
	return make_uniq<WindowExecutorGlobalState>(client, *this, payload_count, partition_mask, order_mask);
}

unique_ptr<LocalSinkState> WindowExecutor::GetLocalState(ExecutionContext &context,
                                                         const GlobalSinkState &gstate) const {
	if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasLocalCallback()) {
		return wexpr.WindowFunction()->GetLocalState(context, gstate);
	}
	return make_uniq<WindowExecutorLocalState>(context, gstate.Cast<WindowExecutorGlobalState>());
}

void WindowExecutor::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                          const idx_t input_idx, OperatorSinkInput &sink) const {
	if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasSinkCallback()) {
		wexpr.WindowFunction()->Sink(context, sink_chunk, coll_chunk, input_idx, sink);
	} else {
		auto &lbstate = sink.local_state.Cast<WindowExecutorLocalState>();
		lbstate.Sink(context, sink_chunk, coll_chunk, input_idx, sink);
	}
}

void WindowExecutor::Finalize(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink) const {
	auto &lbstate = sink.local_state.Cast<WindowExecutorLocalState>();
	lbstate.state.Finalize(collection);

	if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasFinalizeCallback()) {
		wexpr.WindowFunction()->Finalize(context, collection, sink);
	} else {
		lbstate.Finalize(context, collection, sink);
	}
}

void WindowExecutor::EvaluateInternal(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                      Vector &result, idx_t row_idx, OperatorSinkInput &sink) const {
	if (wexpr.WindowFunction() && wexpr.WindowFunction()->HasEvaluateCallback()) {
		wexpr.WindowFunction()->Evaluate(context, eval_chunk, bounds, result, row_idx, sink);
	}
}

} // namespace duckdb
