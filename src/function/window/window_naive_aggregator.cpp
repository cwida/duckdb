#include "duckdb/function/window/window_naive_aggregator.hpp"
#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/function/window/window_collection.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/function/window/window_aggregate_function.hpp"
#include "duckdb/common/enums/window_aggregation_mode.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowNaiveAggregator
//===--------------------------------------------------------------------===//
WindowNaiveAggregator::WindowNaiveAggregator(const WindowAggregateExecutor &executor, WindowSharedExpressions &shared)
    : WindowAggregator(executor.wexpr, shared), executor(executor) {
	for (const auto &order : wexpr.ArgOrders()) {
		arg_order_idx.emplace_back(shared.RegisterCollection(order.expression, false));
	}
}

WindowNaiveAggregator::~WindowNaiveAggregator() {
}

class WindowNaiveLocalState : public WindowAggregatorLocalState {
public:
	struct HashRow {
		explicit HashRow(WindowNaiveLocalState &state) : state(state) {
		}

		inline size_t operator()(const idx_t &i) const {
			return state.Hash(i);
		}

		WindowNaiveLocalState &state;
	};

	struct EqualRow {
		explicit EqualRow(WindowNaiveLocalState &state) : state(state) {
		}

		inline bool operator()(const idx_t &lhs, const idx_t &rhs) const {
			return state.KeyEqual(lhs, rhs);
		}

		WindowNaiveLocalState &state;
	};

	using RowSet = std::unordered_set<idx_t, HashRow, EqualRow>;

	WindowNaiveLocalState(ExecutionContext &context, const WindowNaiveAggregator &aggregator);
	~WindowNaiveLocalState() override;

	void Finalize(ExecutionContext &context, WindowAggregatorGlobalState &gastate, CollectionPtr collection) override;

	void Evaluate(ExecutionContext &context, const WindowAggregatorGlobalState &gsink, const DataChunk &bounds,
	              Vector &result, idx_t count, idx_t row_idx, InterruptState &interrupt);

protected:
	//! Flush the accumulated intermediate states into the result states
	void FlushStates(const WindowAggregatorGlobalState &gsink);

	//! Hashes a value for the hash table
	size_t Hash(idx_t rid);
	//! Compares two values for the hash table
	bool KeyEqual(const idx_t &lhs, const idx_t &rhs);

	//! The global state
	const WindowNaiveAggregator &aggregator;
	//! Data pointer that contains a vector of states, used for row aggregation
	vector<data_t> state;
	//! Reused result state container for the aggregate
	Vector statef;
	//! A vector of pointers to "state", used for buffering intermediate aggregates
	Vector statep;
	//! Input data chunk, used for leaf segment aggregation
	DataChunk leaves;
	//! The rows beginning updated.
	SelectionVector update_sel;
	//! Count of buffered values
	idx_t flush_count;
	//! The number of initialised result states
	idx_t live_count;
	//! The frame boundaries, used for EXCLUDE
	SubFrames frames;
	//! The optional hash table used for DISTINCT
	Vector hashes;
	//! The state used for comparing the collection across chunk boundaries
	unique_ptr<WindowCursor> comparer;

	//! The state used for scanning ORDER BY values from the collection
	unique_ptr<Sort> sort;
	//! The order by collection
	unique_ptr<WindowCursor> arg_orderer;
	//! Reusable sort key chunk
	DataChunk orderby_sink;
	//! Reusable sort payload chunk
	DataChunk orderby_scan;
	//! Reusable sort key slicer
	SelectionVector orderby_sel;
};

