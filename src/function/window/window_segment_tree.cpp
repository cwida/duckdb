#include "duckdb/function/window/window_segment_tree.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/function/window/window_aggregate_states.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// WindowSegmentTree
//===--------------------------------------------------------------------===//
bool WindowSegmentTree::CanAggregate(const BoundWindowExpression &wexpr) {
	if (!wexpr.AggregateFunction()) {
		return false;
	}

	auto &aggr = *wexpr.AggregateFunction();
	if (!aggr.CanAggregate()) {
		return false;
	}

	//	Don't use segment trees for custom windowing or holistic aggregates
	if (aggr.CanWindow() || aggr.IsHolistic()) {
		return false;
	}

	//	We can't handle DISTINCT, ORDER BY args or () args (COUNT(*))
	return !wexpr.Distinct() && wexpr.ArgOrders().empty() && !wexpr.GetChildren().empty();
}

WindowSegmentTree::WindowSegmentTree(const BoundWindowExpression &wexpr, WindowSharedExpressions &shared)
    : WindowAggregator(wexpr, shared) {
}

class WindowSegmentTreeGlobalState : public WindowAggregatorGlobalState {
public:
	using AtomicCounters = vector<std::atomic<idx_t>>;

	WindowSegmentTreeGlobalState(ClientContext &context, const WindowSegmentTree &aggregator, idx_t group_count,
	                             const ValidityMask &partition_mask);

	//! The owning aggregator
	const WindowSegmentTree &tree;
	//! The actual window segment tree: an array of aggregate states that represent all the intermediate nodes
	WindowAggregateStates levels_flat_native;
	//! For each level, the starting location in the levels_flat_native array
	vector<idx_t> levels_flat_start;
	//! The level being built (read)
	std::atomic<idx_t> build_level;
	//! The number of entries started so far at each level
	unique_ptr<AtomicCounters> build_started;
	//! The number of entries completed so far at each level
	unique_ptr<AtomicCounters> build_completed;
	//! Whether building an entry failed - the level being built will then never be completed
	std::atomic<bool> build_failed;
	//! The error that made building an entry fail
	mutex build_lock;
	ErrorData build_error;
	//! The threads waiting for the level being built to be completed
	vector<InterruptState> build_blocked;

	//! Unblock the threads waiting for the level being built (once it is completed, or failed)
	void UnblockBuild() {
		vector<InterruptState> blocked;
		{
			const lock_guard<mutex> guard(build_lock);
			blocked.swap(build_blocked);
		}
		for (auto &state : blocked) {
			state.Callback();
		}
	}
	//! The tree allocators.
	//! We need to hold onto them for the tree lifetime,
	//! not the lifetime of the local state that constructed part of the tree
	vector<unique_ptr<ArenaAllocator>> tree_allocators;

	// TREE_FANOUT needs to cleanly divide STANDARD_VECTOR_SIZE
	static constexpr idx_t TREE_FANOUT = 16;
};

class WindowSegmentTreePart {
public:
	//! Right side nodes need to be cached and processed in reverse order
	using RightEntry = std::pair<idx_t, idx_t>;

	enum FramePart : uint8_t { FULL = 0, LEFT = 1, RIGHT = 2 };

	WindowSegmentTreePart(ArenaAllocator &allocator, const AggregateObject &aggr, unique_ptr<WindowCursor> cursor,
	                      const ValidityArray &filter_mask);
	~WindowSegmentTreePart();

	unique_ptr<WindowSegmentTreePart> Copy() const {
		return make_uniq<WindowSegmentTreePart>(allocator, aggr, cursor->Copy(), filter_mask);
	}

	void FlushStates(bool combining);
	void ExtractFrame(idx_t begin, idx_t end, data_ptr_t current_state);
	void WindowSegmentValue(const WindowSegmentTreeGlobalState &tree, idx_t l_idx, idx_t begin, idx_t end,
	                        data_ptr_t current_state);
	//! Writes result and calls destructors
	void Finalize(Vector &result, idx_t count);
	//! Calls the destructors of the live accumulation states (statef) without finalizing
	void Destroy();

