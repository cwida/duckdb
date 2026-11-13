#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/function/window/window_token_tree.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/function/window/ranking_functions.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowPeerGlobalState
//===--------------------------------------------------------------------===//
class WindowPeerGlobalState : public WindowExecutorGlobalState {
public:
	WindowPeerGlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
	                      const ValidityMask &partition_mask, const ValidityMask &order_mask)
	    : WindowExecutorGlobalState(client, executor, payload_count, partition_mask, order_mask) {
		if (!executor.arg_order_idx.empty()) {
			use_framing = true;

			//	If the argument order is the entire partition ordering
			//	(and the optimizer is enabled), then we can just use the partition ordering.
			//	A prefix is not sufficient because peer boundaries use all of the ordering keys.
			auto &wexpr = executor.wexpr;
			auto &arg_orders = executor.wexpr.ArgOrders();
			auto &orders = wexpr.OrderBy();
			const auto optimize = Settings::Get<EnableOptimizerSetting>(client);
			const auto shared = BoundWindowExpression::GetSharedOrders(orders, arg_orders);
			if (!optimize || shared != arg_orders.size() || arg_orders.size() != orders.size()) {
				token_tree = make_uniq<WindowTokenTree>(client, arg_orders, executor.arg_order_idx, payload_count);
			}
		}
	}

	//! Use framing instead of partitions (ORDER BY arguments)
	bool use_framing = false;

	//! The token tree for ORDER BY arguments
	unique_ptr<WindowTokenTree> token_tree;
};

//===--------------------------------------------------------------------===//
// WindowPeerLocalState
//===--------------------------------------------------------------------===//
//	Base class for non-aggregate functions that use peer boundaries
class WindowPeerLocalState : public WindowExecutorLocalState {
public:
	WindowPeerLocalState(ExecutionContext &context, const WindowPeerGlobalState &gpstate)
	    : WindowExecutorLocalState(context, gpstate), gpstate(gpstate) {
		if (gpstate.token_tree) {
			local_tree = gpstate.token_tree->GetLocalState(context);
		}
	}

	//! Accumulate the secondary sort values
	static void Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	                   OperatorSinkInput &sink);
	//! Finish the sinking and prepare to scan
	static SourceResultType Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink);

	void NextRank(idx_t partition_begin, idx_t peer_begin, idx_t row_idx);

	idx_t row_idx = DConstants::INVALID_INDEX;
	uint64_t dense_rank = 1;
	uint64_t rank_equal = 0;
	uint64_t rank = 1;

	//! The corresponding global peer state
	const WindowPeerGlobalState &gpstate;
	//! The optional sorting state for secondary sorts
	unique_ptr<LocalSinkState> local_tree;
};

void WindowPeerLocalState::Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                  idx_t input_idx, OperatorSinkInput &sink) {
	auto &local_tree = sink.local_state.Cast<WindowPeerLocalState>().local_tree;
	if (local_tree) {
		auto &local_tokens = local_tree->Cast<WindowMergeSortTreeLocalState>();
		local_tokens.Sink(context, sink_chunk, input_idx, nullptr, 0, sink.interrupt_state);
	}
}

SourceResultType WindowPeerLocalState::Finalizer(ExecutionContext &context, CollectionPtr collection,
                                                 OperatorSinkInput &sink) {
	auto &local_tree = sink.local_state.Cast<WindowPeerLocalState>().local_tree;
	if (local_tree) {
		auto &local_tokens = local_tree->Cast<WindowMergeSortTreeLocalState>();
		const auto result = local_tokens.Finalize(context, sink.interrupt_state);
		if (result != SourceResultType::FINISHED) {
			return result;
		}
		return local_tokens.window_tree.Build(sink.interrupt_state);
	}
	return SourceResultType::FINISHED;
}