WindowNaiveLocalState::WindowNaiveLocalState(ExecutionContext &context, const WindowNaiveAggregator &aggregator)
    : WindowAggregatorLocalState(context), aggregator(aggregator), state(aggregator.state_size * STANDARD_VECTOR_SIZE),
      statef(LogicalType::POINTER), statep((LogicalType::POINTER)), flush_count(0), live_count(0),
      hashes(LogicalType::HASH) {
	InitSubFrames(frames, aggregator.exclude_mode);

	update_sel.Initialize();

	//	Build the finalise vector that just points to the result states
	data_ptr_t state_ptr = state.data();
	D_ASSERT(statef.GetVectorType() == VectorType::FLAT_VECTOR);
	statef.SetVectorType(VectorType::CONSTANT_VECTOR);
	statef.Flatten();
	auto fdata = FlatVector::GetDataMutable<data_ptr_t>(statef);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; ++i) {
		fdata[i] = state_ptr;
		state_ptr += aggregator.state_size;
	}
}

WindowNaiveLocalState::~WindowNaiveLocalState() {
	// destroy any states left live by an exception during evaluation
	const auto &aggr = aggregator.aggr;
	if (live_count && aggr.function.HasStateDestructorCallback()) {
		AggregateFinalizeInputData aggr_input_data(aggr, allocator);
		aggr.function.GetStateDestructorCallback()(statef, aggr_input_data, live_count);
	}
}

void WindowNaiveLocalState::Finalize(ExecutionContext &context, WindowAggregatorGlobalState &gastate,
                                     CollectionPtr collection) {
	WindowAggregatorLocalState::Finalize(context, gastate, collection);

	//	Set up the comparison scanner just in case
	if (!comparer) {
		comparer = make_uniq<WindowCursor>(*collection, aggregator.child_idx);
	}

	//	Set up the argument ORDER BY scanner if needed
	if (!aggregator.arg_order_idx.empty() && !arg_orderer) {
		arg_orderer = make_uniq<WindowCursor>(*collection, aggregator.arg_order_idx);
		auto input_types = arg_orderer->chunk.GetTypes();
		input_types.emplace_back(LogicalType::UBIGINT);
		orderby_sink.Initialize(BufferAllocator::Get(gastate.client), input_types);

		//	The sort expressions have already been computed, so we just need to reference them
		vector<BoundOrderByNode> orders;
		for (const auto &order_by : aggregator.wexpr.ArgOrders()) {
			auto order = order_by.Copy();
			const auto &type = order.expression->GetReturnType();
			order.expression = make_uniq<BoundReferenceExpression>(type, orders.size());
			orders.emplace_back(std::move(order));
		}

		//	We only want the row numbers
		vector<idx_t> projection_map(1, input_types.size() - 1);
		orderby_scan.Initialize(BufferAllocator::Get(gastate.client), {input_types.back()});
		sort = make_uniq<Sort>(context.client, orders, input_types, projection_map);

		orderby_sel.Initialize();
	}

	// Initialise the chunks
	const auto types = cursor->chunk.GetTypes();
	if (leaves.ColumnCount() == 0 && !types.empty()) {
		leaves.Initialize(BufferAllocator::Get(context.client), types);
	}
}

void WindowNaiveLocalState::FlushStates(const WindowAggregatorGlobalState &gsink) {
	if (!flush_count) {
		return;
	}

	auto &scanned = cursor->chunk;
	leaves.Slice(scanned, update_sel, flush_count);

	const auto &aggr = gsink.aggr;
	AggregateInputData aggr_input_data(aggr, allocator);
	aggr.function.GetStateUpdateCallback()(leaves.data.data(), aggr_input_data, leaves.ColumnCount(), statep,
	                                       flush_count);

	flush_count = 0;
}

size_t WindowNaiveLocalState::Hash(idx_t rid) {
	D_ASSERT(cursor->RowIsVisible(rid));
	auto s = cursor->RowOffset(rid);
	auto &scanned = cursor->chunk;
	SelectionVector sel(&s, 1);
	leaves.Slice(scanned, sel, 1);
	leaves.Hash(hashes);

	return *FlatVector::GetData<hash_t>(hashes);
}

