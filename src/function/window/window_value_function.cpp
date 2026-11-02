#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/interpolate.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/function/window/window_aggregator.hpp"
#include "duckdb/function/window/window_collection.hpp"
#include "duckdb/function/window/window_index_tree.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/function/window/window_token_tree.hpp"
#include "duckdb/function/window/value_functions.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowValueGlobalState
//===--------------------------------------------------------------------===//
class WindowValueGlobalState : public WindowExecutorGlobalState {
public:
	using WindowCollectionPtr = unique_ptr<WindowCollection>;
	WindowValueGlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
	                       const ValidityMask &partition_mask, const ValidityMask &order_mask)
	    : WindowExecutorGlobalState(client, executor, payload_count, partition_mask, order_mask),
	      value_idx(executor.child_idx[0]) {
		if (!executor.arg_order_idx.empty()) {
			value_tree =
			    make_uniq<WindowIndexTree>(client, executor.wexpr.ArgOrders(), executor.arg_order_idx, payload_count);
		}
	}

	//! The index of the value collection
	const column_t value_idx;

	//! Merge sort tree to map unfiltered row number to value
	unique_ptr<WindowIndexTree> value_tree;
};

//===--------------------------------------------------------------------===//
// WindowValueLocalState
//===--------------------------------------------------------------------===//

//! A class representing the state of the first_value, last_value and nth_value functions
class WindowValueLocalState : public WindowExecutorLocalState {
public:
	WindowValueLocalState(ExecutionContext &context, const WindowValueGlobalState &gvstate)
	    : WindowExecutorLocalState(context, gvstate), gvstate(gvstate), ignore_nulls(&all_valid) {
		WindowAggregatorLocalState::InitSubFrames(frames, gvstate.executor.wexpr.WindowExclude());

		if (gvstate.value_tree) {
			local_value = gvstate.value_tree->GetLocalState(context);
			if (gvstate.executor.wexpr.IgnoreNulls()) {
				sort_nulls.Initialize();
			}
		}
	}

	//! Accumulate the secondary sort values
	static void Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	                   OperatorSinkInput &sink);
	//! Finish the sinking and prepare to scan
	static void Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink);

	//! The corresponding global value state
	const WindowValueGlobalState &gvstate;
	//! The optional sorting state for secondary sorts
	unique_ptr<LocalSinkState> local_value;
	//! Reusable selection vector for NULLs
	SelectionVector sort_nulls;
	//! The frame boundaries, used for EXCLUDE
	SubFrames frames;

	//! The state used for reading the collection
	unique_ptr<WindowCursor> cursor;

	// IGNORE NULLS
	ValidityMask all_valid;
	optional_ptr<ValidityMask> ignore_nulls;
};

void WindowValueLocalState::Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                   idx_t input_idx, OperatorSinkInput &sink) {
	auto &lvstate = sink.local_state.Cast<WindowValueLocalState>();
	auto &local_value = lvstate.local_value;
	if (local_value) {
		const auto &gvstate = sink.global_state.Cast<WindowValueGlobalState>();
		idx_t filtered = 0;
		optional_ptr<SelectionVector> filter_sel;

		// If we need to IGNORE NULLS for the child, and there are NULLs,
		// then build an SV to hold them
		const auto coll_count = coll_chunk.size();
		const auto &values = coll_chunk.data[gvstate.value_idx];
		auto validity = values.Validity();
		auto &sort_nulls = lvstate.sort_nulls;
		if (gvstate.executor.wexpr.IgnoreNulls() && validity.CanHaveNull()) {
			for (sel_t i = 0; i < coll_count; ++i) {
				if (validity.IsValid(i)) {
					sort_nulls[filtered++] = i;
				}
			}
			filter_sel = &sort_nulls;
		}

		auto &value_state = local_value->Cast<WindowIndexTreeLocalState>();
		value_state.Sink(context, sink_chunk, input_idx, filter_sel, filtered, sink.interrupt_state);
	}
}

void WindowValueLocalState::Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink) {
	auto &gvstate = sink.global_state.Cast<WindowValueGlobalState>();
	auto &executor = gvstate.executor;
	const auto &value_idx = gvstate.value_idx;

	auto &lvstate = sink.local_state.Cast<WindowValueLocalState>();
	if (value_idx != DConstants::INVALID_INDEX && executor.wexpr.IgnoreNulls()) {
		lvstate.ignore_nulls = &collection->validities[value_idx];
	}

	auto &local_value = lvstate.local_value;
	if (local_value) {
		auto &value_state = local_value->Cast<WindowIndexTreeLocalState>();
		value_state.Finalize(context, sink.interrupt_state);
		value_state.index_tree.Build(context.client);
	}

	// Prepare to scan
	if (!lvstate.cursor && gvstate.value_idx != DConstants::INVALID_INDEX) {
		lvstate.cursor = make_uniq<WindowCursor>(*collection, gvstate.value_idx);
	}
}

//===--------------------------------------------------------------------===//
// WindowValueStreamingState
//===--------------------------------------------------------------------===//
class WindowValueStreamingState : public WindowExecutorStreamingState {
public:
	static Value GetFirstValue(ClientContext &client, DataChunk &input, const BoundWindowExpression &wexpr) {
		// Just execute the expression once
		ExpressionExecutor executor(client);
		executor.AddExpression(*wexpr.GetChildren()[0]);
		DataChunk result;
		result.Initialize(client, {wexpr.GetChildren()[0]->GetReturnType()});
		executor.Execute(input, result);

		return result.GetValue(0, 0);
	}

	WindowValueStreamingState(ClientContext &client, DataChunk &input, const BoundWindowExpression &wexpr)
	    : wexpr(wexpr), vec(GetFirstValue(client, input, wexpr), count_t(STANDARD_VECTOR_SIZE)),
	      sel(STANDARD_VECTOR_SIZE), eval(client) {
		eval.AddExpression(*wexpr.GetChildren()[0]);
		arg_chunk.Initialize(client, {wexpr.GetChildren()[0]->GetReturnType()});
	}

	Vector &EvalArg(DataChunk &input) {
		arg_chunk.Reset();
		eval.Execute(input, arg_chunk);
		return arg_chunk.data[0];
	}

	const BoundWindowExpression &wexpr;
	//! A constant vector holding the repeated value
	Vector vec;
	//! A reusable selection vector
	SelectionVector sel;
	//! An executor for computing the argument
	ExpressionExecutor eval;
	//! A reusable argument chunk
	DataChunk arg_chunk;
};

//===--------------------------------------------------------------------===//
// WindowValueExecutor
//===--------------------------------------------------------------------===//
struct WindowValueExecutor {
	//! Blocking APIs
	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input);
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);
	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared);

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask);
	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	//! Streaming APIs
	static bool ArgumentIsStreamable(const BoundWindowExpression &wexpr) {
		return !wexpr.IsVolatile();
	}
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowValueStreamingState>(client, input, wexpr);
	}
};

unique_ptr<FunctionData> WindowValueExecutor::Bind(BindWindowFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();

	function.SetReturnType(arguments[0]->GetReturnType());

	return nullptr;
}

void WindowValueExecutor::GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
	required.insert(FRAME_BEGIN);
	required.insert(FRAME_END);
}

void WindowValueExecutor::GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
	//	The children have to be handled separately because only the first one is global
	const auto &wexpr = executor.wexpr;
	D_ASSERT(!wexpr.GetChildren().empty());

	auto &child_idx = executor.child_idx;
	child_idx.emplace_back(shared.RegisterCollection(wexpr.GetChildren()[0], wexpr.IgnoreNulls()));

	if (wexpr.GetChildren().size() > 1) {
		child_idx.emplace_back(shared.RegisterEvaluate(wexpr.GetChildren()[1]));
	}
	if (wexpr.GetChildren().size() > 2) {
		child_idx.emplace_back(shared.RegisterEvaluate(wexpr.GetChildren()[2]));
	}
	auto &arg_order_idx = executor.arg_order_idx;
	for (const auto &order : wexpr.ArgOrders()) {
		arg_order_idx.emplace_back(shared.RegisterSink(order.expression));
	}
}

unique_ptr<GlobalSinkState> WindowValueExecutor::GetGlobal(ClientContext &client, const WindowExecutor &executor,
                                                           const idx_t payload_count,
                                                           const ValidityMask &partition_mask,
                                                           const ValidityMask &order_mask) {
	return make_uniq<WindowValueGlobalState>(client, executor, payload_count, partition_mask, order_mask);
}

unique_ptr<LocalSinkState> WindowValueExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	const auto &gvstate = gstate.Cast<WindowValueGlobalState>();
	return make_uniq<WindowValueLocalState>(context, gvstate);
}

//===--------------------------------------------------------------------===//
// WindowLeadLagGlobalState
//===--------------------------------------------------------------------===//
// The functions LEAD and LAG can be extended to a windowed version with
// two independent ORDER BY clauses just like first_value and other value
// functions.
// To evaluate a windowed LEAD/LAG, one has to (1) compute the ROW_NUMBER
// of the own row, (2) adjust the row number by adding or subtracting an
// offset, (3) find the row at that offset, and (4) evaluate the expression
// provided to LEAD/LAG on this row. One can use the algorithm from Section
// 4.4 to determine the row number of the own row (step 1) and the
// algorithm from Section 4.5 to find the row with the adjusted position
// (step 3). Both algorithms are in O(𝑛 log𝑛), so the overall algorithm
// for LEAD/LAG is also O(𝑛 log𝑛).
//
// 4.4: unique WindowTokenTree
// 4.5: WindowIndexTree

