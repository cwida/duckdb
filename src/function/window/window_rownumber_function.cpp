#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/function/window/window_token_tree.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowRowNumberGlobalState
//===--------------------------------------------------------------------===//
class WindowRowNumberGlobalState : public WindowExecutorGlobalState {
public:
	WindowRowNumberGlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
	                           const ValidityMask &partition_mask, const ValidityMask &order_mask)
	    : WindowExecutorGlobalState(client, executor, payload_count, partition_mask, order_mask) {
		if (!executor.arg_order_idx.empty()) {
			use_framing = true;

			//	If the argument order is prefix of the partition ordering,
			//	then we can just use the partition ordering.
			auto &wexpr = executor.wexpr;
			auto &arg_orders = executor.wexpr.ArgOrders();
			const auto optimize = Settings::Get<EnableOptimizerSetting>(client);
			if (!optimize || BoundWindowExpression::GetSharedOrders(wexpr.OrderBy(), arg_orders) != arg_orders.size()) {
				//	"The ROW_NUMBER function can be computed by disambiguating duplicate elements based on their
				//	position in the input data, such that two elements never compare as equal."
				token_tree = make_uniq<WindowTokenTree>(client, executor.wexpr.ArgOrders(), executor.arg_order_idx,
				                                        payload_count, true);
			}
		}
	}

	//! Use framing instead of partitions (ORDER BY arguments)
	bool use_framing = false;

	//! The token tree for ORDER BY arguments
	unique_ptr<WindowTokenTree> token_tree;
};

//===--------------------------------------------------------------------===//
// WindowRowNumberLocalState
//===--------------------------------------------------------------------===//
class WindowRowNumberLocalState : public WindowExecutorLocalState {
public:
	WindowRowNumberLocalState(ExecutionContext &context, const WindowRowNumberGlobalState &grstate)
	    : WindowExecutorLocalState(context, grstate), grstate(grstate) {
		if (grstate.token_tree) {
			local_tree = grstate.token_tree->GetLocalState(context);
		}
	}

	//! Accumulate the secondary sort values
	static void Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	                   OperatorSinkInput &sink);
	//! Finish the sinking and prepare to scan
	static void Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink);

	//! The corresponding global peer state
	const WindowRowNumberGlobalState &grstate;
	//! The optional sorting state for secondary sorts
	unique_ptr<LocalSinkState> local_tree;
};

void WindowRowNumberLocalState::Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                       idx_t input_idx, OperatorSinkInput &sink) {
	auto &local_tree = sink.local_state.Cast<WindowRowNumberLocalState>().local_tree;
	if (local_tree) {
		auto &local_tokens = local_tree->Cast<WindowMergeSortTreeLocalState>();
		local_tokens.Sink(context, sink_chunk, input_idx, nullptr, 0, sink.interrupt_state);
	}
}

void WindowRowNumberLocalState::Finalizer(ExecutionContext &context, CollectionPtr collection,
                                          OperatorSinkInput &sink) {
	auto &local_tree = sink.local_state.Cast<WindowRowNumberLocalState>().local_tree;
	if (local_tree) {
		auto &local_tokens = local_tree->Cast<WindowMergeSortTreeLocalState>();
		local_tokens.Finalize(context, sink.interrupt_state);
		local_tokens.window_tree.Build(context.client);
	}
}

//===--------------------------------------------------------------------===//
// WindowRowNumberExecutor
//===--------------------------------------------------------------------===//
class WindowRowNumberStreamingState : public WindowExecutorStreamingState {
public:
	WindowRowNumberStreamingState() : vec(Value((int64_t)1), count_t(1)) {
	}

	void Evaluate(idx_t count, Vector &result) {
		// Set row numbers
		auto &row_number = *FlatVector::GetDataMutable<int64_t>(vec);
		auto rdata = FlatVector::Writer<int64_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			rdata.WriteValue(row_number++);
		}
	}
	Vector vec;
};

struct WindowRowNumberExecutor {
	//! Blocking APIs
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);
	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared);

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask);
	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		return true;
	}
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowRowNumberStreamingState>();
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state) {
		state.Cast<WindowRowNumberStreamingState>().Evaluate(input.size(), result);
	}
};

WindowFunction RowNumberFun::GetFunction() {
	WindowFunction fun(
	    Name, {}, LogicalType::BIGINT, ExpressionType::WINDOW_ROW_NUMBER, nullptr, WindowRowNumberExecutor::GetBounds,
	    WindowRowNumberExecutor::GetSharing, WindowRowNumberExecutor::GetGlobal, WindowRowNumberExecutor::GetLocal,
	    WindowRowNumberLocalState::Sinker, WindowRowNumberLocalState::Finalizer, WindowRowNumberExecutor::GetData);
	fun.SetIsRanking(true);
	fun.SetCanStreamCallback(WindowRowNumberExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowRowNumberExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowRowNumberExecutor::StreamData);

	return fun;
}

void WindowRowNumberExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	if (wexpr.ArgOrders().empty()) {
		required.insert(PARTITION_BEGIN);
	} else {
		// Secondary orders need to know where the frame is
		required.insert(FRAME_BEGIN);
		required.insert(FRAME_END);
	}
}

void WindowRowNumberExecutor::GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
	const auto &wexpr = executor.wexpr;

	auto &child_idx = executor.child_idx;
	for (auto &child : wexpr.GetChildren()) {
		child_idx.emplace_back(shared.RegisterEvaluate(child));
	}

	auto &arg_order_idx = executor.arg_order_idx;
	for (const auto &order : wexpr.ArgOrders()) {
		arg_order_idx.emplace_back(shared.RegisterSink(order.expression));
	}
}