	void Combine(WindowSegmentTreePart &other, idx_t count);

	void Evaluate(const WindowSegmentTreeGlobalState &tree, const idx_t *begins, const idx_t *ends, const idx_t *bounds,
	              Vector &result, idx_t count, idx_t row_idx, FramePart frame_part);

protected:
	//! Initialises the accumulation state vector (statef)
	void Initialize(idx_t count);
	//! Accumulate upper tree levels
	void EvaluateUpperLevels(const WindowSegmentTreeGlobalState &tree, const idx_t *begins, const idx_t *ends,
	                         const idx_t *bounds, idx_t count, idx_t row_idx, FramePart frame_part);
	void EvaluateLeaves(const WindowSegmentTreeGlobalState &tree, const idx_t *begins, const idx_t *ends,
	                    const idx_t *bounds, idx_t count, idx_t row_idx, FramePart frame_part, FramePart leaf_part);

	static inline const idx_t *FrameBegins(const idx_t *begins, const idx_t *ends, const idx_t *bounds,
	                                       FramePart frame_part) {
		return frame_part == FramePart::RIGHT ? bounds : begins;
	}
	static inline const idx_t *FrameEnds(const idx_t *begins, const idx_t *ends, const idx_t *bounds,
	                                     FramePart frame_part) {
		return frame_part == FramePart::LEFT ? bounds : ends;
	}

public:
	//! Allocator for aggregates
	ArenaAllocator &allocator;
	//! The aggregate function
	const AggregateObject &aggr;
	//! Order insensitive aggregate (we can optimise internal combines)
	const bool order_insensitive;
	//! The filtered rows in inputs
	const ValidityArray &filter_mask;
	//! The size of a single aggregate state
	const idx_t state_size;
	//! Data pointer that contains a vector of states, used for intermediate window segment aggregation
	vector<data_t> state;
	//! Scanned data state
	unique_ptr<WindowCursor> cursor;
	//! Input data chunk, used for leaf segment aggregation
	DataChunk leaves;
	//! The filtered rows in inputs.
	SelectionVector filter_sel;
	//! A vector of pointers to "state", used for intermediate window segment aggregation
	Vector statep;
	//! Reused state pointers for combining segment tree levels
	Vector statel;
	//! Reused result state container for the window functions
	Vector statef;
	//! Count of buffered values
	idx_t flush_count;
	//! Number of initialized accumulation states that have not been destroyed yet
	idx_t live_count;
	//! Cache of right side tree ranges for ordered aggregates
	vector<RightEntry> right_stack;
};

class WindowSegmentTreeLocalState : public WindowAggregatorLocalState {
public:
	explicit WindowSegmentTreeLocalState(ExecutionContext &context) : WindowAggregatorLocalState(context) {
	}

	//! Build the tree in parallel with the other threads (see window_finalize_function_t for the result)
	SourceResultType BuildTree(ExecutionContext &context, WindowSegmentTreeGlobalState &gstate,
	                           CollectionPtr collection, const InterruptState &interrupt);
	void Evaluate(ExecutionContext &context, const WindowSegmentTreeGlobalState &gsink, const DataChunk &bounds,
	              Vector &result, idx_t count, idx_t row_idx);
	//! The left (default) segment tree part
	unique_ptr<WindowSegmentTreePart> part;
	//! The right segment tree part (for EXCLUDE)
	unique_ptr<WindowSegmentTreePart> right_part;
};

SourceResultType WindowSegmentTree::Finalize(ExecutionContext &context, CollectionPtr collection,
                                             const FrameStats &stats, OperatorSinkInput &sink) {
	auto result = WindowAggregator::Finalize(context, collection, stats, sink);
	if (result != SourceResultType::FINISHED) {
		return result;
	}

	auto &gasink = sink.global_state.Cast<WindowSegmentTreeGlobalState>();
	auto &ltstate = sink.local_state.Cast<WindowSegmentTreeLocalState>();
	result = ltstate.BuildTree(context, gasink, collection, sink.interrupt_state);
	if (result != SourceResultType::FINISHED) {
		return result;
	}

	++gasink.finalized;
	return SourceResultType::FINISHED;
}