class WindowLeadLagGlobalState : public WindowValueGlobalState {
public:
	WindowLeadLagGlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
	                         const ValidityMask &partition_mask, const ValidityMask &order_mask)
	    : WindowValueGlobalState(client, executor, payload_count, partition_mask, order_mask) {
		if (value_tree) {
			use_framing = true;

			//	If the argument order is prefix of the partition ordering,
			//	then we can just use the partition ordering.
			auto &wexpr = executor.wexpr;
			auto &arg_orders = executor.wexpr.ArgOrders();
			const auto optimize = Settings::Get<EnableOptimizerSetting>(client);
			if (!optimize || BoundWindowExpression::GetSharedOrders(wexpr.OrderBy(), arg_orders) != arg_orders.size()) {
				//	"The ROW_NUMBER function can be computed by disambiguating duplicate elements based on their
				//	position in the input data, such that two elements never compare as equal."
				// 	Note: If the user specifies an partial secondary sort, the disambiguation will use the
				//	partition's row numbers, not the secondary sort's row numbers.
				row_tree = make_uniq<WindowTokenTree>(client, arg_orders, executor.arg_order_idx, payload_count, true);
			} else {
				// The value_tree is cheap to construct, so we just get rid of it if we now discover we don't need it.
				value_tree.reset();
			}
		}
	}

	//! Flag that we are using framing, even if we don't need the trees
	bool use_framing = false;

	//! Merge sort tree to map partition offset to row number (algorithm from Section 4.5)
	unique_ptr<WindowTokenTree> row_tree;
};

//===--------------------------------------------------------------------===//
// WindowLeadLagLocalState
//===--------------------------------------------------------------------===//
class WindowLeadLagLocalState : public WindowValueLocalState {
public:
	WindowLeadLagLocalState(ExecutionContext &context, const WindowLeadLagGlobalState &gstate)
	    : WindowValueLocalState(context, gstate) {
		if (gstate.row_tree) {
			local_row = gstate.row_tree->GetLocalState(context);
		}
	}

	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
		if (wexpr.ArgOrders().empty()) {
			required.insert(PARTITION_BEGIN);
			required.insert(PARTITION_END);
		} else {
			// Secondary orders need to know where the frame is
			required.insert(FRAME_BEGIN);
			required.insert(FRAME_END);
		}
	}

	//! Accumulate the secondary sort values
	static void Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	                   OperatorSinkInput &sink);
	//! Finish the sinking and prepare to scan
	static void Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink);

	//! The optional sorting state for the secondary sort row mapping
	unique_ptr<LocalSinkState> local_row;
};

void WindowLeadLagLocalState::Sinker(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                     idx_t input_idx, OperatorSinkInput &sink) {
	WindowValueLocalState::Sinker(context, sink_chunk, coll_chunk, input_idx, sink);

	auto &local_row = sink.local_state.Cast<WindowLeadLagLocalState>().local_row;
	if (local_row) {
		idx_t filtered = 0;
		optional_ptr<SelectionVector> filter_sel;

		auto &row_state = local_row->Cast<WindowMergeSortTreeLocalState>();
		row_state.Sink(context, sink_chunk, input_idx, filter_sel, filtered, sink.interrupt_state);
	}
}

void WindowLeadLagLocalState::Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink) {
	WindowValueLocalState::Finalizer(context, collection, sink);

	auto &local_row = sink.local_state.Cast<WindowLeadLagLocalState>().local_row;
	if (local_row) {
		auto &row_state = local_row->Cast<WindowMergeSortTreeLocalState>();
		row_state.Finalize(context, sink.interrupt_state);
		row_state.window_tree.Build(context.client);
	}
}

//===--------------------------------------------------------------------===//
// WindowLeadLagStreamingState
//===--------------------------------------------------------------------===//
class WindowLeadLagStreamingState : public WindowExecutorStreamingState {
public:
	static bool ComputeOffset(ClientContext &client, const BoundWindowExpression &wexpr, int64_t &offset) {
		offset = 1;
		if (wexpr.GetChildren().size() > 1) {
			auto &offset_expr = wexpr.GetChildren()[1];
			if (offset_expr->HasParameter() || !offset_expr->IsFoldable()) {
				return false;
			}
			auto offset_value = ExpressionExecutor::EvaluateScalar(client, *offset_expr);
			if (offset_value.IsNull()) {
				return false;
			}
			auto bigint_value = offset_value.DefaultTryCastAs(LogicalType::BIGINT);
			if (!bigint_value) {
				return false;
			}
			offset = bigint_value->GetValue<int64_t>();
			if (offset == NumericLimits<int64_t>::Minimum()) {
				return false;
			}
		}

		//	We can only support LEAD and LAG values within one standard vector
		if (wexpr.GetExpressionType() == ExpressionType::WINDOW_LEAD) {
			offset = -offset;
		}
		return true;
	}

	static bool ComputeDefault(ClientContext &client, const BoundWindowExpression &wexpr, Value &result) {
		if (wexpr.GetChildren().size() < 3) {
			result = Value(wexpr.GetReturnType());
			return true;
		}

		auto &default_expr = wexpr.GetChildren()[2];
		if (default_expr && (default_expr->HasParameter() || !default_expr->IsFoldable())) {
			return false;
		}
		auto dflt_value = ExpressionExecutor::EvaluateScalar(client, *default_expr);
		auto cast_value = dflt_value.DefaultTryCastAs(wexpr.GetReturnType());
		if (!cast_value) {
			return false;
		}
		result = std::move(*cast_value);
		return true;
	}

	WindowLeadLagStreamingState(ClientContext &context, const BoundWindowExpression &wexpr)
	    : wexpr(wexpr), executor(context, *wexpr.GetChildren()[0]), prev(wexpr.GetReturnType()),
	      temp(wexpr.GetReturnType()), sel(STANDARD_VECTOR_SIZE) {
		ComputeOffset(context, wexpr, offset);
		ComputeDefault(context, wexpr, dflt);

		buffered = idx_t(std::abs(offset));
		prev.Reference(dflt, count_t(buffered));
		prev.Flatten();
		temp.Initialize(VectorDataInitialization::UNINITIALIZED, buffered);
	}

	void Execute(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	             Vector &result) {
		if (!curr_chunk.ColumnCount()) {
			curr_chunk.Initialize(context.client, {result.GetType()},
			                      MaxValue<idx_t>(STANDARD_VECTOR_SIZE, delayed_capacity));
		}

		if (offset >= 0) {
			ExecuteLag(context, input, result);
		} else {
			ExecuteLead(context, input, delayed, result);
		}
	}

	void ExecuteLag(ExecutionContext &context, DataChunk &input, Vector &result) {
		D_ASSERT(offset >= 0);
		const auto &curr = curr_chunk.data[0];
		curr_chunk.Reset();
		executor.Execute(input, curr_chunk);
		const idx_t count = input.size();
		//	Copy prev[0, buffered] => result[0, buffered]
		idx_t source_count = MinValue<idx_t>(buffered, count);
		VectorOperations::Copy(prev, result, source_count, 0, 0);
		// Special case when we have buffered enough values for the output
		if (count < buffered) {
			//	Shift down incomplete buffers
			//	Copy prev[count, buffered] => temp[0, buffered-count]
			source_count = buffered - count;
			FlatVector::ValidityMutable(temp).Reset();
			VectorOperations::Copy(prev, temp, buffered, count, 0);

			// 	Copy temp[0, buffered-count] => prev[0, buffered-count]
			FlatVector::ValidityMutable(prev).Reset();
			VectorOperations::Copy(temp, prev, source_count, 0, 0);
			// 	Copy curr[0, count] => prev[buffered-count, buffered]
			VectorOperations::Copy(curr, prev, count, 0, source_count);
		} else {
			//	Copy input values beyond what we have buffered
			source_count = count - buffered;
			//	Copy curr[0, count-buffered] => result[buffered, count]
			VectorOperations::Copy(curr, result, source_count, 0, buffered);
			// 	Copy curr[count-buffered, count] => prev[0, buffered]
			FlatVector::ValidityMutable(prev).Reset();
			VectorOperations::Copy(curr, prev, count, source_count, 0);
		}
	}

	void ExecuteLead(ExecutionContext &context, DataChunk &input, DataChunk &delayed, Vector &result) {
		//	We treat input || delayed as a logical unified buffer
		D_ASSERT(offset < 0);
		// Input has been set up with the number of rows we CAN produce.
		const idx_t count = input.size();
		const auto &curr = curr_chunk.data[0];
		// Copy unified[buffered:count] => result[pos:]
		idx_t pos = 0;
		idx_t unified_offset = buffered;
		if (unified_offset < count) {
			curr_chunk.Reset();
			executor.Execute(input, curr_chunk);
			VectorOperations::Copy(curr, result, count, unified_offset, pos);
			pos += count - unified_offset;
			unified_offset = count;
		}
		// Copy unified[unified_offset:] => result[pos:]
		idx_t unified_count = count + delayed.size();
		if (unified_offset < unified_count) {
			curr_chunk.Reset();
			executor.Execute(delayed, curr_chunk);
			idx_t delayed_offset = unified_offset - count;
			// Only copy as many values as we need
			idx_t delayed_count = MinValue<idx_t>(delayed.size(), delayed_offset + (count - pos));
			VectorOperations::Copy(curr, result, delayed_count, delayed_offset, pos);
			pos += delayed_count - delayed_offset;
		}
		// Copy default[:count-pos] => result[pos:]
		if (pos < count) {
			const idx_t defaulted = count - pos;
			VectorOperations::Copy(prev, result, defaulted, 0, pos);
		}
	}

