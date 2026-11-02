#include "duckdb/function/window/window_distinct_aggregator.hpp"

#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/execution/merge_sort_tree.hpp"
#include "duckdb/function/window/window_aggregate_states.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

#include <numeric>
#include <thread>

namespace duckdb {

enum class WindowDistinctSortStage : uint8_t { INIT, COMBINE, FINALIZE, SORTED, FINISHED };

//===--------------------------------------------------------------------===//
// WindowDistinctAggregator
//===--------------------------------------------------------------------===//
bool WindowDistinctAggregator::CanAggregate(const BoundWindowExpression &wexpr) {
	if (!wexpr.AggregateFunction()) {
		return false;
	}

	auto &aggr = *wexpr.AggregateFunction();
	if (!aggr.CanAggregate() || aggr.CanWindow() || aggr.IsHolistic()) {
		return false;
	}

	return wexpr.Distinct() && wexpr.WindowExclude() == WindowExcludeMode::NO_OTHER && wexpr.ArgOrders().empty();
}

WindowDistinctAggregator::WindowDistinctAggregator(const BoundWindowExpression &wexpr, WindowSharedExpressions &shared,
                                                   ClientContext &context)
    : WindowAggregator(wexpr, shared), context(context) {
}

class WindowDistinctAggregatorLocalState;

class WindowDistinctAggregatorGlobalState;

class WindowDistinctSortTree : public MergeSortTree<idx_t, idx_t> {
public:
	// prev_idx, input_idx
	using ZippedTuple = std::tuple<idx_t, idx_t>;
	using ZippedElements = vector<ZippedTuple>;

	WindowDistinctSortTree(Allocator &allocator, WindowDistinctAggregatorGlobalState &gdastate, idx_t count)
	    : MergeSortTree(allocator), gdastate(gdastate) {
		//	Set up for parallel build
		build_level = 0;
		build_complete = 0;
		build_run = 0;
		build_run_length = 1;
		build_num_runs = count;
	}

	void Build(ClientContext &context, WindowDistinctAggregatorLocalState &ldastate);

protected:
	bool TryNextRun(idx_t &level_idx, idx_t &run_idx);
	void BuildRun(idx_t level_nr, idx_t i, WindowDistinctAggregatorLocalState &ldastate);

	WindowDistinctAggregatorGlobalState &gdastate;
};

class WindowDistinctAggregatorGlobalState : public WindowAggregatorGlobalState {
public:
	using ZippedTuple = WindowDistinctSortTree::ZippedTuple;
	using ZippedElements = WindowDistinctSortTree::ZippedElements;

	WindowDistinctAggregatorGlobalState(ClientContext &context, const WindowDistinctAggregator &aggregator,
	                                    idx_t group_count);

	//! Create a new local sort
	optional_ptr<LocalSinkState> InitializeLocalSort(ExecutionContext &context) const;

	bool TryPrepareNextStage(WindowDistinctAggregatorLocalState &lstate);

	//! The tree allocators.
	//! We need to hold onto them for the tree lifetime,
	//! not the lifetime of the local state that constructed part of the tree
	mutable vector<unique_ptr<ArenaAllocator>> tree_allocators;
	//! Finalize guard
	mutable mutex lock;
	//! Finalize stage
	atomic<WindowDistinctSortStage> stage;
	//! Tasks launched
	idx_t total_tasks = 0;
	//! Tasks launched
	mutable idx_t tasks_assigned;
	//! Tasks completed successfully
	mutable atomic<idx_t> tasks_completed;

	//! The aggregate arguments + partition index
	vector<LogicalType> sort_types;

	//! Sorting operations
	vector<idx_t> sort_cols;
	unique_ptr<Sort> sort;
	unique_ptr<GlobalSinkState> global_sink;
	//! Local sort sets
	mutable vector<unique_ptr<LocalSinkState>> local_sinks;
	//! The resulting sorted data
	unique_ptr<ColumnDataCollection> sorted;

	//! The MST with the distinct back pointers
	mutable MergeSortTree<ZippedTuple> zipped_tree;
	//! The merge sort tree for the aggregate.
	WindowDistinctSortTree merge_sort_tree;