WindowSegmentTreePart::WindowSegmentTreePart(ArenaAllocator &allocator, const AggregateObject &aggr,
                                             unique_ptr<WindowCursor> cursor_p, const ValidityArray &filter_mask)
    : allocator(allocator), aggr(aggr),
      order_insensitive(aggr.function.GetOrderDependent() == AggregateOrderDependent::NOT_ORDER_DEPENDENT),
      filter_mask(filter_mask), state_size(aggr.function.GetStateSize(aggr.GetFunctionData())),
      state(state_size * STANDARD_VECTOR_SIZE), cursor(std::move(cursor_p)), statep(LogicalType::POINTER),
      statel(LogicalType::POINTER), statef(LogicalType::POINTER), flush_count(0), live_count(0) {
	auto &inputs = cursor->chunk;
	if (inputs.ColumnCount() > 0) {
		leaves.Initialize(Allocator::DefaultAllocator(), inputs.GetTypes());
		filter_sel.Initialize();
	}

	//	Build the finalise vector that just points to the result states
	data_ptr_t state_ptr = state.data();
	D_ASSERT(statef.GetVectorType() == VectorType::FLAT_VECTOR);
	statef.SetVectorType(VectorType::CONSTANT_VECTOR);
	statef.Flatten();
	auto fdata = FlatVector::GetDataMutable<data_ptr_t>(statef);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; ++i) {
		fdata[i] = state_ptr;
		state_ptr += state_size;
	}
}

WindowSegmentTreePart::~WindowSegmentTreePart() {
	// destroy any states left live by an exception during evaluation
	Destroy();
}

unique_ptr<GlobalSinkState> WindowSegmentTree::GetGlobalState(ClientContext &context, idx_t group_count,
                                                              const ValidityMask &partition_mask) const {
	return make_uniq<WindowSegmentTreeGlobalState>(context, *this, group_count, partition_mask);
}

unique_ptr<LocalSinkState> WindowSegmentTree::GetLocalState(ExecutionContext &context,
                                                            const GlobalSinkState &gstate) const {
	return make_uniq<WindowSegmentTreeLocalState>(context);
}

void WindowSegmentTreePart::FlushStates(bool combining) {
	if (!flush_count) {
		return;
	}

	AggregateInputData aggr_input_data(aggr, allocator);
	if (combining) {
		statel.Verify();
		aggr.function.GetStateCombineCallback()(statel, statep, aggr_input_data, flush_count);
	} else {
		auto &scanned = cursor->chunk;
		leaves.Slice(scanned, filter_sel, flush_count);
		aggr.function.GetStateUpdateCallback()(&leaves.data[0], aggr_input_data, leaves.ColumnCount(), statep,
		                                       flush_count);
	}

	flush_count = 0;
}

void WindowSegmentTreePart::Combine(WindowSegmentTreePart &other, idx_t count) {
	AggregateInputData aggr_input_data(aggr, allocator);
	aggr.function.GetStateCombineCallback()(other.statef, statef, aggr_input_data, count);
}

void WindowSegmentTreePart::ExtractFrame(idx_t begin, idx_t end, data_ptr_t state_ptr) {
	const auto count = end - begin;

	//	If we are not filtering,
	//	just update the shared dictionary selection to the range
	//	Otherwise set it to the input rows that pass the filter
	auto states = FlatVector::GetDataMutable<data_ptr_t>(statep);
	if (filter_mask.CannotHaveNull()) {
		auto offset = cursor->RowOffset(begin);
		idx_t remaining = count;
		while (remaining > 0) {
			const auto batch_count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE - flush_count);
			for (idx_t i = 0; i < batch_count; ++i) {
				states[flush_count + i] = state_ptr;
				filter_sel.set_index(flush_count + i, offset + i);
			}
			flush_count += batch_count;
			offset += batch_count;
			remaining -= batch_count;
			if (flush_count >= STANDARD_VECTOR_SIZE) {
				FlushStates(false);
			}
		}
	} else {
		for (idx_t i = begin; i < end; ++i) {
			if (filter_mask.RowIsValid(i)) {
				states[flush_count] = state_ptr;
				filter_sel.set_index(flush_count++, cursor->RowOffset(i));
				if (flush_count >= STANDARD_VECTOR_SIZE) {
					FlushStates(false);
				}
			}
		}
	}
}