bool WindowNaiveLocalState::KeyEqual(const idx_t &lidx, const idx_t &ridx) {
	//	One of the indices will be scanned, so make it the left one
	auto lhs = lidx;
	auto rhs = ridx;
	if (!cursor->RowIsVisible(lhs)) {
		std::swap(lhs, rhs);
		D_ASSERT(cursor->RowIsVisible(lhs));
	}

	auto &scanned = cursor->chunk;
	auto l = cursor->RowOffset(lhs);
	SelectionVector lsel(&l, 1);

	auto rreader = cursor.get();
	if (!cursor->RowIsVisible(rhs)) {
		//	Values on different pages!
		rreader = comparer.get();
		rreader->Seek(rhs);
	}
	auto rscanned = &rreader->chunk;
	auto r = rreader->RowOffset(rhs);
	SelectionVector rsel(&r, 1);

	sel_t f = 0;
	SelectionVector fsel(&f, 1);

	for (column_t c = 0; c < scanned.ColumnCount(); ++c) {
		Vector left(scanned.data[c], lsel, 1);
		Vector right(rscanned->data[c], rsel, 1);
		if (!VectorOperations::NotDistinctFrom(left, right, nullptr, 1, nullptr, &fsel)) {
			return false;
		}
	}

	return true;
}