	//! The actual window segment tree: an array of aggregate states that represent all the intermediate nodes
	WindowAggregateStates levels_flat_native;
	//! For each level, the starting location in the levels_flat_native array
	vector<idx_t> levels_flat_start;
};

WindowDistinctAggregatorGlobalState::WindowDistinctAggregatorGlobalState(ClientContext &client,
                                                                         const WindowDistinctAggregator &aggregator,
                                                                         idx_t group_count)
    : WindowAggregatorGlobalState(client, aggregator, group_count), stage(WindowDistinctSortStage::INIT),
      tasks_assigned(0), tasks_completed(0), zipped_tree(BufferAllocator::Get(client)),
      merge_sort_tree(BufferAllocator::Get(client), *this, group_count), levels_flat_native(client, aggr) {
	//	1:	functionComputePrevIdcs(𝑖𝑛)
	//	2:		sorted ← []
	//	We sort the aggregate arguments and use the partition index as a tie-breaker.
	//	TODO: Use a hash table?
	sort_types = aggregator.arg_types;
	sort_types.emplace_back(LogicalType::UBIGINT);

	//	All expressions will be precomputed for sharing, so we just need to reference the arguments
	vector<BoundOrderByNode> orders;
	for (const auto &type : sort_types) {
		auto expr = make_uniq<BoundReferenceExpression>(type, orders.size());
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST, std::move(expr));
		sort_cols.emplace_back(sort_cols.size());
	}

	sort = make_uniq<Sort>(client, orders, sort_types, sort_cols);
	global_sink = sort->GetGlobalSinkState(client);

	//	6:	prevIdcs ← []
	//	7:	prevIdcs[0] ← “-”
	auto &prev_idcs = zipped_tree.Allocate(group_count);

	//	To handle FILTER clauses we make the missing elements
	//	point to themselves so they won't be counted.
	for (idx_t i = 0; i < group_count; ++i) {
		prev_idcs[i] = ZippedTuple(i + 1, i);
	}

	// compute space required to store aggregation states of merge sort tree
	// this is one aggregate state per entry per level
	idx_t internal_nodes = 0;
	levels_flat_start.push_back(internal_nodes);
	for (idx_t level_nr = 0; level_nr < zipped_tree.tree.size(); ++level_nr) {
		internal_nodes += zipped_tree.tree[level_nr].first.size();
		levels_flat_start.push_back(internal_nodes);
	}
	levels_flat_native.Initialize(internal_nodes);

	merge_sort_tree.tree.reserve(zipped_tree.tree.size());
	for (idx_t level_nr = 0; level_nr < zipped_tree.tree.size(); ++level_nr) {
		auto &zipped_level = zipped_tree.tree[level_nr].first;
		WindowDistinctSortTree::Elements level(merge_sort_tree.allocator);
		WindowDistinctSortTree::Offsets cascades(merge_sort_tree.allocator);
		level.resize(zipped_level.size());
		merge_sort_tree.tree.emplace_back(std::move(level), std::move(cascades));
	}
}

optional_ptr<LocalSinkState> WindowDistinctAggregatorGlobalState::InitializeLocalSort(ExecutionContext &context) const {
	lock_guard<mutex> local_sort_guard(lock);
	auto local_sink = sort->GetLocalSinkState(context);
	++tasks_assigned;
	local_sinks.emplace_back(std::move(local_sink));

	return local_sinks.back().get();
}

class WindowDistinctAggregatorLocalState : public WindowAggregatorLocalState {
public:
	WindowDistinctAggregatorLocalState(ExecutionContext &context,
	                                   const WindowDistinctAggregatorGlobalState &aggregator);

	~WindowDistinctAggregatorLocalState() override {
		statef.Destroy();
	}

	void Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	          optional_ptr<SelectionVector> filter_sel, idx_t filtered, InterruptState &interrupt);
	void Finalize(ExecutionContext &context, WindowAggregatorGlobalState &gastate, CollectionPtr collection) override;
	void Sorted();
	void ExecuteTask(ExecutionContext &context, WindowDistinctAggregatorGlobalState &gdstate);
	void Evaluate(ExecutionContext &context, const WindowDistinctAggregatorGlobalState &gdstate,
	              const DataChunk &bounds, Vector &result, idx_t count, idx_t row_idx);

	//! Thread-local sorting data
	optional_ptr<LocalSinkState> local_sink;
	//! Finalize stage
	WindowDistinctSortStage stage = WindowDistinctSortStage::INIT;
	//! Finalize scan block index
	idx_t block_idx;
	//! Thread-local tree aggregation
	Vector update_v;
	Vector source_v;
	Vector target_v;
	DataChunk leaves;
	SelectionVector sel;