void WindowPeerLocalState::NextRank(idx_t partition_begin, idx_t peer_begin, idx_t row_idx) {
	if (partition_begin == row_idx) {
		dense_rank = 1;
		rank = 1;
		rank_equal = 0;
	} else if (peer_begin == row_idx) {
		dense_rank++;
		rank += rank_equal;
		rank_equal = 0;
	}
	rank_equal++;
}

//===--------------------------------------------------------------------===//
// WindowPeerStreamingState
//===--------------------------------------------------------------------===//
class WindowPeerStreamingState : public WindowExecutorStreamingState {
public:
	explicit WindowPeerStreamingState(const Value &val) : vec(val, count_t(STANDARD_VECTOR_SIZE)) {
	}

	void Evaluate(Vector &result) {
		// Reference constant vector
		result.Reference(vec);
	}
	Vector vec;
};

//===--------------------------------------------------------------------===//
// WindowPeerExecutor
//===--------------------------------------------------------------------===//
struct WindowPeerExecutor : public WindowExecutor {
	//! Blocking APIs
	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared);

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask);

	//! Streaming APIs
	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		return true;
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state) {
		state.Cast<WindowPeerStreamingState>().Evaluate(result);
	}
};

void WindowPeerExecutor::GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
	const auto &wexpr = executor.wexpr;
	auto &arg_order_idx = executor.arg_order_idx;
	for (const auto &order : wexpr.ArgOrders()) {
		arg_order_idx.emplace_back(shared.RegisterSink(order.expression));
	}
}
unique_ptr<GlobalSinkState> WindowPeerExecutor::GetGlobal(ClientContext &client, const WindowExecutor &executor,
                                                          const idx_t payload_count, const ValidityMask &partition_mask,
                                                          const ValidityMask &order_mask) {
	return make_uniq<WindowPeerGlobalState>(client, executor, payload_count, partition_mask, order_mask);
}

//===--------------------------------------------------------------------===//
// WindowRankExecutor
//===--------------------------------------------------------------------===//
struct WindowRankExecutor : public WindowPeerExecutor {
	//! Blocking APIs
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);

	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowPeerStreamingState>(Value((int64_t)1));
	}
};

class WindowRankLocalState : public WindowPeerLocalState {
public:
	WindowRankLocalState(ExecutionContext &context, const WindowPeerGlobalState &gpstate)
	    : WindowPeerLocalState(context, gpstate) {
	}
};

void WindowRankExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	if (wexpr.ArgOrders().empty()) {
		required.insert(PARTITION_BEGIN);
		required.insert(PEER_BEGIN);
	} else {
		// Secondary orders need to know where the frame is
		required.insert(FRAME_BEGIN);
		required.insert(FRAME_END);
		required.insert(PEER_BEGIN);
	}
}

WindowFunction RankFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::BIGINT, ExpressionType::WINDOW_RANK, nullptr,
	                   WindowRankExecutor::GetBounds, WindowRankExecutor::GetSharing, WindowRankExecutor::GetGlobal,
	                   WindowRankExecutor::GetLocal, WindowRankLocalState::Sinker, WindowRankLocalState::Finalizer,
	                   WindowRankExecutor::GetData);
	fun.SetIsRanking(true);
	fun.SetCanStreamCallback(WindowRankExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowRankExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowRankExecutor::StreamData);
	return fun;
}

unique_ptr<LocalSinkState> WindowRankExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	return make_uniq<WindowRankLocalState>(context, gstate.Cast<WindowPeerGlobalState>());
}

void WindowRankExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
                                 idx_t row_idx, OperatorSinkInput &sink) {
	auto &gpeer = sink.global_state.Cast<WindowPeerGlobalState>();
	auto &lpeer = sink.local_state.Cast<WindowPeerLocalState>();
	const auto count = bounds.size();
	auto rdata = FlatVector::Writer<int64_t>(result, count);

	if (gpeer.use_framing) {
		auto frame_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
		auto frame_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
		if (gpeer.token_tree) {
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				rdata.WriteValue(
				    UnsafeNumericCast<int64_t>(gpeer.token_tree->Rank(frame_begin[i], frame_end[i], row_idx)));
			}
		} else {
			auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				//	Clamp peer to the frame
				const auto frame_peer_begin = MaxValue(frame_begin[i], peer_begin[i]);
				rdata.WriteValue(UnsafeNumericCast<int64_t>((frame_peer_begin - frame_begin[i]) + 1));
			}
		}
		return;
	}

	//	Reset to "previous" row
	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
	lpeer.rank = (peer_begin[0] - partition_begin[0]) + 1;
	lpeer.rank_equal = (row_idx - peer_begin[0]);

	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		lpeer.NextRank(partition_begin[i], peer_begin[i], row_idx);
		rdata.WriteValue(UnsafeNumericCast<int64_t>(lpeer.rank));
	}
}

//===--------------------------------------------------------------------===//
// WindowDenseRankExecutor
//===--------------------------------------------------------------------===//
struct WindowDenseRankExecutor : public WindowPeerExecutor {
	//! Blocking APIs
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);

	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowPeerStreamingState>(Value((int64_t)1));
	}
};

class WindowDenseRankLocalState : public WindowPeerLocalState {
public:
	WindowDenseRankLocalState(ExecutionContext &context, const WindowPeerGlobalState &gpstate)
	    : WindowPeerLocalState(context, gpstate) {
	}
};

void WindowDenseRankExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	required.insert(PARTITION_BEGIN);
	required.insert(PEER_BEGIN);
}

WindowFunction DenseRankFun::GetFunction() {
	WindowFunction fun({}, LogicalType::BIGINT, ExpressionType::WINDOW_RANK_DENSE, nullptr,
	                   WindowDenseRankExecutor::GetBounds, nullptr, WindowDenseRankExecutor::GetGlobal,
	                   WindowDenseRankExecutor::GetLocal, nullptr, nullptr, WindowDenseRankExecutor::GetData);
	fun.SetCanOrderBy(false);
	fun.SetIsRanking(true);
	fun.SetCanStreamCallback(WindowDenseRankExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowDenseRankExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowDenseRankExecutor::StreamData);
	return fun;
}

unique_ptr<LocalSinkState> WindowDenseRankExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	return make_uniq<WindowDenseRankLocalState>(context, gstate.Cast<WindowPeerGlobalState>());
}

void WindowDenseRankExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                      Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gpeer = sink.global_state.Cast<WindowPeerGlobalState>();
	auto &lpeer = sink.local_state.Cast<WindowPeerLocalState>();
	const auto count = bounds.size();

	auto &order_mask = gpeer.order_mask;
	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
	auto rdata = FlatVector::Writer<int64_t>(result, count);

	//	Reset to "previous" row
	//	Resetting is slow because we have to rescan the mask.
	//	So check whether we are just picking up where we left off.
	//	This is common because the main window operator
	//	evaluates maximally sized runs for each hash group.
	if (lpeer.row_idx != row_idx) {
		lpeer.rank = (peer_begin[0] - partition_begin[0]) + 1;
		lpeer.rank_equal = (row_idx - peer_begin[0]);

		//	The previous dense rank is the number of order mask bits in [partition_begin, row_idx)
		lpeer.dense_rank = 0;

		auto order_begin = partition_begin[0];
		idx_t begin_idx;
		idx_t begin_offset;
		order_mask.GetEntryIndex(order_begin, begin_idx, begin_offset);

		auto order_end = row_idx;
		idx_t end_idx;
		idx_t end_offset;
		order_mask.GetEntryIndex(order_end, end_idx, end_offset);

		//	If they are in the same entry, just loop
		if (begin_idx == end_idx) {
			const auto entry = order_mask.GetValidityEntry(begin_idx);
			for (; begin_offset < end_offset; ++begin_offset) {
				lpeer.dense_rank += order_mask.RowIsValid(entry, begin_offset);
			}
		} else {
			// Count the ragged bits at the start of the partition
			if (begin_offset) {
				const auto entry = order_mask.GetValidityEntry(begin_idx);
				for (; begin_offset < order_mask.BITS_PER_VALUE; ++begin_offset) {
					lpeer.dense_rank += order_mask.RowIsValid(entry, begin_offset);
					++order_begin;
				}
				++begin_idx;
			}

			//	Count the the aligned bits.
			ValidityMask tail_mask(order_mask.GetData() + begin_idx, end_idx - begin_idx);
			lpeer.dense_rank += tail_mask.CountValid(order_end - order_begin);
		}
	}

	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		lpeer.NextRank(partition_begin[i], peer_begin[i], row_idx);
		rdata.WriteValue(NumericCast<int64_t>(lpeer.dense_rank));
	}

	//	Remember where we left off
	lpeer.row_idx = row_idx;
}