void WindowSegmentTreePart::WindowSegmentValue(const WindowSegmentTreeGlobalState &tree, idx_t l_idx, idx_t begin,
                                               idx_t end, data_ptr_t state_ptr) {
	D_ASSERT(begin <= end);
	auto &inputs = cursor->chunk;
	if (begin == end || inputs.ColumnCount() == 0) {
		return;
	}

	const auto count = end - begin;
	if (l_idx == 0) {
		//	Check the leaves when they cross chunk boundaries
		while (begin < end) {
			if (!cursor->RowIsVisible(begin)) {
				FlushStates(false);
				cursor->Seek(begin);
			}
			auto next = MinValue(end, cursor->state.next_row_index);
			ExtractFrame(begin, next, state_ptr);
			begin = next;
		}
	} else {
		// find out where the states begin
		auto begin_ptr = tree.levels_flat_native.GetStatePtr(begin + tree.levels_flat_start[l_idx - 1]);
		// set up a vector of pointers that point towards the set of states
		auto ldata = FlatVector::GetDataMutable<const_data_ptr_t>(statel);
		auto pdata = FlatVector::GetDataMutable<data_ptr_t>(statep);
		for (idx_t i = 0; i < count; i++) {
			pdata[flush_count] = state_ptr;
			ldata[flush_count++] = begin_ptr;
			begin_ptr += state_size;
			if (flush_count >= STANDARD_VECTOR_SIZE) {
				FlushStates(true);
			}
		}
	}
}
void WindowSegmentTreePart::Finalize(Vector &result, idx_t count) {
	//	Finalise the result aggregates and write to result if write_result is set
	AggregateFinalizeInputData aggr_input_data(aggr, allocator);
	aggr.function.GetStateFinalizeCallback()(statef, aggr_input_data, result, count, 0);

	//	Destruct the result aggregates
	if (aggr.function.HasStateDestructorCallback()) {
		aggr.function.GetStateDestructorCallback()(statef, aggr_input_data, count);
	}
	live_count = 0;
}

void WindowSegmentTreePart::Destroy() {
	if (!live_count) {
		return;
	}
	if (aggr.function.HasStateDestructorCallback()) {
		AggregateFinalizeInputData aggr_input_data(aggr, allocator);
		aggr.function.GetStateDestructorCallback()(statef, aggr_input_data, live_count);
	}
	live_count = 0;
}

WindowSegmentTreeGlobalState::WindowSegmentTreeGlobalState(ClientContext &client, const WindowSegmentTree &aggregator,
                                                           idx_t group_count, const ValidityMask &partition_mask)
    : WindowAggregatorGlobalState(client, aggregator, group_count), tree(aggregator), levels_flat_native(client, aggr) {
	D_ASSERT(!aggregator.wexpr.GetChildren().empty());

	//	We need the partition offsets to avoid combining across partitions.
	BuildPartitionOffsets(group_count, partition_mask);

	// compute space required to store internal nodes of segment tree
	levels_flat_start.push_back(0);

	idx_t levels_flat_offset = 0;
	idx_t level_current = 0;
	// level 0 is data itself
	idx_t level_size;
	// iterate over the levels of the segment tree
	while ((level_size =
	            (level_current == 0 ? group_count : levels_flat_offset - levels_flat_start[level_current - 1])) > 1) {
		for (idx_t pos = 0; pos < level_size; pos += TREE_FANOUT) {
			levels_flat_offset++;
		}

		levels_flat_start.push_back(levels_flat_offset);
		level_current++;
	}

	// Corner case: single element in the window
	if (levels_flat_offset == 0) {
		++levels_flat_offset;
	}

	levels_flat_native.Initialize(levels_flat_offset);

	// Start by building from the bottom level
	build_level = 0;
	build_failed = false;

	build_started = make_uniq<AtomicCounters>(levels_flat_start.size());
	for (auto &counter : *build_started) {
		counter = 0;
	}

	build_completed = make_uniq<AtomicCounters>(levels_flat_start.size());
	for (auto &counter : *build_completed) {
		counter = 0;
	}
}