	//! The aggregate expression
	const BoundWindowExpression &wexpr;
	//! Cache the executor to cut down on memory allocation
	ExpressionExecutor executor;
	//! The number of rows we have buffered
	idx_t buffered;
	//! The constant default value
	Value dflt;
	//! The current set of values
	DataChunk curr_chunk;
	//! The previous set of values
	Vector prev;
	//! The copy buffer
	Vector temp;
	//! A temporary selection vector
	SelectionVector sel;
};

//===--------------------------------------------------------------------===//
// WindowLeadLagExecutor
//===--------------------------------------------------------------------===//
struct WindowLeadLagExecutor : public WindowValueExecutor {
public:
	//! Blocking APIs
	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input);

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask);
	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		// We can stream LEAD/LAG if the arguments are constant and the delta is less than a block behind
		if (!wexpr.IgnoreNulls()) {
			Value dflt;
			if (!WindowLeadLagStreamingState::ComputeDefault(client, wexpr, dflt)) {
				return false;
			}
			int64_t offset;
			if (!WindowLeadLagStreamingState::ComputeOffset(client, wexpr, offset)) {
				return false;
			}
			if (offset < 0 && !ArgumentIsStreamable(wexpr)) {
				return false;
			}

			return UnsafeNumericCast<idx_t>(std::abs(offset)) < max_delta;
		}
		return false;
	}
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowLeadLagStreamingState>(client, wexpr);
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state) {
		state.Cast<WindowLeadLagStreamingState>().Execute(context, input, delayed, delayed_capacity, result);
	}
};

unique_ptr<FunctionData> WindowLeadLagExecutor::Bind(BindWindowFunctionInput &input) {
	WindowValueExecutor::Bind(input);

	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();

	if (arguments.size() > 2) {
		function.GetArguments()[2] = function.GetReturnType();
	}

	return nullptr;
}

static WindowFunction GetLeadLagFunction(const char *name, const ExpressionType &type) {
	const auto bind = WindowLeadLagExecutor::Bind;
	const auto bounds = WindowLeadLagLocalState::GetBounds;
	const auto sharing = WindowLeadLagExecutor::GetSharing;
	const auto global = WindowLeadLagExecutor::GetGlobal;
	const auto local = WindowLeadLagExecutor::GetLocal;
	const auto sink = WindowLeadLagLocalState::Sinker;
	const auto finalize = WindowLeadLagLocalState::Finalizer;
	const auto evaluate = WindowLeadLagExecutor::GetData;

	WindowFunction func(name, {}, LogicalType::ANY, type, bind, bounds, sharing, global, local, sink, finalize,
	                    evaluate);
	auto &sig = func.GetSignature();

	// Type of the default value is not actually T, as it is force-cast to the return type - not unified.
	sig.AddParameter("col", LogicalType::TEMPLATE("T"));
	sig.AddParameter("offset", LogicalType::BIGINT, Value::BIGINT(1));
	sig.AddParameter("default", LogicalTypeId::ANY, Value(LogicalTypeId::SQLNULL));
	sig.SetReturnType(LogicalType::TEMPLATE("T"));

	func.SetCanStreamCallback(WindowLeadLagExecutor::CanStream);
	func.SetStreamingStateCallback(WindowLeadLagExecutor::GetStreamingState);
	func.SetStreamingDataCallback(WindowLeadLagExecutor::StreamData);

	return func;
}

WindowFunction LeadFun::GetFunction() {
	return GetLeadLagFunction(Name, ExpressionType::WINDOW_LEAD);
}

WindowFunction LeadFun::GetTypedFunction(const LogicalType &type, idx_t nargs) {
	auto func = GetLeadLagFunction(Name, ExpressionType::WINDOW_LEAD);
	func.GetSignature().GetParameter(0).SetType(type);
	if (nargs > 2) {
		func.GetSignature().GetParameter(2).SetType(type);
	}
	func.GetSignature().SetReturnType(type);
	return func;
}

WindowFunction LagFun::GetFunction() {
	return GetLeadLagFunction(Name, ExpressionType::WINDOW_LAG);
}

unique_ptr<GlobalSinkState> WindowLeadLagExecutor::GetGlobal(ClientContext &client, const WindowExecutor &executor,
                                                             const idx_t payload_count,
                                                             const ValidityMask &partition_mask,
                                                             const ValidityMask &order_mask) {
	return make_uniq<WindowLeadLagGlobalState>(client, executor, payload_count, partition_mask, order_mask);
}

unique_ptr<LocalSinkState> WindowLeadLagExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	const auto &glstate = gstate.Cast<WindowLeadLagGlobalState>();
	return make_uniq<WindowLeadLagLocalState>(context, glstate);
}

//! The index of the row at the offset - saturated if it does not fit, as it is outside the partition anyway
static int64_t LeadLagIndex(ExpressionType type, int64_t row_idx, int64_t offset) {
	int64_t result;
	if (type == ExpressionType::WINDOW_LEAD) {
		if (!TryAddOperator::Operation(row_idx, offset, result)) {
			result = offset < 0 ? NumericLimits<int64_t>::Minimum() : NumericLimits<int64_t>::Maximum();
		}
	} else if (!TrySubtractOperator::Operation(row_idx, offset, result)) {
		result = offset < 0 ? NumericLimits<int64_t>::Maximum() : NumericLimits<int64_t>::Minimum();
	}
	return result;
}

void WindowLeadLagExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
                                    idx_t row_idx, OperatorSinkInput &sink) {
	auto &glstate = sink.global_state.Cast<WindowLeadLagGlobalState>();
	auto &llstate = sink.local_state.Cast<WindowLeadLagLocalState>();
	const auto count = bounds.size();
	auto &cursor = *llstate.cursor;

	auto &wexpr = glstate.executor.wexpr;
	auto &child_idx = glstate.executor.child_idx;
	const bool has_offset = (child_idx.size() > 1);
	const bool has_default = (child_idx.size() > 2);

	const idx_t offset_idx = has_offset ? child_idx[1] : DConstants::INVALID_INDEX;
	const idx_t default_idx = has_default ? child_idx[2] : DConstants::INVALID_INDEX;

	WindowInputExpression leadlag_offset(eval_chunk, offset_idx);
	WindowInputExpression leadlag_default(eval_chunk, default_idx);

	auto frame_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
	auto frame_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);

	if (glstate.row_tree) {
		// TODO: Handle subframes (SelectNth can handle it but Rank can't)
		auto &frames = llstate.frames;
		frames.resize(1);
		auto &frame = frames[0];
		for (idx_t i = 0; i < count; ++i, ++row_idx) {
			int64_t offset = 1;
			if (has_offset) {
				if (leadlag_offset.CellIsNull(i)) {
					FlatVector::SetNull(result, i, true);
					continue;
				}
				offset = leadlag_offset.GetCell<int64_t>(i);
			}

			// (1) compute the ROW_NUMBER of the own row
			frame = FrameBounds(frame_begin[i], MaxValue(frame_end[i], frame_begin[i]));
			const auto own_row = glstate.row_tree->Rank(frame.start, frame.end, row_idx) - 1;
			// (2) adjust the row number by adding or subtracting an offset
			auto val_idx = LeadLagIndex(wexpr.GetExpressionType(), NumericCast<int64_t>(own_row), offset);
			const auto is_lead = (wexpr.GetExpressionType() == ExpressionType::WINDOW_LEAD);
			if (row_idx < frame.start || frame.end <= row_idx) {
				//	An own row outside the frame ranks just before own_row instead of at it,
				//	so a zero shift is the own row itself and a forward shift lands one row earlier.
				if (!offset) {
					cursor.CopyCell(0, row_idx, result, i);
					continue;
				}
				if (is_lead == (offset > 0)) {
					--val_idx;
				}
			}
			const auto frame_width = NumericCast<int64_t>(frame.end - frame.start);
			if (val_idx >= 0 && val_idx < frame_width) {
				// (3) find the row at that offset
				const auto n = NumericCast<idx_t>(val_idx);
				const auto nth_index = glstate.value_tree->SelectNth(frames, n);
				// (4) evaluate the expression provided to LEAD/LAG on this row.
				if (nth_index.second) {
					//	Overflow
					FlatVector::SetNull(result, i, true);
				} else {
					cursor.CopyCell(0, nth_index.first, result, i);
				}
			} else if (has_default) {
				leadlag_default.CopyCell(result, i);
			} else {
				FlatVector::SetNull(result, i, true);
			}
		}
		return;
	}

	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);

	// Only shift within the frame if we are using a shared ordering clause.
	if (glstate.use_framing) {
		partition_begin = frame_begin;
		partition_end = frame_end;
	}

	// We can't shift if we are ignoring NULLs (the rows may not be contiguous)
	// or if we are using framing (the frame may change on each row)
	auto &ignore_nulls = llstate.ignore_nulls;
	bool can_shift = ignore_nulls->CannotHaveNull() && !glstate.use_framing;
	if (has_offset) {
		can_shift = can_shift && wexpr.GetChildren()[1]->IsFoldable();
	}
	if (has_default) {
		can_shift = can_shift && wexpr.GetChildren()[2]->IsFoldable();
	}

	const auto row_end = row_idx + count;
	for (idx_t i = 0; i < count;) {
		int64_t offset = 1;
		if (offset_idx != DConstants::INVALID_INDEX) {
			if (leadlag_offset.CellIsNull(i)) {
				FlatVector::SetNull(result, i, true);
				++i;
				++row_idx;
				continue;
			}
			offset = leadlag_offset.GetCell<int64_t>(i);
		}
		auto val_idx = LeadLagIndex(wexpr.GetExpressionType(), NumericCast<int64_t>(row_idx), offset);

		idx_t delta = 0;
		if (val_idx < (int64_t)row_idx) {
			// Count backwards, starting from the last row at or before the own row
			delta = idx_t(row_idx - idx_t(val_idx));
			const auto lo = partition_begin[i];
			const auto hi = MinValue(row_idx, partition_end[i]);
			if (lo < hi) {
				val_idx = int64_t(WindowBoundariesState::FindPrevStart(*ignore_nulls, lo, hi, delta));
			}
		} else if (val_idx > (int64_t)row_idx) {
			// Count forwards, starting from the first row at or after the own row
			delta = idx_t(idx_t(val_idx) - row_idx);
			const auto lo = MaxValue(row_idx + 1, partition_begin[i]);
			const auto hi = partition_end[i];
			if (lo < hi) {
				val_idx = int64_t(WindowBoundariesState::FindNextStart(*ignore_nulls, lo, hi, delta));
			}
		}
		// else offset is zero, so don't move.

		if (can_shift) {
			const auto target_limit = MinValue(partition_end[i], row_end) - row_idx;
			if (!delta) {
				//	Copy source[index:index+width] => result[i:]
				auto index = NumericCast<idx_t>(val_idx);
				const auto source_limit = partition_end[i] - index;
				auto width = MinValue(source_limit, target_limit);
				// We may have to scan multiple blocks here, so loop until we have copied everything
				const idx_t col_idx = 0;
				while (width) {
					const auto source_offset = cursor.Seek(index);
					const auto &source = cursor.chunk.data[col_idx];
					const auto copied = MinValue<idx_t>(cursor.chunk.size() - source_offset, width);
					VectorOperations::Copy(source, result, source_offset + copied, source_offset, i);
					i += copied;
					row_idx += copied;
					index += copied;
					width -= copied;
				}
			} else if (has_default) {
				const auto width = MinValue(delta, target_limit);
				leadlag_default.CopyCell(result, i, width);
				i += width;
				row_idx += width;
			} else {
				for (idx_t nulls = MinValue(delta, target_limit); nulls--; ++i, ++row_idx) {
					FlatVector::SetNull(result, i, true);
				}
			}
		} else {
			if (!delta) {
				cursor.CopyCell(0, NumericCast<idx_t>(val_idx), result, i);
			} else if (has_default) {
				leadlag_default.CopyCell(result, i);
			} else {
				FlatVector::SetNull(result, i, true);
			}
			++i;
			++row_idx;
		}
	}
}