//===--------------------------------------------------------------------===//
// WindowPercentRankExecutor
//===--------------------------------------------------------------------===//
struct WindowPercentRankExecutor : public WindowPeerExecutor {
	//! Blocking APIs
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);

	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowPeerStreamingState>(Value((double)0));
	}
};

class WindowPercentRankLocalState : public WindowPeerLocalState {
public:
	WindowPercentRankLocalState(ExecutionContext &context, const WindowPeerGlobalState &gpstate)
	    : WindowPeerLocalState(context, gpstate) {
	}
};

void WindowPercentRankExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	if (wexpr.ArgOrders().empty()) {
		required.insert(PARTITION_BEGIN);
		required.insert(PARTITION_END);
		required.insert(PEER_BEGIN);
	} else {
		// Secondary orders need to know where the frame is
		required.insert(FRAME_BEGIN);
		required.insert(FRAME_END);
		required.insert(PEER_BEGIN);
	}
}
WindowFunction PercentRankFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::DOUBLE, ExpressionType::WINDOW_PERCENT_RANK, nullptr,
	                   WindowPercentRankExecutor::GetBounds, WindowPercentRankExecutor::GetSharing,
	                   WindowPercentRankExecutor::GetGlobal, WindowPercentRankExecutor::GetLocal,
	                   WindowPercentRankLocalState::Sinker, WindowPercentRankLocalState::Finalizer,
	                   WindowPercentRankExecutor::GetData);
	fun.SetIsRanking(true);
	fun.SetCanStreamCallback(WindowPercentRankExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowPercentRankExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowPercentRankExecutor::StreamData);
	return fun;
}

unique_ptr<LocalSinkState> WindowPercentRankExecutor::GetLocal(ExecutionContext &context,
                                                               const GlobalSinkState &gstate) {
	return make_uniq<WindowPercentRankLocalState>(context, gstate.Cast<WindowPeerGlobalState>());
}

static inline double PercentRank(const idx_t begin, const idx_t end, const uint64_t rank) {
	if (end <= begin + 1) {
		return 0;
	}
	const auto denom = static_cast<double>(NumericCast<int64_t>(end - begin - 1));
	return ((double)rank - 1) / denom;
}

void WindowPercentRankExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                        Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gpeer = sink.global_state.Cast<WindowPeerGlobalState>();
	auto &lpeer = sink.local_state.Cast<WindowPeerLocalState>();
	const auto count = bounds.size();
	auto rdata = FlatVector::Writer<double>(result, count);

	if (gpeer.use_framing) {
		auto frame_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
		auto frame_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
		if (gpeer.token_tree) {
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				const auto begin = frame_begin[i];
				const auto end = MaxValue(frame_end[i], begin);
				const auto rank = gpeer.token_tree->Rank(begin, end, row_idx);
				rdata.WriteValue(PercentRank(begin, end, rank));
			}
		} else {
			//	Clamp peer to the frame
			auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				const auto begin = frame_begin[i];
				const auto end = MaxValue(frame_end[i], begin);
				const auto frame_peer_begin = MaxValue(begin, peer_begin[i]);
				lpeer.rank = (frame_peer_begin - begin) + 1;
				rdata.WriteValue(PercentRank(begin, end, lpeer.rank));
			}
		}
		return;
	}

	//	Reset to "previous" row
	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
	lpeer.rank = (peer_begin[0] - partition_begin[0]) + 1;
	lpeer.rank_equal = (row_idx - peer_begin[0]);

	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		lpeer.NextRank(partition_begin[i], peer_begin[i], row_idx);
		rdata.WriteValue(PercentRank(partition_begin[i], partition_end[i], lpeer.rank));
	}
}