SourceResultType WindowSegmentTreeLocalState::BuildTree(ExecutionContext &context, WindowSegmentTreeGlobalState &gstate,
                                                        CollectionPtr collection, const InterruptState &interrupt) {
	//	Single part for constructing the tree
	//	This can be called again after yielding or blocking, and then continues with the level being built
	auto cursor = make_uniq<WindowCursor>(*collection, gstate.aggregator.child_idx);
	const auto leaf_count = collection->size();
	auto &filter_mask = gstate.filter_mask;
	WindowSegmentTreePart gtstate(allocator, gstate.aggr, std::move(cursor), filter_mask);

	//	Some aggregates (e.g., `MIN(x, n)`) allow "partition-constant" arguments (e.g., `n`)
	//	and throw if you `Combine` across partitions. To avoid this, we only combine states
	//	that do not contain a partition boundary and use the partition mask to exclude them.
	//	The uncombined states will be empty, which is fine because they will never be used.
	const auto &partition_offsets = gstate.partition_offsets;
	idx_t combine_width = gstate.TREE_FANOUT;
	idx_t combine_level = 0;
	auto combine_begin = partition_offsets.rend();
	std::greater<idx_t> combine_cmp;

	auto &levels_flat_native = gstate.levels_flat_native;
	const auto &levels_flat_start = gstate.levels_flat_start;
	// iterate over the levels of the segment tree
	for (;;) {
		const idx_t level_current = gstate.build_level.load();
		if (level_current >= levels_flat_start.size()) {
			break;
		}

		// level 0 is data itself
		const auto level_size =
		    (level_current == 0 ? leaf_count : levels_flat_start[level_current] - levels_flat_start[level_current - 1]);
		if (level_size <= 1) {
			break;
		}
		const idx_t build_count = (level_size + gstate.TREE_FANOUT - 1) / gstate.TREE_FANOUT;

		// Keep adjacent leaves on the same cursor to reuse its pinned input chunk.
		const idx_t build_batch =
		    level_current == 0 ? MaxValue<idx_t>(1, STANDARD_VECTOR_SIZE / gstate.TREE_FANOUT) : 1;
		const idx_t build_begin = (*gstate.build_started).at(level_current).fetch_add(build_batch);
		if (build_begin >= build_count) {
			//	Nothing left at this level, so wait until other threads are done.
			{
				const lock_guard<mutex> guard(gstate.build_lock);
				if (level_current == gstate.build_level.load() && !gstate.build_failed) {
					if (!interrupt.CanCallback()) {
						return SourceResultType::HAVE_MORE_OUTPUT;
					}
					gstate.build_blocked.push_back(interrupt);
					return SourceResultType::BLOCKED;
				}
			}
			if (gstate.build_failed) {
				// another thread failed to build its part of the level, so the tree will never be complete
				const lock_guard<mutex> guard(gstate.build_lock);
				gstate.build_error.Throw();
			}
			continue;
		}

		//	Update the combine width to the current level.
		for (; combine_level < level_current; ++combine_level) {
			combine_width *= gstate.TREE_FANOUT;
			combine_begin = partition_offsets.rend();
		}

		const idx_t build_end = MinValue(build_begin + build_batch, build_count);
		for (idx_t build_idx = build_begin; build_idx < build_end; ++build_idx) {
			//	Check for crossing a partition boundary
			//	Note that it is OK for a state to _start_ on a partition boundary
			//	- it just shouldn't _contain_ one.
			const idx_t combine_left = build_idx * combine_width;
			const idx_t combine_right = MinValue(combine_left + combine_width, leaf_count);
			combine_begin = std::lower_bound(partition_offsets.rbegin(), combine_begin, combine_left, combine_cmp);
			auto combine_end =
			    std::lower_bound(partition_offsets.rbegin(), combine_begin, combine_right - 1, combine_cmp);
			const bool has_boundary = (combine_begin != combine_end);
			combine_begin = combine_end;

			// compute the aggregate for this entry in the segment tree
			if (!has_boundary) {
				const idx_t pos = build_idx * gstate.TREE_FANOUT;
				const idx_t levels_flat_offset = levels_flat_start[level_current] + build_idx;
				auto state_ptr = levels_flat_native.GetStatePtr(levels_flat_offset);
				try {
					gtstate.WindowSegmentValue(gstate, level_current, pos,
					                           MinValue(level_size, pos + gstate.TREE_FANOUT), state_ptr);
					gtstate.FlushStates(level_current > 0);
				} catch (std::exception &ex) {
					// wake up the threads waiting for this level to be completed
					{
						const lock_guard<mutex> guard(gstate.build_lock);
						gstate.build_error = ErrorData(ex);
					}
					gstate.build_failed = true;
					gstate.UnblockBuild();
					throw;
				}
			}
		}

		//	If that was the last one, mark the level as complete.
		const idx_t completed = build_end - build_begin;
		const idx_t build_complete = (*gstate.build_completed).at(level_current).fetch_add(completed) + completed;
		if (build_complete == build_count) {
			gstate.build_level++;
			gstate.UnblockBuild();
			continue;
		}
	}
	return SourceResultType::FINISHED;
}