//===--------------------------------------------------------------------===//
// WindowFirstValueExecutor
//===--------------------------------------------------------------------===//
struct WindowFirstValueExecutor : public WindowValueExecutor {
	//! Blocking APIs
	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		if (!ArgumentIsStreamable(wexpr)) {
			return false;
		}
		if (wexpr.IgnoreNulls()) {
			// We can stream first values ignoring NULLs if they are "running totals"
			return wexpr.WindowStart() == WindowBoundary::UNBOUNDED_PRECEDING &&
			       wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_ROWS;
		}
		// We can stream first values if the frame start is fixed at the partition start
		return wexpr.WindowStart() == WindowBoundary::UNBOUNDED_PRECEDING &&
		       (wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_ROWS ||
		        wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_RANGE ||
		        wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_GROUPS ||
		        wexpr.WindowEnd() == WindowBoundary::UNBOUNDED_FOLLOWING);
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state);
};

void WindowFirstValueExecutor::StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed,
                                          idx_t delayed_capacity, Vector &result, WindowExecutorStreamingState &state) {
	auto &sstate = state.Cast<WindowValueStreamingState>();
	auto &wexpr = sstate.wexpr;
	const auto count = input.size();

	// If we are ignoring NULLs and we started with a NULL,
	// then look for a non-NULL value and update it
	if (wexpr.IgnoreNulls() && ConstantVector::IsNull(sstate.vec)) {
		//	Find the first non-NULL value
		auto &arg = sstate.EvalArg(input);
		UnifiedVectorFormat unified;
		arg.ToUnifiedFormat(unified);
		const auto &validity = unified.validity;
		auto &prev = sstate.vec;
		if (validity.CannotHaveNull()) {
			prev.Reference(arg.GetValue(0), count_t(count));
			result.Reference(prev);
		} else {
			auto &sel = sstate.sel;
			Vector split(wexpr.GetChildren()[0]->GetReturnType());
			split.SetValue(0, prev.GetValue(0));
			sel_t s = 0;
			for (sel_t i = 0; i < count; ++i) {
				if (!s && validity.RowIsValidUnsafe(unified.sel->get_index(i))) {
					auto v = arg.GetValue(i);
					prev.Reference(v, count_t(count));
					s = 1;
					split.SetValue(s, v);
				}
				sel.set_index(i, s);
			}
			result.Slice(split, sel, count);
		}
	} else {
		// Reference constant vector
		result.Reference(sstate.vec);
	}
}

WindowFunction FirstValueFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::ANY, ExpressionType::WINDOW_FIRST_VALUE, WindowFirstValueExecutor::Bind,
	                   WindowFirstValueExecutor::GetBounds, WindowFirstValueExecutor::GetSharing,
	                   WindowFirstValueExecutor::GetGlobal, WindowFirstValueExecutor::GetLocal,
	                   WindowValueLocalState::Sinker, WindowValueLocalState::Finalizer,
	                   WindowFirstValueExecutor::GetData);
	fun.GetSignature().AddParameter("expr", LogicalTypeId::ANY);
	fun.SetCanStreamCallback(WindowFirstValueExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowFirstValueExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowFirstValueExecutor::StreamData);
	fun.SetCanExclude(true);
	return fun;
}

void WindowFirstValueExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                       Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gvstate = sink.global_state.Cast<WindowValueGlobalState>();
	auto &lvstate = sink.local_state.Cast<WindowValueLocalState>();
	auto &cursor = *lvstate.cursor;
	const auto count = bounds.size();
	auto &frames = lvstate.frames;
	auto &ignore_nulls = *lvstate.ignore_nulls;
	auto exclude_mode = gvstate.executor.wexpr.WindowExclude();
	WindowAggregator::EvaluateSubFrames(bounds, exclude_mode, count, row_idx, frames, [&](idx_t i) {
		if (gvstate.value_tree) {
			idx_t frame_width = 0;
			for (const auto &frame : frames) {
				frame_width += frame.end - frame.start;
			}

			if (frame_width) {
				const auto first_idx = gvstate.value_tree->SelectNth(frames, 0);
				if (first_idx.second || first_idx.first >= cursor.Count()) {
					//	No first value - give up.
					FlatVector::SetNull(result, i, true);
				} else {
					cursor.CopyCell(0, first_idx.first, result, i);
				}
			} else {
				FlatVector::SetNull(result, i, true);
			}
			return;
		}

		for (const auto &frame : frames) {
			if (frame.start >= frame.end) {
				continue;
			}

			//	Same as NTH_VALUE(..., 1)
			idx_t n = 1;
			const auto first_idx = WindowBoundariesState::FindNextStart(ignore_nulls, frame.start, frame.end, n);
			if (!n) {
				cursor.CopyCell(0, first_idx, result, i);
				return;
			}
		}

		// Didn't find one
		FlatVector::SetNull(result, i, true);
	});
}

//===--------------------------------------------------------------------===//
// WindowLastValueExecutor
//===--------------------------------------------------------------------===//
struct WindowLastValueExecutor : public WindowValueExecutor {
	//! Blocking APIs
	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		if (!ArgumentIsStreamable(wexpr)) {
			return false;
		}
		// We can stream last values if they are "running totals"
		return wexpr.WindowStart() == WindowBoundary::UNBOUNDED_PRECEDING &&
		       wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_ROWS;
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state);
};

void WindowLastValueExecutor::StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed,
                                         idx_t delayed_capacity, Vector &result, WindowExecutorStreamingState &state) {
	//	Evaluate the argument and copy the values
	auto &sstate = state.Cast<WindowValueStreamingState>();
	auto &wexpr = sstate.wexpr;
	const auto count = input.size();
	auto &executor = sstate.eval;
	if (wexpr.IgnoreNulls()) {
		auto &prev = sstate.vec;
		auto &arg = sstate.EvalArg(input);
		UnifiedVectorFormat unified;
		arg.ToUnifiedFormat(unified);
		const auto &validity = unified.validity;
		if (validity.CannotHaveNull()) {
			VectorOperations::Copy(arg, result, count, 0, 0);
		} else {
			//	Copy the data as it may be a reference to the argument
			Vector copy(wexpr.GetChildren()[0]->GetReturnType());
			VectorOperations::Copy(arg, copy, count, 0, 0);
			//	Overwrite the previous non-NULL value if the first one is NULL
			if (!validity.RowIsValidUnsafe(unified.sel->get_index(0))) {
				VectorOperations::Copy(prev, copy, 1, 0, 0);
			}
			//	Select appropriate the non-NULL values to copy over
			auto &sel = sstate.sel;
			sel_t non_null = 0;
			for (sel_t i = 0; i < count; ++i) {
				if (validity.RowIsValidUnsafe(unified.sel->get_index(i))) {
					non_null = i;
				}
				sel.set_index(i, non_null);
			}
			result.Slice(copy, sel, count);
		}
		//	Remember the last non-NULL value for the next iteration
		prev.Reference(result.GetValue(count - 1), count_t(count));
	} else {
		executor.ExecuteExpression(input, result);
	}
}