protected:
	//! Flush the accumulated intermediate states into the result states
	void FlushStates();

	//! The aggregator we are working with
	const WindowDistinctAggregatorGlobalState &gdstate;
	//! The sort input chunk
	DataChunk sort_chunk;
	//! Reused result state container for the window functions
	WindowAggregateStates statef;
	//! A vector of pointers to "state", used for buffering intermediate aggregates
	Vector statep;
	//! Reused state pointers for combining tree elements
	Vector statel;
	//! Count of buffered values
	idx_t flush_count;
	//! The frame boundaries, used for the window functions
	SubFrames frames;
};

WindowDistinctAggregatorLocalState::WindowDistinctAggregatorLocalState(
    ExecutionContext &context, const WindowDistinctAggregatorGlobalState &gdstate)
    : WindowAggregatorLocalState(context), update_v(LogicalType::POINTER), source_v(LogicalType::POINTER),
      target_v(LogicalType::POINTER), gdstate(gdstate), statef(context.client, gdstate.aggr),
      statep(LogicalType::POINTER), statel(LogicalType::POINTER), flush_count(0) {
	InitSubFrames(frames, gdstate.aggregator.exclude_mode);

	sort_chunk.Initialize(context.client, gdstate.sort_types);

	gdstate.locals++;
}

unique_ptr<GlobalSinkState> WindowDistinctAggregator::GetGlobalState(ClientContext &context, idx_t group_count,
                                                                     const ValidityMask &partition_mask) const {
	return make_uniq<WindowDistinctAggregatorGlobalState>(context, *this, group_count);
}

void WindowDistinctAggregator::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                    idx_t input_idx, optional_ptr<SelectionVector> filter_sel, idx_t filtered,
                                    OperatorSinkInput &sink) {
	WindowAggregator::Sink(context, sink_chunk, coll_chunk, input_idx, filter_sel, filtered, sink);

	auto &ldstate = sink.local_state.Cast<WindowDistinctAggregatorLocalState>();
	ldstate.Sink(context, sink_chunk, coll_chunk, input_idx, filter_sel, filtered, sink.interrupt_state);
}

void WindowDistinctAggregatorLocalState::Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk,
                                              idx_t input_idx, optional_ptr<SelectionVector> filter_sel, idx_t filtered,
                                              InterruptState &interrupt) {
	//	3: 	for i ← 0 to in.size do
	//	4: 		sorted[i] ← (in[i], i)
	const auto count = sink_chunk.size();
	sort_chunk.Reset();
	auto &sorted_vec = sort_chunk.data.back();
	auto sorted = FlatVector::GetDataMutable<idx_t>(sorted_vec);
	std::iota(sorted, sorted + count, input_idx);
	FlatVector::SetSize(sorted_vec, count_t(count));

	// Our arguments are being fully materialised,
	// but we also need them as sort keys.
	auto &child_idx = gdstate.aggregator.child_idx;
	for (column_t c = 0; c < child_idx.size(); ++c) {
		sort_chunk.data[c].Reference(coll_chunk.data[child_idx[c]]);
	}

	//	Apply FILTER clause, if any
	if (filter_sel) {
		sort_chunk.Slice(*filter_sel, filtered);
	}

	if (!local_sink) {
		local_sink = gdstate.InitializeLocalSort(context);
	}

	OperatorSinkInput sink {*gdstate.global_sink, *local_sink, interrupt};
	gdstate.sort->Sink(context, sort_chunk, sink);
}

void WindowDistinctAggregatorLocalState::Finalize(ExecutionContext &context, WindowAggregatorGlobalState &gastate,
                                                  CollectionPtr collection) {
	WindowAggregatorLocalState::Finalize(context, gastate, collection);

	//! Input data chunk, used for leaf segment aggregation
	leaves.Initialize(context.client, cursor->chunk.GetTypes());
	sel.Initialize();
}