void WindowSegmentTree::Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count,
                                 idx_t row_idx, OperatorSinkInput &sink) const {
	const auto &gtstate = sink.global_state.Cast<WindowSegmentTreeGlobalState>();
	auto &ltstate = sink.local_state.Cast<WindowSegmentTreeLocalState>();
	ltstate.Evaluate(context, gtstate, bounds, result, count, row_idx);
}

void WindowSegmentTreeLocalState::Evaluate(ExecutionContext &context, const WindowSegmentTreeGlobalState &gtstate,
                                           const DataChunk &bounds, Vector &result, idx_t count, idx_t row_idx) {
	auto window_begin = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
	auto window_end = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
	auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
	auto peer_end = FlatVector::GetData<const idx_t>(bounds.data[PEER_END]);

	if (!part) {
		part = make_uniq<WindowSegmentTreePart>(allocator, gtstate.aggr, cursor->Copy(), gtstate.filter_mask);
	}

	if (gtstate.aggregator.exclude_mode != WindowExcludeMode::NO_OTHER) {
		// If we exclude the current row, then both left and right need to contain it.
		const bool exclude_current = gtstate.aggregator.exclude_mode == WindowExcludeMode::CURRENT_ROW;

		// 1. evaluate the tree left of the excluded part
		auto middle = exclude_current ? peer_end : peer_begin;
		part->Evaluate(gtstate, window_begin, middle, window_end, result, count, row_idx, WindowSegmentTreePart::LEFT);

		// 2. set up a second state for the right of the excluded part
		if (!right_part) {
			right_part = part->Copy();
		}

		// 3. evaluate the tree right of the excluded part
		middle = exclude_current ? peer_begin : peer_end;
		right_part->Evaluate(gtstate, middle, window_end, window_begin, result, count, row_idx,
		                     WindowSegmentTreePart::RIGHT);

		// 4. combine the buffer state into the Segment Tree State
		part->Combine(*right_part, count);

		// right_part's states are re-initialized on every chunk, so destroy them after combining to avoid leaking
		// aggregate states that own heap memory (e.g. histogram, reservoir_quantile)
		right_part->Destroy();
	} else {
		part->Evaluate(gtstate, window_begin, window_end, nullptr, result, count, row_idx, WindowSegmentTreePart::FULL);
	}

	part->Finalize(result, count);
}