WindowFunction LastValueFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::ANY, ExpressionType::WINDOW_LAST_VALUE, WindowLastValueExecutor::Bind,
	                   WindowLastValueExecutor::GetBounds, WindowLastValueExecutor::GetSharing,
	                   WindowLastValueExecutor::GetGlobal, WindowLastValueExecutor::GetLocal,
	                   WindowValueLocalState::Sinker, WindowValueLocalState::Finalizer,
	                   WindowLastValueExecutor::GetData);
	fun.GetSignature().AddParameter("expr", LogicalTypeId::ANY);
	fun.SetCanStreamCallback(WindowLastValueExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowLastValueExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowLastValueExecutor::StreamData);
	fun.SetCanExclude(true);
	return fun;
}

void WindowLastValueExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                      Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gvstate = sink.global_state.Cast<WindowValueGlobalState>();
	auto &lvstate = sink.local_state.Cast<WindowValueLocalState>();
	auto &cursor = *lvstate.cursor;
	const auto count = bounds.size();
	auto &frames = lvstate.frames;
	auto &ignore_nulls = *lvstate.ignore_nulls;
	auto exclude_mode = gvstate.executor.wexpr.WindowExclude();
	WindowAggregator::EvaluateSubFrames(bounds, exclude_mode, count, row_idx, frames, [&](idx_t i) {
		if (gvstate.value_tree) {
			idx_t frame_width = 0;
			for (const auto &frame : frames) {
				frame_width += frame.end - frame.start;
			}

			if (frame_width) {
				auto n = frame_width - 1;
				auto last_idx = gvstate.value_tree->SelectNth(frames, n);
				if (last_idx.second && last_idx.second <= n) {
					//	Frame larger than data. Since we want last, we back off by the overflow
					n -= last_idx.second;
					last_idx = gvstate.value_tree->SelectNth(frames, n);
				}
				if (last_idx.second || last_idx.first >= cursor.Count()) {
					//	No last value - give up.
					FlatVector::SetNull(result, i, true);
				} else {
					cursor.CopyCell(0, last_idx.first, result, i);
				}
			} else {
				FlatVector::SetNull(result, i, true);
			}
			return;
		}

		for (idx_t f = frames.size(); f-- > 0;) {
			const auto &frame = frames[f];
			if (frame.start >= frame.end) {
				continue;
			}

			idx_t n = 1;
			const auto last_idx = WindowBoundariesState::FindPrevStart(ignore_nulls, frame.start, frame.end, n);
			if (!n) {
				cursor.CopyCell(0, last_idx, result, i);
				return;
			}
		}

		// Didn't find one
		FlatVector::SetNull(result, i, true);
	});
}

//===--------------------------------------------------------------------===//
// WindowNthValueExecutor
//===--------------------------------------------------------------------===//
class WindowNthValueStreamingState : public WindowValueStreamingState {
public:
	static bool ComputeNthIndex(ClientContext &client, const BoundWindowExpression &wexpr, idx_t &nth_index) {
		auto &nth_expr = wexpr.GetChildren()[1];
		if (nth_expr && (nth_expr->HasParameter() || !nth_expr->IsFoldable())) {
			return false;
		}
		auto nth_value = ExpressionExecutor::EvaluateScalar(client, *nth_expr);
		if (nth_value.IsNull()) {
			return false;
		}
		auto bigint_value = nth_value.DefaultTryCastAs(LogicalType::BIGINT);
		if (!bigint_value) {
			return false;
		}
		//	Unlike PG we are currently accepting negative indices and mapping them to 0.
		//	Streaming this will have the same behaviour if we use a 0 index.
		const auto bigint = bigint_value->GetValue<int64_t>();
		nth_index = bigint > 0 ? UnsafeNumericCast<idx_t>(bigint) : 0;
		return true;
	}
	WindowNthValueStreamingState(ClientContext &client, DataChunk &input, const BoundWindowExpression &wexpr)
	    : WindowValueStreamingState(client, input, wexpr) {
		vec.SetValue(0, Value(wexpr.GetReturnType()));
		ComputeNthIndex(client, wexpr, nth_index);
	}

	void StreamData(ExecutionContext &context, DataChunk &input, Vector &result);

	//! The target N
	idx_t nth_index = DConstants::INVALID_INDEX;
	//! The current N
	idx_t nth_count = 0;
};

struct WindowNthValueExecutor : public WindowValueExecutor {
	//! Blocking APIs
	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! Streaming APIs
	static bool CanStream(ClientContext &client, const BoundWindowExpression &wexpr, idx_t max_delta) {
		if (!ArgumentIsStreamable(wexpr)) {
			return false;
		}
		// We can only stream Nth Value if N is positive constant.
		idx_t nth_index;
		if (!WindowNthValueStreamingState::ComputeNthIndex(client, wexpr, nth_index)) {
			return false;
		}

		// We can stream Nth values if they are "running totals"
		return wexpr.WindowStart() == WindowBoundary::UNBOUNDED_PRECEDING &&
		       wexpr.WindowEnd() == WindowBoundary::CURRENT_ROW_ROWS;
	}
	static unique_ptr<WindowExecutorStreamingState> GetStreamingState(ClientContext &client, DataChunk &input,
	                                                                  const BoundWindowExpression &wexpr) {
		return make_uniq<WindowNthValueStreamingState>(client, input, wexpr);
	}
	static void StreamData(ExecutionContext &context, DataChunk &input, DataChunk &delayed, idx_t delayed_capacity,
	                       Vector &result, WindowExecutorStreamingState &state) {
		state.Cast<WindowNthValueStreamingState>().StreamData(context, input, result);
	}
};

void WindowNthValueStreamingState::StreamData(ExecutionContext &context, DataChunk &input, Vector &result) {
	//	If we haven't reached the chunk with the value yet, reference the current value
	const auto count = input.size();
	if (!wexpr.IgnoreNulls() && nth_count + count < nth_index) {
		result.Reference(vec);
		nth_count += count;
		return;
	}
	//	If we have found the Nth value already, reference the current value.
	if (nth_index <= nth_count) {
		result.Reference(vec);
		return;
	}

	auto &arg = EvalArg(input);

	UnifiedVectorFormat unified;
	arg.ToUnifiedFormat(unified);
	const auto &validity = unified.validity;

	//	Split the result between NULLs and the Nth Value
	Vector split(wexpr.GetChildren()[0]->GetReturnType(), 2);
	sel_t s = 0;
	split.SetValue(s, vec.GetValue(0));

	// If we are ignoring NULLS and there are NULLS, search for a non-NULL value.
	const bool any_value = wexpr.IgnoreNulls() && !validity.CannotHaveNull();
	for (idx_t i = 0; i < count; ++i) {
		if (!any_value || validity.RowIsValidUnsafe(unified.sel->get_index(i))) {
			++nth_count;
			// One-based comparison.
			if (nth_count == nth_index) {
				auto v = arg.GetValue(i);
				vec.Reference(v, count_t(count));
				s = 1;
				split.SetValue(s, v);
			}
		}
		sel.set_index(i, s);
	}
	result.Slice(split, sel, count);
}

WindowFunction NthValueFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::ANY, ExpressionType::WINDOW_NTH_VALUE, WindowNthValueExecutor::Bind,
	                   WindowNthValueExecutor::GetBounds, WindowNthValueExecutor::GetSharing,
	                   WindowNthValueExecutor::GetGlobal, WindowNthValueExecutor::GetLocal,
	                   WindowValueLocalState::Sinker, WindowValueLocalState::Finalizer,
	                   WindowNthValueExecutor::GetData);
	fun.GetSignature().AddParameter("expr", LogicalTypeId::ANY).AddParameter("n", LogicalType::BIGINT);
	fun.SetCanStreamCallback(WindowNthValueExecutor::CanStream);
	fun.SetStreamingStateCallback(WindowNthValueExecutor::GetStreamingState);
	fun.SetStreamingDataCallback(WindowNthValueExecutor::StreamData);
	fun.SetCanExclude(true);
	return fun;
}

void WindowNthValueExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds,
                                     Vector &result, idx_t row_idx, OperatorSinkInput &sink) {
	auto &gvstate = sink.global_state.Cast<WindowValueGlobalState>();
	auto &lvstate = sink.local_state.Cast<WindowValueLocalState>();
	auto &cursor = *lvstate.cursor;
	const auto count = bounds.size();
	auto &frames = lvstate.frames;
	auto &ignore_nulls = *lvstate.ignore_nulls;
	auto exclude_mode = gvstate.executor.wexpr.WindowExclude();
	D_ASSERT(cursor.chunk.ColumnCount() == 1);
	const auto &child_idx = gvstate.executor.child_idx;
	const auto nth_idx = child_idx[1];
	WindowInputExpression nth_col(eval_chunk, nth_idx);
	WindowAggregator::EvaluateSubFrames(bounds, exclude_mode, count, row_idx, frames, [&](idx_t i) {
		// Returns value evaluated at the row that is the n'th row of the window frame (counting from 1);
		// returns NULL if there is no such row.
		if (nth_col.CellIsNull(i)) {
			FlatVector::SetNull(result, i, true);
			return;
		}
		auto n_param = nth_col.GetCell<int64_t>(i);
		if (n_param < 1) {
			FlatVector::SetNull(result, i, true);
			return;
		}

		//	Decrement as we go along.
		auto n = idx_t(n_param);

		if (gvstate.value_tree) {
			idx_t frame_width = 0;
			for (const auto &frame : frames) {
				frame_width += frame.end - frame.start;
			}

			if (--n < frame_width) {
				const auto nth_index = gvstate.value_tree->SelectNth(frames, n);
				if (nth_index.second || nth_index.first >= cursor.Count()) {
					// Past end of frame
					FlatVector::SetNull(result, i, true);
				} else {
					cursor.CopyCell(0, nth_index.first, result, i);
				}
			} else {
				FlatVector::SetNull(result, i, true);
			}
			return;
		}

		for (const auto &frame : frames) {
			if (frame.start >= frame.end) {
				continue;
			}

			const auto nth_index = WindowBoundariesState::FindNextStart(ignore_nulls, frame.start, frame.end, n);
			if (!n) {
				cursor.CopyCell(0, nth_index, result, i);
				return;
			}
		}
		FlatVector::SetNull(result, i, true);
	});
}