void WindowDistinctAggregatorLocalState::ExecuteTask(ExecutionContext &context,
                                                     WindowDistinctAggregatorGlobalState &gdstate) {
	switch (stage) {
	case WindowDistinctSortStage::COMBINE: {
		auto &local_sink = *gdstate.local_sinks[block_idx];
		InterruptState interrupt_state;
		OperatorSinkCombineInput combine {*gdstate.global_sink, local_sink, interrupt_state};
		gdstate.sort->Combine(context, combine);
		break;
	}
	case WindowDistinctSortStage::FINALIZE: {
		//	5: Sort sorted lexicographically increasing
		auto &sort = *gdstate.sort;
		InterruptState interrupt;
		OperatorSinkFinalizeInput finalize {*gdstate.global_sink, interrupt};
		sort.Finalize(context.client, finalize);
		auto sort_global = sort.GetGlobalSourceState(context.client, *gdstate.global_sink);
		auto sort_local = sort.GetLocalSourceState(context, *sort_global);
		OperatorSourceInput source {*sort_global, *sort_local, interrupt};
		sort.MaterializeColumnData(context, source);
		gdstate.sorted = sort.GetColumnData(source);
		break;
	}
	case WindowDistinctSortStage::SORTED:
		Sorted();
		break;
	default:
		break;
	}

	//	Completion means success: a failed task never completes its stage,
	//	and the threads waiting for the stage leave through the executor's interrupt.
	++gdstate.tasks_completed;
}

bool WindowDistinctAggregatorGlobalState::TryPrepareNextStage(WindowDistinctAggregatorLocalState &lstate) {
	lock_guard<mutex> stage_guard(lock);

	switch (stage.load()) {
	case WindowDistinctSortStage::INIT:
		total_tasks = local_sinks.size();
		tasks_assigned = 0;
		tasks_completed = 0;
		lstate.stage = stage = WindowDistinctSortStage::COMBINE;
		lstate.block_idx = tasks_assigned++;
		return true;
	case WindowDistinctSortStage::COMBINE:
		if (tasks_assigned < total_tasks) {
			lstate.stage = WindowDistinctSortStage::COMBINE;
			lstate.block_idx = tasks_assigned++;
			return true;
		} else if (tasks_completed < tasks_assigned) {
			return false;
		}
		// All combines are done, so move on to materialising the sorted data (1 task)
		total_tasks = 1;
		tasks_completed = 0;
		tasks_assigned = 0;
		lstate.stage = stage = WindowDistinctSortStage::FINALIZE;
		lstate.block_idx = tasks_assigned++;
		return true;
	case WindowDistinctSortStage::FINALIZE:
		if (tasks_completed < tasks_assigned) {
			//	Wait for the single task to finish
			return false;
		}
		//	Move on to building the tree in parallel
		total_tasks = local_sinks.size();
		tasks_completed = 0;
		tasks_assigned = 0;
		lstate.stage = stage = WindowDistinctSortStage::SORTED;
		lstate.block_idx = tasks_assigned++;
		return true;
	case WindowDistinctSortStage::SORTED:
		if (tasks_assigned < total_tasks) {
			lstate.stage = WindowDistinctSortStage::SORTED;
			lstate.block_idx = tasks_assigned++;
			return true;
		} else if (tasks_completed < tasks_assigned) {
			lstate.stage = WindowDistinctSortStage::FINISHED;
			// Sleep while other tasks finish
			return false;
		}
		break;
	default:
		break;
	}

	lstate.stage = stage = WindowDistinctSortStage::FINISHED;

	return true;
}

void WindowDistinctAggregator::Finalize(ExecutionContext &context, CollectionPtr collection, const FrameStats &stats,
                                        OperatorSinkInput &sink) {
	auto &gdsink = sink.global_state.Cast<WindowDistinctAggregatorGlobalState>();
	auto &ldstate = sink.local_state.Cast<WindowDistinctAggregatorLocalState>();
	ldstate.Finalize(context, gdsink, collection);

	// Sort, merge and build the tree in parallel
	while (gdsink.stage.load() != WindowDistinctSortStage::FINISHED) {
		//	A failed task never completes its stage, so the waiting threads
		//	leave through the interrupt that the executor raises for the failure
		context.client.InterruptCheck();
		if (gdsink.TryPrepareNextStage(ldstate)) {
			ldstate.ExecuteTask(context, gdsink);
		} else {
			std::this_thread::yield();
		}
	}

	//	These are a parallel implementations,
	//	so every thread can call them.
	gdsink.zipped_tree.Build(context.client);
	gdsink.merge_sort_tree.Build(context.client, ldstate);

	++gdsink.finalized;
}