void WindowSegmentTreePart::Evaluate(const WindowSegmentTreeGlobalState &tree, const idx_t *begins, const idx_t *ends,
                                     const idx_t *bounds, Vector &result, idx_t count, idx_t row_idx,
                                     FramePart frame_part) {
	Initialize(count);

	if (order_insensitive) {
		//	First pass: aggregate the segment tree nodes with sharing
		EvaluateUpperLevels(tree, begins, ends, bounds, count, row_idx, frame_part);

		//	Second pass: aggregate the ragged leaves
		EvaluateLeaves(tree, begins, ends, bounds, count, row_idx, frame_part, FramePart::FULL);
	} else {
		//	Evaluate leaves in order
		EvaluateLeaves(tree, begins, ends, bounds, count, row_idx, frame_part, FramePart::LEFT);
		EvaluateUpperLevels(tree, begins, ends, bounds, count, row_idx, frame_part);
		EvaluateLeaves(tree, begins, ends, bounds, count, row_idx, frame_part, FramePart::RIGHT);
	}
}

void WindowSegmentTreePart::Initialize(idx_t count) {
	D_ASSERT(live_count == 0);
	auto fdata = FlatVector::GetDataMutable<data_ptr_t>(statef);
	AggregateStateInput state_input(aggr.function, aggr.GetFunctionData());
	aggr.function.GetStateInitCallback()(state_input, fdata, count);
	live_count = count;
}

void WindowSegmentTreePart::EvaluateUpperLevels(const WindowSegmentTreeGlobalState &tree, const idx_t *begins,
                                                const idx_t *ends, const idx_t *bounds, idx_t count, idx_t row_idx,
                                                FramePart frame_part) {
	auto fdata = FlatVector::GetDataMutable<data_ptr_t>(statef);

	const auto exclude_mode = tree.tree.exclude_mode;
	const bool begin_on_curr_row = frame_part == FramePart::RIGHT && exclude_mode == WindowExcludeMode::CURRENT_ROW;
	const bool end_on_curr_row = frame_part == FramePart::LEFT && exclude_mode == WindowExcludeMode::CURRENT_ROW;

	// We need the full range of the frame to clamp
	auto frame_begins = FrameBegins(begins, ends, bounds, frame_part);
	auto frame_ends = FrameEnds(begins, ends, bounds, frame_part);

	const auto max_level = tree.levels_flat_start.size() + 1;
	right_stack.resize(max_level, {0, 0});

	//	Share adjacent identical states
	//  We do this first because we want to share only tree aggregations
	idx_t prev_begin = 1;
	idx_t prev_end = 0;
	auto ldata = FlatVector::GetDataMutable<data_ptr_t>(statel);
	auto pdata = FlatVector::GetDataMutable<data_ptr_t>(statep);
	data_ptr_t prev_state = nullptr;
	for (idx_t rid = 0, cur_row = row_idx; rid < count; ++rid, ++cur_row) {
		auto state_ptr = fdata[rid];

		auto begin = MaxValue(begin_on_curr_row ? cur_row + 1 : begins[rid], frame_begins[rid]);
		auto end = MinValue(end_on_curr_row ? cur_row : ends[rid], frame_ends[rid]);
		if (begin >= end) {
			continue;
		}

		//	Skip level 0
		idx_t l_idx = 0;
		idx_t right_max = 0;
		for (; l_idx < max_level; l_idx++) {
			idx_t parent_begin = begin / tree.TREE_FANOUT;
			idx_t parent_end = end / tree.TREE_FANOUT;
			if (prev_state && l_idx == 1 && begin == prev_begin && end == prev_end) {
				//	Just combine the previous top level result
				ldata[flush_count] = prev_state;
				pdata[flush_count] = state_ptr;
				if (++flush_count >= STANDARD_VECTOR_SIZE) {
					FlushStates(true);
				}
				break;
			}

			if (order_insensitive && l_idx == 1) {
				prev_state = state_ptr;
				prev_begin = begin;
				prev_end = end;
			}

			if (parent_begin == parent_end) {
				if (l_idx) {
					WindowSegmentValue(tree, l_idx, begin, end, state_ptr);
				}
				break;
			}
			idx_t group_begin = parent_begin * tree.TREE_FANOUT;
			if (begin != group_begin) {
				if (l_idx) {
					WindowSegmentValue(tree, l_idx, begin, group_begin + tree.TREE_FANOUT, state_ptr);
				}
				parent_begin++;
			}
			idx_t group_end = parent_end * tree.TREE_FANOUT;
			if (end != group_end) {
				if (l_idx) {
					if (order_insensitive) {
						WindowSegmentValue(tree, l_idx, group_end, end, state_ptr);
					} else {
						right_stack[l_idx] = {group_end, end};
						right_max = l_idx;
					}
				}
			}
			begin = parent_begin;
			end = parent_end;
		}

		// Flush the right side values from left to right for order_sensitive aggregates
		// As we go up the tree, the right side ranges move left,
		// so we just cache them in a fixed size, preallocated array.
		// Then we can just reverse scan the array and append the cached ranges.
		for (l_idx = right_max; l_idx > 0; --l_idx) {
			auto &right_entry = right_stack[l_idx];
			const auto group_end = right_entry.first;
			const auto end = right_entry.second;
			if (end) {
				WindowSegmentValue(tree, l_idx, group_end, end, state_ptr);
				right_entry = {0, 0};
			}
		}
	}
	FlushStates(true);
}