//===--------------------------------------------------------------------===//
// WindowFillExecutor
//===--------------------------------------------------------------------===//
struct WindowFillExecutor : public WindowValueExecutor {
	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input);
	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared);

	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask);
	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);

	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);
};

template <class TO, class FROM>
static TO LossyFillCast(FROM val) {
	return LossyNumericCast<TO, FROM>(val);
}

template <>
double LossyFillCast(hugeint_t val) {
	double d;
	(void)Hugeint::TryCast(val, d);
	return d;
}

template <>
double LossyFillCast(uhugeint_t val) {
	double d;
	(void)Hugeint::TryCast(val, d);
	return d;
}

template <typename T>
static double FillSlopeFunc(WindowCursor &cursor, idx_t row_idx, idx_t prev_valid, idx_t next_valid) {
	//	Cast everything to doubles immediately so we can interpolate backwards (x < x0)
	const auto x = LossyFillCast<double>(cursor.GetCell<T>(0, row_idx));
	const auto x0 = LossyFillCast<double>(cursor.GetCell<T>(0, prev_valid));
	const auto x1 = LossyFillCast<double>(cursor.GetCell<T>(0, next_valid));

	auto den = (x1 - x0);
	if (den == 0) {
		// Duplicate X values, so pick the first.
		return 0;
	}
	auto num = x - x0;
	if (!std::isfinite(den)) {
		const auto scale = MaxValue(std::abs(x0), std::abs(x1));
		num = x / scale - x0 / scale;
		den = x1 / scale - x0 / scale;
	}
	return num / den;
}

typedef double (*fill_slope_t)(WindowCursor &cursor, idx_t row_idx, idx_t prev_valid, idx_t next_valid);

static fill_slope_t GetFillSlopeFunction(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		return FillSlopeFunc<uint8_t>;
	case PhysicalType::UINT16:
		return FillSlopeFunc<uint16_t>;
	case PhysicalType::UINT32:
		return FillSlopeFunc<uint32_t>;
	case PhysicalType::UINT64:
		return FillSlopeFunc<uint64_t>;
	case PhysicalType::UINT128:
		return FillSlopeFunc<uhugeint_t>;
	case PhysicalType::INT8:
		return FillSlopeFunc<int8_t>;
	case PhysicalType::INT16:
		return FillSlopeFunc<int16_t>;
	case PhysicalType::INT32:
		return FillSlopeFunc<int32_t>;
	case PhysicalType::INT64:
		return FillSlopeFunc<int64_t>;
	case PhysicalType::INT128:
		return FillSlopeFunc<hugeint_t>;
	case PhysicalType::FLOAT:
		return FillSlopeFunc<float>;
	case PhysicalType::DOUBLE:
		return FillSlopeFunc<double>;
	default:
		throw InternalException("Unsupported FILL slope type.");
	}
}

struct TryExtrapolateOperator {
	template <typename T>
	static bool Operation(const T &lo, const double d, const T &hi, T &result) {
		if (lo > hi) {
			//	Swap the endpoints to keep the offset positive.
			//	The slope in the exchanged frame is (1 - d), not (-d):
			//	lo + d*(hi - lo) == hi + (1 - d)*(lo - hi)
			return Operation<T>(hi, 1 - d, lo, result);
		}
		const auto delta = LossyNumericCast<double>(hi) - LossyNumericCast<double>(lo);
		T offset;
		if (d < 0) {
			if (!TryCast::Operation(delta * (-d), offset)) {
				return false;
			}
			return TrySubtractOperator::Operation(lo, offset, result);
			;
		}

		if (!TryCast::Operation(delta * d, offset)) {
			return false;
		}
		return TryAddOperator::Operation(lo, offset, result);
	}
};

template <>
bool TryExtrapolateOperator::Operation(const double &lo, const double d, const double &hi, double &result) {
	result = InterpolateOperator::Operation<double>(lo, d, hi);
	return true;
}

template <>
bool TryExtrapolateOperator::Operation(const float &lo, const double d, const float &hi, float &result) {
	result = InterpolateOperator::Operation<float>(lo, d, hi);
	return true;
}

template <>
bool TryExtrapolateOperator::Operation(const hugeint_t &lo, const double d, const hugeint_t &hi, hugeint_t &result) {
	double temp;
	return Operation(Hugeint::Cast<double>(lo), d, Hugeint::Cast<double>(hi), temp) &&
	       Hugeint::TryConvert(temp, result);
}

template <>
bool TryExtrapolateOperator::Operation(const uhugeint_t &lo, const double d, const uhugeint_t &hi, uhugeint_t &result) {
	double temp;
	return Operation(Uhugeint::Cast<double>(lo), d, Uhugeint::Cast<double>(hi), temp) &&
	       Uhugeint::TryConvert(temp, result);
}

typedef void (*fill_interpolate_t)(Vector &result, idx_t i, WindowCursor &cursor, idx_t lo, idx_t hi, double slope);

template <typename T>
static void FillInterpolateFunc(Vector &result, idx_t i, WindowCursor &cursor, idx_t lo, idx_t hi, double slope) {
	const auto y0 = cursor.GetCell<T>(0, lo);
	const auto y1 = cursor.GetCell<T>(0, hi);
	auto data = FlatVector::GetDataMutable<T>(result);
	if (slope < 0 || slope > 1) {
		if (TryExtrapolateOperator::Operation(y0, slope, y1, data[i])) {
			FlatVector::SetNull(result, i, false);
		}
		return;
	}

	FlatVector::SetNull(result, i, false);
	if constexpr (std::is_integral<T>::value) {
		//	The interpolated value lies between the two values, but the double result can round past them
		const auto lo = MinValue(y0, y1);
		const auto hi = MaxValue(y0, y1);
		const auto interpolated = static_cast<double>(y0) + (static_cast<double>(y1) - static_cast<double>(y0)) * slope;
		if (interpolated <= static_cast<double>(lo)) {
			data[i] = lo;
		} else if (interpolated >= static_cast<double>(hi)) {
			data[i] = hi;
		} else {
			data[i] = static_cast<T>(interpolated);
		}
		return;
	}
	data[i] = InterpolateOperator::Operation<T>(y0, slope, y1);
}

static fill_interpolate_t GetFillInterpolateFunction(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		return FillInterpolateFunc<uint8_t>;
	case PhysicalType::UINT16:
		return FillInterpolateFunc<uint16_t>;
	case PhysicalType::UINT32:
		return FillInterpolateFunc<uint32_t>;
	case PhysicalType::UINT64:
		return FillInterpolateFunc<uint64_t>;
	case PhysicalType::UINT128:
		return FillInterpolateFunc<uhugeint_t>;
	case PhysicalType::INT8:
		return FillInterpolateFunc<int8_t>;
	case PhysicalType::INT16:
		return FillInterpolateFunc<int16_t>;
	case PhysicalType::INT32:
		return FillInterpolateFunc<int32_t>;
	case PhysicalType::INT64:
		return FillInterpolateFunc<int64_t>;
	case PhysicalType::INT128:
		return FillInterpolateFunc<hugeint_t>;
	case PhysicalType::FLOAT:
		return FillInterpolateFunc<float>;
	case PhysicalType::DOUBLE:
		return FillInterpolateFunc<double>;
	default:
		throw InternalException("Unsupported FILL interpolation type.");
	}
}

//	Interpolation and extrapolation only check the physical type for overflow,
//	so the results still need to be checked against the range of the logical type
typedef bool (*fill_validate_t)(Vector &result, idx_t i, const LogicalType &type);

template <int64_t MAX_VALUE>
static bool FillValidateTime(Vector &result, idx_t i, const LogicalType &type) {
	const auto value = FlatVector::GetData<int64_t>(result)[i];
	return value >= 0 && value <= MAX_VALUE;
}

template <typename T>
static bool FillValidateDecimal(Vector &result, idx_t i, const LogicalType &type) {
	const auto value = FlatVector::GetData<T>(result)[i];
	T limit;
	if (!TryCast::Operation(Hugeint::POWERS_OF_TEN[DecimalType::GetWidth(type)], limit)) {
		return true;
	}
	return value < limit && value > -limit;
}

template <typename T>
static bool FillValidateFinite(Vector &result, idx_t i, const LogicalType &type) {
	return Value::IsFinite(FlatVector::GetData<T>(result)[i]);
}