void WindowDistinctAggregatorLocalState::Sorted() {
	using ZippedTuple = WindowDistinctAggregatorGlobalState::ZippedTuple;
	auto &collection = *gdstate.sorted;
	auto &prev_idcs = gdstate.zipped_tree.LowestLevel();
	auto &aggregator = gdstate.aggregator;

	// Find our chunk range
	const auto block_begin = (block_idx * collection.ChunkCount()) / gdstate.total_tasks;
	const auto block_end = ((block_idx + 1) * collection.ChunkCount()) / gdstate.total_tasks;

	const auto &scan_cols = gdstate.sort_cols;
	const auto key_count = aggregator.arg_types.size();

	//	Setting up the first row is a bit tricky - we have to scan the first value ourselves
	WindowCollectionChunkScanner scanner(collection, scan_cols, block_begin ? block_begin - 1 : 0);
	auto &scanned = scanner.chunk;
	if (!scanner.Scan()) {
		return;
	}

	idx_t prev_i = 0;
	if (!block_begin) {
		// First block, so set up initial sentinel
		auto input_idx = FlatVector::GetData<idx_t>(scanned.data.back());
		prev_i = input_idx[0];
		prev_idcs[prev_i] = ZippedTuple(0, prev_i);
	} else {
		// Move to the to end of the previous block
		// so we can record the comparison result for the first row
		auto input_idx = FlatVector::GetData<idx_t>(scanned.data.back());
		auto scan_idx = scanned.size() - 1;
		prev_i = input_idx[scan_idx];
	}

	//	8:	for i ← 1 to in.size do
	WindowDeltaScanner(collection, block_begin, block_end, scan_cols, key_count,
	                   [&](const idx_t row_idx, DataChunk &prev, DataChunk &curr, const idx_t ndistinct,
	                       SelectionVector &distinct, const SelectionVector &matching) {
		                   const auto count = MinValue<idx_t>(prev.size(), curr.size());

		                   // The input index has probably been sliced.
		                   auto input_idx = curr.data.back().Values<idx_t>();

		                   const auto nmatch = count - ndistinct;
		                   //	9:	if sorted[i].first == sorted[i-1].first then
		                   //	10:		prevIdcs[i] ← sorted[i-1].second
		                   for (idx_t j = 0; j < nmatch; ++j) {
			                   auto scan_idx = matching.get_index(j);
			                   auto i = input_idx[scan_idx].GetValueUnsafe();
			                   auto second = scan_idx ? input_idx[scan_idx - 1].GetValueUnsafe() : prev_i;
			                   prev_idcs[i] = ZippedTuple(second + 1, i);
		                   }
		                   //	11:	else
		                   //	12:		prevIdcs[i] ← “-”
		                   for (idx_t j = 0; j < ndistinct; ++j) {
			                   auto scan_idx = distinct.get_index(j);
			                   auto i = input_idx[scan_idx].GetValueUnsafe();
			                   prev_idcs[i] = ZippedTuple(0, i);
		                   }

		                   //	Remember the last input_idx of this chunk.
		                   prev_i = input_idx[count - 1].GetValueUnsafe();
	                   });

	//	13:	return prevIdcs
}

bool WindowDistinctSortTree::TryNextRun(idx_t &level_idx, idx_t &run_idx) {
	const auto fanout = FANOUT;

	lock_guard<mutex> stage_guard(build_lock);

	//	Verify we are not done
	if (build_level >= tree.size()) {
		return false;
	}

	// Finished with this level?
	if (build_complete >= build_num_runs) {
		auto &zipped_tree = gdastate.zipped_tree;
		std::swap(tree[build_level].second, zipped_tree.tree[build_level].second);

		++build_level;
		if (build_level >= tree.size()) {
			zipped_tree.tree.clear();
			return false;
		}

		const auto count = LowestLevel().size();
		build_run_length *= fanout;
		build_num_runs = (count + build_run_length - 1) / build_run_length;
		build_run = 0;
		build_complete = 0;
	}

	// If all runs are in flight,
	// yield until the next level is ready
	if (build_run >= build_num_runs) {
		return false;
	}

	level_idx = build_level;
	run_idx = build_run++;

	return true;
}