void WindowSegmentTreePart::EvaluateLeaves(const WindowSegmentTreeGlobalState &tree, const idx_t *begins,
                                           const idx_t *ends, const idx_t *bounds, idx_t count, idx_t row_idx,
                                           FramePart frame_part, FramePart leaf_part) {
	auto fdata = FlatVector::GetDataMutable<data_ptr_t>(statef);

	// For order-sensitive aggregates, we have to process the ragged leaves in two pieces.
	// The left side have to be added before the main tree followed by the ragged right sides.
	// The current row is the leftmost value of the right hand side.
	const bool compute_left = leaf_part != FramePart::RIGHT;
	const bool compute_right = leaf_part != FramePart::LEFT;
	const auto exclude_mode = tree.tree.exclude_mode;
	const bool begin_on_curr_row = frame_part == FramePart::RIGHT && exclude_mode == WindowExcludeMode::CURRENT_ROW;
	const bool end_on_curr_row = frame_part == FramePart::LEFT && exclude_mode == WindowExcludeMode::CURRENT_ROW;
	// with EXCLUDE TIES, in addition to the frame part right of the peer group's end, we also need to consider the
	// current row
	const bool add_curr_row = compute_left && frame_part == FramePart::RIGHT && exclude_mode == WindowExcludeMode::TIES;

	// We need the full range of the frame to clamp
	auto frame_begins = FrameBegins(begins, ends, bounds, frame_part);
	auto frame_ends = FrameEnds(begins, ends, bounds, frame_part);

	for (idx_t rid = 0, cur_row = row_idx; rid < count; ++rid, ++cur_row) {
		auto state_ptr = fdata[rid];

		const auto frame_begin = frame_begins[rid];
		auto begin = MaxValue(begin_on_curr_row ? cur_row + 1 : begins[rid], frame_begin);

		const auto frame_end = frame_ends[rid];
		auto end = MinValue(end_on_curr_row ? cur_row : ends[rid], frame_end);
		if (add_curr_row && frame_begin <= cur_row && cur_row < frame_end) {
			WindowSegmentValue(tree, 0, cur_row, cur_row + 1, state_ptr);
		}
		if (begin >= end) {
			continue;
		}

		idx_t parent_begin = begin / tree.TREE_FANOUT;
		idx_t parent_end = end / tree.TREE_FANOUT;
		if (parent_begin == parent_end) {
			if (compute_left) {
				WindowSegmentValue(tree, 0, begin, end, state_ptr);
			}
			continue;
		}

		idx_t group_begin = parent_begin * tree.TREE_FANOUT;
		if (begin != group_begin && compute_left) {
			WindowSegmentValue(tree, 0, begin, group_begin + tree.TREE_FANOUT, state_ptr);
		}
		idx_t group_end = parent_end * tree.TREE_FANOUT;
		if (end != group_end && compute_right) {
			WindowSegmentValue(tree, 0, group_end, end, state_ptr);
		}
	}
	FlushStates(false);
}

} // namespace duckdb