template <int64_t FACTOR>
static bool FillValidateScaledTimestamp(Vector &result, idx_t i, const LogicalType &type) {
	//	the value must be representable in microseconds
	int64_t micros;
	if (!TryMultiplyOperator::Operation(FlatVector::GetData<int64_t>(result)[i], FACTOR, micros)) {
		return false;
	}
	return Value::IsFinite(timestamp_t(micros));
}

static fill_validate_t GetFillValidateFunction(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIME:
		return FillValidateTime<Interval::MICROS_PER_DAY>;
	case LogicalTypeId::TIME_NS:
		return FillValidateTime<Interval::MICROS_PER_DAY * Interval::NANOS_PER_MICRO>;
	case LogicalTypeId::DATE:
		return FillValidateFinite<date_t>;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return FillValidateFinite<timestamp_t>;
	case LogicalTypeId::TIMESTAMP_SEC:
		return FillValidateScaledTimestamp<Interval::MICROS_PER_SEC>;
	case LogicalTypeId::TIMESTAMP_MS:
		return FillValidateScaledTimestamp<Interval::MICROS_PER_MSEC>;
	case LogicalTypeId::DECIMAL:
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return FillValidateDecimal<int16_t>;
		case PhysicalType::INT32:
			return FillValidateDecimal<int32_t>;
		case PhysicalType::INT64:
			return FillValidateDecimal<int64_t>;
		case PhysicalType::INT128:
			return FillValidateDecimal<hugeint_t>;
		default:
			throw InternalException("Unsupported FILL decimal type.");
		}
	default:
		return nullptr;
	}
}

typedef bool (*fill_value_t)(idx_t i, WindowCursor &cursor);

template <typename T>
static bool FillValueFunction(idx_t row_idx, WindowCursor &cursor) {
	return !cursor.CellIsNull(0, row_idx) && Value::IsFinite(cursor.GetCell<T>(0, row_idx));
}

static fill_value_t GetFillValueFunction(const LogicalType &type) {
	//	Special cases temporal values because they can have infinities
	switch (type.id()) {
	case LogicalTypeId::DATE:
		return FillValueFunction<date_t>;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return FillValueFunction<timestamp_t>;
	default:
		break;
	}

	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		return FillValueFunction<uint8_t>;
	case PhysicalType::UINT16:
		return FillValueFunction<uint16_t>;
	case PhysicalType::UINT32:
		return FillValueFunction<uint32_t>;
	case PhysicalType::UINT64:
		return FillValueFunction<uint64_t>;
	case PhysicalType::UINT128:
		return FillValueFunction<uhugeint_t>;
	case PhysicalType::INT8:
		return FillValueFunction<int8_t>;
	case PhysicalType::INT16:
		return FillValueFunction<int16_t>;
	case PhysicalType::INT32:
		return FillValueFunction<int32_t>;
	case PhysicalType::INT64:
		return FillValueFunction<int64_t>;
	case PhysicalType::INT128:
		return FillValueFunction<hugeint_t>;
	case PhysicalType::FLOAT:
		return FillValueFunction<float>;
	case PhysicalType::DOUBLE:
		return FillValueFunction<double>;
	default:
		throw InternalException("Unsupported FILL value type.");
	}
}

static bool IsFillType(const LogicalType &type) {
	return type.IsNumeric() || (type.IsTemporal() && type.id() != LogicalTypeId::TIME_TZ);
}

unique_ptr<FunctionData> WindowFillExecutor::Bind(BindWindowFunctionInput &input) {
	WindowValueExecutor::Bind(input);

	const auto &arguments = input.GetArguments();

	//	Check FILL arguments support subtraction
	if (!IsFillType(arguments[0]->GetReturnType())) {
		throw BinderException("FILL argument must support subtraction");
	}

	// Can we validate?
	if (!input.HasOrders() || !input.HasArgumentOrders()) {
		return nullptr;
	}

	auto &order_types = input.GetOrderTypes();
	auto &arg_order_types = input.GetArgumentOrderTypes();

	if (arg_order_types.size() > 1 || (arg_order_types.empty() && order_types.size() != 1)) {
		throw BinderException("FILL functions must have only one ORDER BY expression");
	}

	LogicalType order_type;
	if (arg_order_types.empty()) {
		D_ASSERT(!order_types.empty());
		order_type = order_types[0];
	} else {
		order_type = arg_order_types[0];
	}
	if (!IsFillType(order_type)) {
		throw BinderException("FILL ordering must support subtraction");
	}

	return nullptr;
}

void WindowFillExecutor::GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared) {
	const auto &wexpr = executor.wexpr;
	D_ASSERT(!wexpr.GetChildren().empty());

	//! Never ignore nulls (that's the point!)
	auto &child_idx = executor.child_idx;
	child_idx.emplace_back(shared.RegisterCollection(wexpr.GetChildren()[0], false));

	//	If the argument order is prefix of the partition ordering,
	//	then we can just use the partition ordering.
	auto &arg_orders = wexpr.ArgOrders();
	auto &arg_order_idx = executor.arg_order_idx;
	if (BoundWindowExpression::GetSharedOrders(wexpr.OrderBy(), arg_orders) != arg_orders.size()) {
		for (const auto &order : wexpr.ArgOrders()) {
			arg_order_idx.emplace_back(shared.RegisterSink(order.expression));
		}
	}

	//	We need the sort values for interpolation, so either use the range or the secondary ordering expression
	if (arg_order_idx.empty()) {
		//	We use the range ordering, even if it has not been defined
		if (executor.range_idx == DConstants::INVALID_INDEX) {
			// the first ordering - also when it is shared with an argument ordering that is a prefix of the orderings
			D_ASSERT(!wexpr.OrderBy().empty());
			//	We don't need the validity mask because we have also requested the valid range for the ordering.
			executor.range_idx = shared.RegisterCollection(wexpr.OrderBy()[0].expression, false);
		}
		executor.aux_idx.emplace_back(executor.range_idx);
	} else {
		//	For secondary sorts, we need the entire collection so we can interpolate using the values
		D_ASSERT(arg_order_idx.size() == 1);
		executor.aux_idx.emplace_back(shared.RegisterCollection(wexpr.ArgOrders()[0].expression, false));
	}
}

static void WindowFillCopy(WindowCursor &cursor, Vector &result, idx_t count, idx_t row_idx, column_t col_idx = 0) {
	for (idx_t target_offset = 0; target_offset < count;) {
		const auto source_offset = cursor.Seek(row_idx);
		const auto &source = cursor.chunk.data[col_idx];
		const auto copied = MinValue<idx_t>(cursor.chunk.size() - source_offset, count - target_offset);
		VectorOperations::Copy(source, result, source_offset + copied, source_offset, target_offset);
		target_offset += copied;
		row_idx += copied;
	}
}

class WindowFillGlobalState : public WindowLeadLagGlobalState {
public:
	WindowFillGlobalState(ClientContext &client, const WindowExecutor &executor, const idx_t payload_count,
	                      const ValidityMask &partition_mask, const ValidityMask &order_mask)
	    : WindowLeadLagGlobalState(client, executor, payload_count, partition_mask, order_mask),
	      order_idx(executor.aux_idx.empty() ? DConstants::INVALID_INDEX : executor.aux_idx[0]) {
	}

	//! Collection index of the secondary sort values
	const idx_t order_idx;
};

class WindowFillLocalState : public WindowLeadLagLocalState {
public:
	WindowFillLocalState(ExecutionContext &context, const WindowLeadLagGlobalState &gvstate)
	    : WindowLeadLagLocalState(context, gvstate) {
	}

	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr) {
		required.insert(FRAME_BEGIN);
		required.insert(FRAME_END);

		auto &arg_orders = wexpr.ArgOrders();
		const auto shared = BoundWindowExpression::GetSharedOrders(wexpr.OrderBy(), arg_orders) == arg_orders.size();
		if (wexpr.ArgOrders().empty() || shared) {
			//	FILL uses the validity ranges to quickly eliminate indexes that can't be interpolated.
			//	This only works for non-secondary orderings
			required.insert(VALID_BEGIN);
			required.insert(VALID_END);
		}
	}

	//! Finish the sinking and prepare to scan
	static void Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink);

	//! Cursor for the secondary sort values
	unique_ptr<WindowCursor> order_cursor;
};

void WindowFillLocalState::Finalizer(ExecutionContext &context, CollectionPtr collection, OperatorSinkInput &sink) {
	WindowLeadLagLocalState::Finalizer(context, collection, sink);

	// Prepare to scan
	auto &gfstate = sink.global_state.Cast<WindowFillGlobalState>();
	auto &lfstate = sink.local_state.Cast<WindowFillLocalState>();
	if (!lfstate.order_cursor && gfstate.order_idx != DConstants::INVALID_INDEX) {
		lfstate.order_cursor = make_uniq<WindowCursor>(*collection, gfstate.order_idx);
	}
}

WindowFunction FillFun::GetFunction() {
	WindowFunction fun(Name, {}, LogicalType::ANY, ExpressionType::WINDOW_FILL, WindowFillExecutor::Bind,
	                   WindowFillLocalState::GetBounds, WindowFillExecutor::GetSharing, WindowFillExecutor::GetGlobal,
	                   WindowFillExecutor::GetLocal, WindowFillLocalState::Sinker, WindowFillLocalState::Finalizer,
	                   WindowFillExecutor::GetData);
	fun.GetSignature().AddParameter("expr", LogicalTypeId::ANY);

	//! Never ignore nulls (that's the point!)
	fun.SetCanIgnoreNulls(false);

	return fun;
}