unique_ptr<GlobalSinkState> WindowRowNumberExecutor::GetGlobal(ClientContext &client, const WindowExecutor &executor,
                                                               const idx_t payload_count,
                                                               const ValidityMask &partition_mask,
                                                               const ValidityMask &order_mask) {
	return make_uniq<WindowRowNumberGlobalState>(client, executor, payload_count, partition_mask, order_mask);
}

unique_ptr<LocalSinkState> WindowRowNumberExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	return make_uniq<WindowRowNumberLocalState>(context, gstate.Cast<WindowRowNumberGlobalState>());
}

void WindowRowNumberExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                      Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &grstate = sink.global_state.Cast<WindowRowNumberGlobalState>();
	const auto count = bounds.size();
	auto rdata = FlatVector::Writer<int64_t>(result, count);

	if (grstate.use_framing) {
		auto frame_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
		auto frame_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
		if (grstate.token_tree) {
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				// Row numbers are unique ranks
				rdata.WriteValue(
				    UnsafeNumericCast<int64_t>(grstate.token_tree->Rank(frame_begin[i], frame_end[i], row_idx)));
			}
		} else {
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				const auto frame_idx = MaxValue(frame_begin[i], MinValue(row_idx, frame_end[i]));
				rdata.WriteValue(UnsafeNumericCast<int64_t>(frame_idx - frame_begin[i] + 1));
			}
		}
		return;
	}

	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		rdata.WriteValue(UnsafeNumericCast<int64_t>(row_idx - partition_begin[i] + 1));
	}
}

//===--------------------------------------------------------------------===//
// WindowNtileExecutor
//===--------------------------------------------------------------------===//
// NTILE is just scaled ROW_NUMBER
struct WindowNtileExecutor : public WindowRowNumberExecutor {
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);

	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);
};

class WindowNtileLocalState : public WindowRowNumberLocalState {
public:
	WindowNtileLocalState(ExecutionContext &context, const WindowRowNumberGlobalState &grstate)
	    : WindowRowNumberLocalState(context, grstate) {
	}
};

WindowFunction NtileFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::BIGINT, ExpressionType::WINDOW_NTILE, nullptr,
	                   WindowNtileExecutor::GetBounds, WindowNtileExecutor::GetSharing, WindowNtileExecutor::GetGlobal,
	                   WindowNtileExecutor::GetLocal, WindowNtileLocalState::Sinker, WindowNtileLocalState::Finalizer,
	                   WindowNtileExecutor::GetData);
	fun.SetIsRanking(true);
	fun.GetSignature().AddParameter("num_buckets", LogicalType::BIGINT);
	fun.SetFallible(); // non-positive bucket counts are rejected at runtime
	return fun;
}

void WindowNtileExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	if (wexpr.ArgOrders().empty()) {
		required.insert(PARTITION_BEGIN);
		required.insert(PARTITION_END);
	} else {
		// Secondary orders need to know where the frame is
		required.insert(FRAME_BEGIN);
		required.insert(FRAME_END);
	}
}

unique_ptr<LocalSinkState> WindowNtileExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	return make_uniq<WindowNtileLocalState>(context, gstate.Cast<WindowRowNumberGlobalState>());
}

void WindowNtileExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
                                  idx_t row_idx, OperatorSinkInput &sink) {
	auto &grstate = sink.global_state.Cast<WindowRowNumberGlobalState>();
	const auto count = bounds.size();

	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	if (grstate.use_framing) {
		// With secondary sorts, we restrict to the frame boundaries, but everything else should compute the same.
		partition_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
		partition_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
	}
	auto rdata = FlatVector::Writer<int64_t>(result, count);
	const auto ntile_idx = grstate.executor.child_idx[0];
	WindowInputExpression ntile_col(eval_chunk, ntile_idx);
	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		if (ntile_col.CellIsNull(i)) {
			rdata.WriteNull();
		} else {
			auto n_param = ntile_col.GetCell<int64_t>(i);
			if (n_param < 1) {
				throw InvalidInputException("Argument for ntile must be greater than zero");
			}
			const auto begin = partition_begin[i];
			const auto end = MaxValue(partition_end[i], begin);
			// With thanks from SQLite's ntileValueFunc()
			auto n_total = NumericCast<int64_t>(end - begin);
			if (n_total == 0) {
				rdata.WriteNull();
				continue;
			}
			if (n_param > n_total) {
				// more groups allowed than we have values
				// map every entry to a unique group
				n_param = n_total;
			}
			int64_t n_size = (n_total / n_param);
			// find the row idx within the group
			idx_t partition_idx = 0;
			if (grstate.token_tree) {
				partition_idx = MinValue(grstate.token_tree->Rank(begin, end, row_idx) - 1, idx_t(n_total - 1));
			} else {
				const auto frame_row_idx = MinValue(MaxValue(begin, row_idx), idx_t(end - 1));
				partition_idx = frame_row_idx - begin;
			}
			auto adjusted_row_idx = NumericCast<int64_t>(partition_idx);

			// now compute the ntile
			int64_t n_large = n_total - n_param * n_size;
			int64_t i_small = n_large * (n_size + 1);
			int64_t result_ntile;

			D_ASSERT((n_large * (n_size + 1) + (n_param - n_large) * n_size) == n_total);

			if (adjusted_row_idx < i_small) {
				result_ntile = 1 + adjusted_row_idx / (n_size + 1);
			} else {
				result_ntile = 1 + n_large + (adjusted_row_idx - i_small) / n_size;
			}
			// result has to be between [1, NTILE]
			D_ASSERT(result_ntile >= 1 && result_ntile <= n_param);
			rdata.WriteValue(result_ntile);
		}
	}
}

} // namespace duckdb
