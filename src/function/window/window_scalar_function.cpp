#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/function/aggregate_state.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

namespace {

struct ScalarWindowBindData : public FunctionData {
	using BindInfoPtr = unique_ptr<FunctionData>;

	ScalarWindowBindData(ClientContext &client, BoundWindowExpression &wexpr) : client(client), wexpr(wexpr.Copy()) {
	}

	ScalarWindowBindData(const ScalarWindowBindData &other)
	    : FunctionData(other), client(other.client), wexpr(other.wexpr->Copy()) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ScalarWindowBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScalarWindowBindData>();
		if (!wexpr->Equals(*other.wexpr)) {
			return false;
		}
		return true;
	}

	ClientContext &client;
	unique_ptr<Expression> wexpr;
};

void WindowScalarFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &scalar_info = func_expr.BindInfo()->Cast<ScalarWindowBindData>();
	auto &wexpr = scalar_info.wexpr->Cast<BoundWindowExpression>();

	auto &client = scalar_info.client;
	ThreadContext thread(client);
	ExecutionContext context(client, thread, nullptr);
	auto &allocator = Allocator::Get(client);
	ArenaAllocator arena_allocator(allocator);
	const auto count = args.size();

	//	Build shared expressions
	WindowSharedExpressions shared;
	auto wexec = WindowExecutor::Factory(wexpr, client, shared);

	DataChunk coll_chunk;
	ExpressionExecutor coll_exec(client);
	shared.PrepareCollection(coll_exec, coll_chunk);

	//	Build acceleration data
	//	`count` partitions in which every row is its own peer group, so the frame is the current row
	ValidityMask partition_mask;
	partition_mask.Initialize(count);
	ValidityMask order_mask;
	order_mask.Initialize(count);
	auto gsink = wexec->GetGlobalState(client, count, partition_mask, order_mask);
	auto lsink = wexec->GetLocalState(context, *gsink);

	//	Compute fully materialised expressions
	auto &buffer_manager = BufferManager::GetBufferManager(client);
	auto collection = make_uniq<WindowCollection>(buffer_manager, count, coll_chunk.GetTypes());
	if (coll_chunk.data.empty()) {
		coll_chunk.SetChildCardinality(count);
	} else {
		coll_exec.Execute(args, coll_chunk);
		auto builder = make_uniq<WindowBuilder>(*collection);
		builder->Sink(coll_chunk, 0);
	}

	// Compute sink expressions
	DataChunk sink_chunk;
	ExpressionExecutor sink_exec(client);
	shared.PrepareSink(sink_exec, sink_chunk);
	if (sink_chunk.data.empty()) {
		sink_chunk.SetChildCardinality(count);
	} else {
		sink_exec.Execute(args, sink_chunk);
	}

	InterruptState interrupt;
	OperatorSinkInput sink {*gsink, *lsink, interrupt};
	wexec->Sink(context, sink_chunk, coll_chunk, 0, sink);

	collection->Combine(shared.coll_validity);
	wexec->Finalize(context, collection, sink);

	//	Evaluate
	DataChunk eval_chunk;
	ExpressionExecutor eval_exec(client);
	shared.PrepareEvaluate(eval_exec, eval_chunk);
	if (eval_chunk.data.empty()) {
		eval_chunk.SetChildCardinality(count);
	} else {
		eval_exec.Execute(args, eval_chunk);
	}

	wexec->Evaluate(context, 0, eval_chunk, result, sink, count);
}

} // namespace

unique_ptr<Expression> FunctionBinder::BindScalarWindowFunction(BoundWindowExpression &wexpr,
                                                                const vector<LogicalType> &input_types) {
	//	The window expressions reference the input row, so the function receives all of it
	vector<unique_ptr<Expression>> children;
	for (idx_t col_idx = 0; col_idx < input_types.size(); ++col_idx) {
		children.emplace_back(make_uniq<BoundReferenceExpression>(input_types[col_idx], col_idx));
	}
	auto &aggr = wexpr.AggregateFunction();
	ScalarFunction scalar(wexpr.GetName(), input_types, wexpr.GetReturnType(), WindowScalarFunc);
	if (aggr) {
		scalar.SetProperties(aggr->GetProperties());
	} else {
		scalar.SetProperties(wexpr.WindowFunction()->GetProperties());
	}
	// the wrapper also executes the window expression's children (e.g. casts) and validates frame boundary
	// expressions at runtime, not just the function itself
	if (wexpr.CanThrow() || wexpr.StartExpr() || wexpr.EndExpr()) {
		scalar.SetFallible();
	}
	//	NULLs in the input row do not imply a NULL result
	scalar.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	auto bind_info = make_uniq<ScalarWindowBindData>(context, wexpr);
	BoundScalarFunction bound(scalar);
	return make_uniq<BoundFunctionExpression>(bound, std::move(children), std::move(bind_info));
}

} // namespace duckdb