void WindowDistinctSortTree::Build(ClientContext &context, WindowDistinctAggregatorLocalState &ldastate) {
	//	Fan in parent levels until we are at the top
	//	Note that we don't build the top layer as that would just be all the data.
	while (build_level.load() < tree.size()) {
		//	A failed run never completes its level, so the waiting threads
		//	leave through the interrupt that the executor raises for the failure
		context.InterruptCheck();
		idx_t level_idx;
		idx_t run_idx;
		if (TryNextRun(level_idx, run_idx)) {
			BuildRun(level_idx, run_idx, ldastate);
		} else {
			std::this_thread::yield();
		}
	}
}

void WindowDistinctSortTree::BuildRun(idx_t level_nr, idx_t run_idx, WindowDistinctAggregatorLocalState &ldastate) {
	auto &aggr = gdastate.aggr;
	auto &inputs = ldastate.cursor->chunk;
	auto &levels_flat_native = gdastate.levels_flat_native;

	//! Input data chunk, used for leaf segment aggregation
	auto &leaves = ldastate.leaves;
	auto &sel = ldastate.sel;

	AggregateInputData aggr_input_data(aggr, ldastate.allocator);

	//! The states to update
	auto &update_v = ldastate.update_v;
	auto updates = FlatVector::ScatterWriter<data_ptr_t>(update_v);

	auto &source_v = ldastate.source_v;
	auto sources = FlatVector::ScatterWriter<data_ptr_t>(source_v);
	auto &target_v = ldastate.target_v;
	auto targets = FlatVector::ScatterWriter<data_ptr_t>(target_v);

	auto &zipped_tree = gdastate.zipped_tree;
	auto &zipped_level = zipped_tree.tree[level_nr].first;
	auto &level = tree[level_nr].first;

	//	Reset the combine state
	idx_t nupdate = 0;
	idx_t ncombine = 0;
	data_ptr_t prev_state = nullptr;
	idx_t i = run_idx * build_run_length;
	auto next_limit = MinValue<idx_t>(zipped_level.size(), i + build_run_length);
	idx_t levels_flat_offset = level_nr * zipped_level.size() + i;
	for (auto j = i; j < next_limit; ++j) {
		//	Initialise the next aggregate
		auto curr_state = levels_flat_native.GetStatePtr(levels_flat_offset++);

		//	Update this state (if it matches)
		const auto prev_idx = std::get<0>(zipped_level[j]);
		level[j] = prev_idx;
		if (prev_idx < i + 1) {
			const auto update_idx = std::get<1>(zipped_level[j]);
			if (!ldastate.cursor->RowIsVisible(update_idx)) {
				// 	Flush if we have to move the cursor
				//	Push the updates first so they propagate
				leaves.Reference(inputs);
				leaves.Slice(sel, nupdate);
				aggr.function.GetStateUpdateCallback()(leaves.data.data(), aggr_input_data, leaves.ColumnCount(),
				                                       update_v, nupdate);
				nupdate = 0;

				//	Combine the states sequentially
				aggr.function.GetStateCombineCallback()(source_v, target_v, aggr_input_data, ncombine);
				ncombine = 0;

				// Move the update into range.
				ldastate.cursor->Seek(update_idx);
			}

			updates[nupdate] = curr_state;
			//	input_idx
			sel[nupdate] = ldastate.cursor->RowOffset(update_idx);
			++nupdate;
		}

		//	Merge the previous state (if any)
		if (prev_state) {
			sources[ncombine] = prev_state;
			targets[ncombine] = curr_state;
			++ncombine;
		}
		prev_state = curr_state;

		//	Flush the states if one is maxed out.
		if (MaxValue<idx_t>(ncombine, nupdate) >= STANDARD_VECTOR_SIZE) {
			//	Push the updates first so they propagate
			leaves.Reference(inputs);
			leaves.Slice(sel, nupdate);
			aggr.function.GetStateUpdateCallback()(leaves.data.data(), aggr_input_data, leaves.ColumnCount(), update_v,
			                                       nupdate);
			nupdate = 0;

			//	Combine the states sequentially
			aggr.function.GetStateCombineCallback()(source_v, target_v, aggr_input_data, ncombine);
			ncombine = 0;
		}
	}

	//	Flush any remaining states
	if (ncombine || nupdate) {
		//	Push  the updates
		leaves.Reference(inputs);
		leaves.Slice(sel, nupdate);
		aggr.function.GetStateUpdateCallback()(leaves.data.data(), aggr_input_data, leaves.ColumnCount(), update_v,
		                                       nupdate);
		nupdate = 0;

		//	Combine the states sequentially
		aggr.function.GetStateCombineCallback()(source_v, target_v, aggr_input_data, ncombine);
		ncombine = 0;
	}

	++build_complete;
}