void WindowNaiveLocalState::Evaluate(ExecutionContext &context, const WindowAggregatorGlobalState &gsink,
                                     const DataChunk &bounds, Vector &result, idx_t count, idx_t row_idx,
                                     InterruptState &interrupt) {
	const auto &aggr = gsink.aggr;
	auto &filter_mask = gsink.filter_mask;
	const auto types = cursor->chunk.GetTypes();

	auto fdata = FlatVector::GetDataMutable<data_ptr_t>(statef);
	auto pdata = FlatVector::GetDataMutable<data_ptr_t>(statep);

	HashRow hash_row(*this);
	EqualRow equal_row(*this);
	RowSet row_set(STANDARD_VECTOR_SIZE, hash_row, equal_row);

	AggregateStateInput state_input(aggr.function, aggr.GetFunctionData());
	WindowAggregator::EvaluateSubFrames(bounds, aggregator.exclude_mode, count, row_idx, frames, [&](idx_t rid) {
		data_ptr_t agg_state = fdata[rid];
		aggr.function.GetStateInitCallback()(state_input, &agg_state, 1);
		live_count = rid + 1;

		//	Reset the DISTINCT hash table
		row_set.clear();

		// 	Sort the input rows by the argument
		if (arg_orderer) {
			auto global_sink = sort->GetGlobalSinkState(context.client);
			auto local_sink = sort->GetLocalSinkState(context);
			OperatorSinkInput sink {*global_sink, *local_sink, interrupt};

			idx_t orderby_count = 0;
			auto orderby_row = FlatVector::GetDataMutable<idx_t>(orderby_sink.data.back());
			for (const auto &frame : frames) {
				for (auto f = frame.start; f < frame.end; ++f) {
					//	FILTER before the ORDER BY
					if (!filter_mask.RowIsValid(f)) {
						continue;
					}

					if (!arg_orderer->RowIsVisible(f) || orderby_count >= STANDARD_VECTOR_SIZE) {
						if (orderby_count) {
							for (column_t c = 0; c < arg_orderer->chunk.ColumnCount(); ++c) {
								orderby_sink.data[c].Reference(arg_orderer->chunk.data[c]);
							}
							orderby_sink.Slice(orderby_sel, orderby_count);
							sort->Sink(context, orderby_sink, sink);
							orderby_sink.Reset();
						}
						orderby_count = 0;
						arg_orderer->Seek(f);
						//	Fill in the row numbers
						for (idx_t i = 0; i < arg_orderer->chunk.size(); ++i) {
							orderby_row[i] = arg_orderer->state.current_row_index + i;
						}
					}
					orderby_sel.set_index(orderby_count++, arg_orderer->RowOffset(f));
				}
			}
			if (orderby_count) {
				for (column_t c = 0; c < arg_orderer->chunk.ColumnCount(); ++c) {
					orderby_sink.data[c].Reference(arg_orderer->chunk.data[c]);
				}
				orderby_sink.Slice(orderby_sel, orderby_count);
				sort->Sink(context, orderby_sink, sink);
				orderby_sink.Reset();
			}

			OperatorSinkCombineInput combine {*global_sink, *local_sink, interrupt};
			sort->Combine(context, combine);

			OperatorSinkFinalizeInput finalize {*global_sink, interrupt};
			sort->Finalize(context.client, finalize);

			auto global_source = sort->GetGlobalSourceState(context.client, *global_sink);
			auto local_source = sort->GetLocalSourceState(context, *global_source);
			OperatorSourceInput source {*global_source, *local_source, interrupt};
			orderby_scan.Reset();
			for (; SourceResultType::FINISHED != sort->GetData(context, orderby_scan, source); orderby_scan.Reset()) {
				orderby_row = FlatVector::GetDataMutable<idx_t>(orderby_scan.data[0]);
				for (idx_t i = 0; i < orderby_scan.size(); ++i) {
					const auto f = orderby_row[i];
					//	Seek to the current position
					if (!cursor->RowIsVisible(f)) {
						//	We need to flush when we cross a chunk boundary
						FlushStates(gsink);
						cursor->Seek(f);
					}

					//	Filter out duplicates
					if (aggr.IsDistinct() && !row_set.insert(f).second) {
						continue;
					}

					pdata[flush_count] = agg_state;
					update_sel[flush_count++] = cursor->RowOffset(f);
					if (flush_count >= STANDARD_VECTOR_SIZE) {
						FlushStates(gsink);
					}
				}
			}
			return;
		}

		//	Just update the aggregate with the unfiltered input rows
		for (const auto &frame : frames) {
			for (auto f = frame.start; f < frame.end; ++f) {
				if (!filter_mask.RowIsValid(f)) {
					continue;
				}

				//	Seek to the current position
				if (!cursor->RowIsVisible(f)) {
					//	We need to flush when we cross a chunk boundary
					FlushStates(gsink);
					cursor->Seek(f);
				}

				//	Filter out duplicates
				if (aggr.IsDistinct() && !row_set.insert(f).second) {
					continue;
				}

				pdata[flush_count] = agg_state;
				update_sel[flush_count++] = cursor->RowOffset(f);
				if (flush_count >= STANDARD_VECTOR_SIZE) {
					FlushStates(gsink);
				}
			}
		}
	});

	//	Flush the final states
	FlushStates(gsink);

	//	Finalise the result aggregates and write to the result
	AggregateFinalizeInputData aggr_input_data(aggr, allocator);
	aggr.function.GetStateFinalizeCallback()(statef, aggr_input_data, result, count, 0);

	//	Destruct the result aggregates
	if (aggr.function.HasStateDestructorCallback()) {
		aggr.function.GetStateDestructorCallback()(statef, aggr_input_data, count);
		allocator.FreeAll();
	}
	live_count = 0;
}

unique_ptr<LocalSinkState> WindowNaiveAggregator::GetLocalState(ExecutionContext &context,
                                                                const GlobalSinkState &gstate) const {
	return make_uniq<WindowNaiveLocalState>(context, *this);
}

void WindowNaiveAggregator::Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count,
                                     idx_t row_idx, OperatorSinkInput &sink) const {
	const auto &gnstate = sink.global_state.Cast<WindowAggregatorGlobalState>();
	auto &lnstate = sink.local_state.Cast<WindowNaiveLocalState>();
	lnstate.Evaluate(context, gnstate, bounds, result, count, row_idx, sink.interrupt_state);
}

bool WindowNaiveAggregator::CanAggregate(const BoundWindowExpression &wexpr) {
	if (!wexpr.AggregateFunction() || !wexpr.AggregateFunction()->CanAggregate()) {
		return false;
	}
	return true;
}

} // namespace duckdb