unique_ptr<GlobalSinkState> WindowFillExecutor::GetGlobal(ClientContext &client, const WindowExecutor &executor,
                                                          const idx_t payload_count, const ValidityMask &partition_mask,
                                                          const ValidityMask &order_mask) {
	return make_uniq<WindowFillGlobalState>(client, executor, payload_count, partition_mask, order_mask);
}

unique_ptr<LocalSinkState> WindowFillExecutor::GetLocal(ExecutionContext &context, const GlobalSinkState &gstate) {
	const auto &gfstate = gstate.Cast<WindowFillGlobalState>();
	return make_uniq<WindowFillLocalState>(context, gfstate);
}

void WindowFillExecutor::GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
                                 idx_t row_idx, OperatorSinkInput &sink) {
	auto &lfstate = sink.local_state.Cast<WindowFillLocalState>();
	const auto count = bounds.size();
	auto &cursor = *lfstate.cursor;
	auto &order_cursor = *lfstate.order_cursor;

	//	Assume the best and just batch copy all the values
	WindowFillCopy(cursor, result, count, row_idx, 0);

	//	If all are valid, we are done
	auto validity = result.Validity();
	if (!validity.CanHaveNull()) {
		return;
	}

	//	Missing values - linear interpolation
	auto &gfstate = sink.global_state.Cast<WindowFillGlobalState>();
	auto partition_begin = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_BEGIN]);
	auto partition_end = FlatVector::GetData<const idx_t>(bounds.data[PARTITION_END]);

	idx_t prev_valid = DConstants::INVALID_INDEX;
	idx_t next_valid = DConstants::INVALID_INDEX;

	const auto &wexpr = gfstate.executor.wexpr;
	const auto &fill_type = wexpr.GetChildren()[0]->GetReturnType();
	auto interpolate_func = GetFillInterpolateFunction(fill_type);
	auto validate_func = GetFillValidateFunction(fill_type);
	auto interpolate = [&](idx_t i, idx_t lo, idx_t hi, double slope) {
		interpolate_func(result, i, cursor, lo, hi, slope);
		if (validate_func && FlatVector::Validity(result).RowIsValid(i) && !validate_func(result, i, fill_type)) {
			FlatVector::SetNull(result, i, true);
		}
	};
	auto value_func = GetFillValueFunction(wexpr.GetChildren()[0]->GetReturnType());

	//	Secondary sort - use the MSTs
	if (gfstate.value_tree) {
		//	Roughly what we need to do is find the previous and next non-null values
		//	with non-null ordering values. This is essentially LEAD/LAG(IGNORE NULLS)
		auto slope_func = GetFillSlopeFunction(wexpr.ArgOrders()[0].expression->GetReturnType());
		auto order_value_func = GetFillValueFunction(wexpr.ArgOrders()[0].expression->GetReturnType());
		auto &frames = lfstate.frames;
		frames.resize(1);
		auto &frame = frames[0];
		for (idx_t i = 0; i < count; ++i, ++row_idx) {
			//	If this value is valid, move on
			if (validity.IsValid(i)) {
				continue;
			}

			//	Frame is the entire partition
			frame = {partition_begin[i], partition_end[i]};
			const auto frame_width = frame.end - frame.start;
			D_ASSERT(frame.end != frame.start);

			//	If we are outside the validity range of the sort column, we can't fix this value.
			if (!order_value_func(row_idx, order_cursor)) {
				continue;
			}

			//	Find the own row number in the secondary sort
			const auto own_row = gfstate.row_tree->Rank(frame.start, frame.end, row_idx) - 1;

			//	Find the previous valid value, scanning backwards from the current row
			//	Note that we can't reuse previous values as the scan order is not the sort order
			prev_valid = DConstants::INVALID_INDEX;
			idx_t prev_n = DConstants::INVALID_INDEX;
			for (idx_t n = own_row; n-- > 0;) {
				auto j = gfstate.value_tree->SelectNth(frames, n).first;
				if (!order_value_func(j, order_cursor)) {
					break;
				}
				if (value_func(j, cursor)) {
					prev_valid = j;
					prev_n = n;
					break;
				}
			}

			//	If there is nothing being us (missing early value) then scan forward
			if (prev_valid == DConstants::INVALID_INDEX) {
				for (idx_t n = own_row + 1; n < frame_width; ++n) {
					auto j = gfstate.value_tree->SelectNth(frames, n).first;
					if (!order_value_func(j, order_cursor)) {
						break;
					}
					if (value_func(j, cursor)) {
						prev_valid = j;
						prev_n = n;
						break;
					}
				}
			}

			//	No valid values!
			if (prev_valid == DConstants::INVALID_INDEX) {
				//	Skip to the next partition
				i += partition_end[i] - row_idx - 1;
				if (i >= count) {
					return;
				}
				row_idx = partition_end[i] - 1;
				continue;
			}

			//	Find the next valid value after the previous
			next_valid = DConstants::INVALID_INDEX;
			for (idx_t n = prev_n + 1; n < frame_width; ++n) {
				auto j = gfstate.value_tree->SelectNth(frames, n).first;
				if (!order_value_func(j, order_cursor)) {
					break;
				}
				if (value_func(j, cursor)) {
					next_valid = j;
					break;
				}
			}

			//	Nothing after, so scan backwards from the previous
			if (next_valid == DConstants::INVALID_INDEX && prev_n > 0) {
				for (idx_t n = prev_n; n-- > 0;) {
					auto j = gfstate.value_tree->SelectNth(frames, n).first;
					if (!order_value_func(j, order_cursor)) {
						break;
					}
					if (value_func(j, cursor)) {
						next_valid = j;
						//	Restore ordering
						std::swap(prev_valid, next_valid);
						break;
					}
				}
			}

			//	If we only have one value, then just copy it
			if (next_valid == DConstants::INVALID_INDEX) {
				cursor.CopyCell(0, prev_valid, result, i);
				continue;
			}

			//	Two values, so interpolate
			//	y = y0 + (y1 - y0) / (x1 - x0) * (x - x0)
			const auto slope = slope_func(order_cursor, row_idx, prev_valid, next_valid);
			interpolate(i, prev_valid, next_valid, slope);
		}
		return;
	}

	auto valid_begin = FlatVector::GetData<const idx_t>(bounds.data[VALID_BEGIN]);
	auto valid_end = FlatVector::GetData<const idx_t>(bounds.data[VALID_END]);
	idx_t prev_partition = DConstants::INVALID_INDEX;
	auto slope_func = GetFillSlopeFunction(wexpr.OrderBy()[0].expression->GetReturnType());
	auto order_value_func = GetFillValueFunction(wexpr.OrderBy()[0].expression->GetReturnType());

	for (idx_t i = 0; i < count; ++i, ++row_idx) {
		//	Did we change partitions?
		if (prev_partition != partition_begin[i]) {
			prev_partition = partition_begin[i];
			prev_valid = DConstants::INVALID_INDEX;
			next_valid = DConstants::INVALID_INDEX;
		}

		//	If we are outside the validity range of the sort column, we can't fix this value.
		if (row_idx < valid_begin[i] || valid_end[i] <= row_idx || !order_value_func(row_idx, order_cursor)) {
			continue;
		}

		//	If this value is valid,
		if (validity.IsValid(i)) {
			//	If it is usable, track it for the next gap.
			if (value_func(row_idx, cursor)) {
				prev_valid = row_idx;
			}
			continue;
		}

		//	Missing value, so look for interpolation values

		//	Find the previous valid value, scanning backwards from the current row
		if (prev_valid == DConstants::INVALID_INDEX) {
			for (idx_t j = row_idx; j-- > valid_begin[i];) {
				if (!order_value_func(j, order_cursor)) {
					break;
				}
				if (value_func(j, cursor)) {
					prev_valid = j;
					break;
				}
			}
		}

		//	If there is nothing behind us (missing early value) then scan forward
		if (prev_valid == DConstants::INVALID_INDEX) {
			for (idx_t j = row_idx + 1; j < valid_end[i]; ++j) {
				if (!order_value_func(j, order_cursor)) {
					break;
				}
				if (value_func(j, cursor)) {
					prev_valid = j;
					break;
				}
			}
		}

		//	No valid values!
		if (prev_valid == DConstants::INVALID_INDEX) {
			//	Skip to the next partition
			i += partition_end[i] - row_idx - 1;
			if (i >= count) {
				break;
			}
			row_idx = partition_end[i] - 1;
			continue;
		}

		//	Find the next valid value after the previous
		next_valid = DConstants::INVALID_INDEX;
		for (idx_t j = prev_valid + 1; j < valid_end[i]; ++j) {
			if (!order_value_func(j, order_cursor)) {
				break;
			}
			if (value_func(j, cursor)) {
				next_valid = j;
				break;
			}
		}

		//	Nothing after, so scan backwards
		if (next_valid == DConstants::INVALID_INDEX) {
			for (idx_t j = prev_valid; j-- > valid_begin[i];) {
				if (!order_value_func(j, order_cursor)) {
					break;
				}
				if (value_func(j, cursor)) {
					next_valid = j;
					//	Restore ordering
					std::swap(prev_valid, next_valid);
					break;
				}
			}
		}

		//	If we only have one value, then just copy it
		if (next_valid == DConstants::INVALID_INDEX) {
			cursor.CopyCell(0, prev_valid, result, i);
			continue;
		}

		//	Two values, so interpolate
		//	y = y0 + (y1 - y0) / (x1 - x0) * (x - x0)
		const auto slope = slope_func(order_cursor, row_idx, prev_valid, next_valid);
		interpolate(i, prev_valid, next_valid, slope);
	}
}

} // namespace duckdb