void WindowDistinctAggregatorLocalState::FlushStates() {
	if (!flush_count) {
		return;
	}

	const auto &aggr = gdstate.aggr;
	AggregateInputData aggr_input_data(aggr, allocator);
	statel.Verify();
	aggr.function.GetStateCombineCallback()(statel, statep, aggr_input_data, flush_count);

	flush_count = 0;
}

void WindowDistinctAggregatorLocalState::Evaluate(ExecutionContext &context,
                                                  const WindowDistinctAggregatorGlobalState &gdstate,
                                                  const DataChunk &bounds, Vector &result, idx_t count, idx_t row_idx) {
	auto ldata = FlatVector::GetDataMutable<const_data_ptr_t>(statel);
	auto pdata = FlatVector::GetDataMutable<data_ptr_t>(statep);

	const auto &merge_sort_tree = gdstate.merge_sort_tree;
	const auto &levels_flat_native = gdstate.levels_flat_native;
	const auto exclude_mode = gdstate.aggregator.exclude_mode;

	//	Build the finalise vector that just points to the result states
	statef.Initialize(count);

	WindowAggregator::EvaluateSubFrames(bounds, exclude_mode, count, row_idx, frames, [&](idx_t rid) {
		auto agg_state = statef.GetStatePtr(rid);

		//	TODO: Extend AggregateLowerBound to handle subframes, just like SelectNth.
		const auto lower = frames[0].start;
		const auto upper = frames[0].end;
		merge_sort_tree.AggregateLowerBound(lower, upper, lower + 1,
		                                    [&](idx_t level, const idx_t run_begin, const idx_t run_pos) {
			                                    if (run_pos != run_begin) {
				                                    //	Find the source aggregate
				                                    // Buffer a merge of the indicated state into the current state
				                                    const auto agg_idx = gdstate.levels_flat_start[level] + run_pos - 1;
				                                    const auto running_agg = levels_flat_native.GetStatePtr(agg_idx);
				                                    pdata[flush_count] = agg_state;
				                                    ldata[flush_count++] = running_agg;
				                                    if (flush_count >= STANDARD_VECTOR_SIZE) {
					                                    FlushStates();
				                                    }
			                                    }
		                                    });
	});

	//	Flush the final states
	FlushStates();

	//	Finalise the result aggregates and write to the result
	statef.Finalize(result);

	//	Destruct any non-POD state
	statef.Destroy();
}

unique_ptr<LocalSinkState> WindowDistinctAggregator::GetLocalState(ExecutionContext &context,
                                                                   const GlobalSinkState &gstate) const {
	auto &gdstate = gstate.Cast<const WindowDistinctAggregatorGlobalState>();
	return make_uniq<WindowDistinctAggregatorLocalState>(context, gdstate);
}

void WindowDistinctAggregator::Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count,
                                        idx_t row_idx, OperatorSinkInput &sink) const {
	const auto &gdstate = sink.global_state.Cast<WindowDistinctAggregatorGlobalState>();
	auto &ldstate = sink.local_state.Cast<WindowDistinctAggregatorLocalState>();
	ldstate.Evaluate(context, gdstate, bounds, result, count, row_idx);
}

} // namespace duckdb