//===--------------------------------------------------------------------===//
// WindowCumeDistExecutor
//===--------------------------------------------------------------------===//
struct WindowCumeDistExecutor : public WindowPeerExecutor {
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);

	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);
};

class WindowCumeDistLocalState : public WindowPeerLocalState {
public:
	WindowCumeDistLocalState(ExecutionContext &context, const WindowPeerGlobalState &gpstate)
	    : WindowPeerLocalState(context, gpstate) {
	}
};

void WindowCumeDistExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	if (wexpr.ArgOrders().empty()) {
		required.insert(PARTITION_BEGIN);
		required.insert(PARTITION_END);
		required.insert(PEER_END);
	} else {
		// Secondary orders need to know where the frame is
		required.insert(FRAME_BEGIN);
		required.insert(FRAME_END);
		required.insert(PEER_END);
	}
}
WindowFunction CumeDistFun::GetFunction() {
	WindowFunction fun(
	    Name, {}, LogicalType::DOUBLE, ExpressionType::WINDOW_CUME_DIST, nullptr, WindowCumeDistExecutor::GetBounds,
	    WindowCumeDistExecutor::GetSharing, WindowCumeDistExecutor::GetGlobal, WindowCumeDistExecutor::GetLocal,
	    WindowCumeDistLocalState::Sinker, WindowCumeDistLocalState::Finalizer, WindowCumeDistExecutor::GetData);
	fun.SetIsRanking(true);
	//	Not streamable?
	return fun;
}

unique_ptr<LocalSinkState> WindowCumeDistExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	return make_uniq<WindowCumeDistLocalState>(context, gstate.Cast<WindowPeerGlobalState>());
}

static inline double CumeDist(const idx_t begin, const idx_t end, const idx_t peer_end) {
	if (end <= begin) {
		return 0;
	}
	const auto denom = static_cast<double>(NumericCast<int64_t>(end - begin));
	const auto num_begin = MaxValue(peer_end, begin);
	const auto num = static_cast<double>(num_begin - begin);
	return num / denom;
}

void WindowCumeDistExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                     Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gpeer = sink.global_state.Cast<WindowPeerGlobalState>();
	const auto count = bounds.size();
	auto rdata = FlatVector::Writer<double>(result, count);

	if (gpeer.use_framing) {
		auto frame_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
		auto frame_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
		if (gpeer.token_tree) {
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				const auto begin = frame_begin[i];
				const auto end = MaxValue(frame_end[i], begin);
				const auto peer_end = gpeer.token_tree->PeerEnd(begin, end, row_idx);
				rdata.WriteValue(CumeDist(begin, end, peer_end));
			}
		} else {
			auto peer_end = FlatVector::GetData<const idx_t>(bounds.data[PEER_END]);
			for (idx_t i = 0; i < count; ++i, ++row_idx) {
				const auto begin = frame_begin[i];
				const auto end = MaxValue(frame_end[i], begin);
				//	Clamp the peer end to the frame
				const auto frame_peer_end = MinValue(peer_end[i], end);
				rdata.WriteValue(CumeDist(begin, end, frame_peer_end));
			}
		}
		return;
	}

	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);
	auto peer_end = FlatVector::GetData<const idx_t>(bounds.data[PEER_END]);
	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		rdata.WriteValue(CumeDist(partition_begin[i], partition_end[i], peer_end[i]));
	}
}

} // namespace duckdb
