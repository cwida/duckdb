#include "duckdb/execution/operator/aggregate/physical_window.hpp"

#include "duckdb/common/once_flag.hpp"
#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/function/window/window_aggregate_function.hpp"
#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/function/window/window_shared_expressions.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

//	Global sink state
class WindowGlobalSinkState;

enum WindowGroupStage : uint8_t { SORT, MATERIALIZE, MASK, SINK, FINALIZE, GETDATA, DONE };

struct WindowSourceTask {
	WindowSourceTask() {
	}

	WindowGroupStage stage = WindowGroupStage::DONE;
	//! The hash group
	idx_t group_idx = 0;
	//! The thread index (for local state)
	idx_t thread_idx = 0;
	//! The first block index count
	idx_t begin_idx = 0;
	//! The end block index count
	idx_t end_idx = 0;
};

class WindowHashGroup {
public:
	using HashGroupPtr = unique_ptr<ColumnDataCollection>;
	using OrderMasks = unordered_map<idx_t, ValidityMask>;
	using ExecutorGlobalStatePtr = unique_ptr<GlobalSinkState>;
	using ExecutorGlobalStates = vector<ExecutorGlobalStatePtr>;
	using ExecutorLocalStatePtr = unique_ptr<LocalSinkState>;
	using ExecutorLocalStates = vector<ExecutorLocalStatePtr>;
	using ThreadLocalStates = vector<ExecutorLocalStates>;
	using Task = WindowSourceTask;
	using TaskPtr = optional_ptr<Task>;
	using ScannerPtr = unique_ptr<WindowCollectionChunkScanner>;
	using ChunkRow = SortStrategy::ChunkRow;

	template <typename T>
	static T BinValue(T n, T val) {
		return ((n + (val - 1)) / val);
	}

	WindowHashGroup(WindowGlobalSinkState &gsink, const ChunkRow &chunk_row, idx_t group_idx, idx_t bin_idx);

	void AllocateMasks();
	void ComputeMasks(const idx_t begin_idx, const idx_t end_idx);

	ExecutorGlobalStates &GetGlobalStates(ClientContext &client);

	//! The number of chunks in the group
	inline idx_t ChunkCount() const {
		return blocks;
	}

	// The total number of tasks we will execute per thread
	inline idx_t GetTaskCount() const {
		return GetThreadCount() * (uint8_t(WindowGroupStage::DONE) - uint8_t(WindowGroupStage::SORT));
	}
	// The total number of threads we will use
	inline idx_t GetThreadCount() const {
		return group_threads;
	}
	// Set up the task parameters
	idx_t InitTasks(idx_t per_thread);

	// Scan all of the chunks, starting at a given point
	ScannerPtr GetScanner(const idx_t begin_idx) const;
	void UpdateScanner(ScannerPtr &scanner, idx_t begin_idx) const;

	// The processing stage for this group
	WindowGroupStage GetStage() const {
		return stage;
	}

	bool TryPrepareNextStage() {
		lock_guard<mutex> prepare_guard(lock);
		switch (stage.load()) {
		case WindowGroupStage::SORT:
			if (sorted == blocks) {
				stage = WindowGroupStage::MATERIALIZE;
				return true;
			}
			return false;
		case WindowGroupStage::MATERIALIZE:
			if (materialized == blocks && rows.get()) {
				stage = WindowGroupStage::MASK;
				return true;
			}
			return false;
		case WindowGroupStage::MASK:
			if (masked == blocks) {
				stage = WindowGroupStage::SINK;
				return true;
			}
			return false;
		case WindowGroupStage::SINK:
			// Gate on blocks (not rows): every SINK task must have completed before FINALIZE
			// can run, otherwise a FINALIZE task can read a thread_states[thread_idx] entry
			// that the matching SINK task hasn't initialised yet.
			if (sunk == blocks) {
				stage = WindowGroupStage::FINALIZE;
				return true;
			}
			return false;
		case WindowGroupStage::FINALIZE:
			if (finalized == blocks) {
				stage = WindowGroupStage::GETDATA;
				return true;
			}
			return false;
		case WindowGroupStage::GETDATA:
		case WindowGroupStage::DONE:
			// never block in GETDATA
			return true;
		}

		//	Stop Linux whinging about control flow...
		return true;
	}

	bool TryNextTask(Task &task) {
		if (next_task >= GetTaskCount()) {
			return false;
		}
		const auto group_stage = GetStage();
		const auto group_threads = GetThreadCount();
		task.stage = WindowGroupStage(next_task / group_threads);
		if (task.stage == group_stage) {
			task.thread_idx = next_task % group_threads;
			task.group_idx = group_idx;
			task.begin_idx = task.thread_idx * per_thread;
			task.end_idx = MinValue<idx_t>(task.begin_idx + per_thread, ChunkCount());
			++next_task;
			return true;
		}

		return false;
	}

	//! The shared global state from sinking
	WindowGlobalSinkState &gsink;
	//! The hash partition data
	HashGroupPtr rows;
	//! The size of the group
	idx_t count = 0;
	//! The number of blocks in the group
	idx_t blocks = 0;
	//! The partition boundary mask. The masks are O(rows): their storage comes from the buffer allocator and the
	//! masks point into it
	ValidityMask partition_mask;
	AllocatedData partition_mask_data;
	//! The order boundary mask (maps sort column END to mask)
	OrderMasks order_masks;
	unordered_map<idx_t, AllocatedData> order_mask_data;
	OnceFlag masks_initialized;
	//! The fully materialised data collection
	unique_ptr<WindowCollection> collection;
	//! The processing stage for this group
	atomic<WindowGroupStage> stage;
	//! The function global states for this hash group
	ExecutorGlobalStates gestates;
	OnceFlag gestates_initialized;
	//! Executor local states, one per thread
	ThreadLocalStates thread_states;

	//! The global group number (so we can find the task
	const idx_t group_idx;
	//! The sink bin (so we can find the bin within the sink state)
	const idx_t bin_idx;
	//! Single threading lock
	mutex lock;
	//! The the number of blocks per thread.
	idx_t per_thread = 0;
	//! The the number of blocks per thread.
	idx_t group_threads = 0;
	//! The next task to process
	idx_t next_task = 0;
	//! Count of sorted run blocks
	atomic<idx_t> sorted;
	//! Count of materialized run blocks
	atomic<idx_t> materialized;
	//! Count of masked blocks
	atomic<idx_t> masked;
	//! Count of sunk blocks (one per completed SINK task block, not per row)
	atomic<idx_t> sunk;
	//! Count of finalized blocks
	atomic<idx_t> finalized;
	//! Count of completed tasks
	atomic<idx_t> completed;
	//! The output ordering batch index this hash group starts at
	idx_t batch_base;
};

class WindowGlobalSinkState : public GlobalSinkState {
public:
	using GlobalStatePtr = unique_ptr<GlobalSinkState>;
	using ExecutorPtr = unique_ptr<WindowExecutor>;
	using Executors = vector<ExecutorPtr>;

	WindowGlobalSinkState(const PhysicalWindow &op, ClientContext &context);

	//! Parent operator
	const PhysicalWindow &op;
	//! Client context
	ClientContext &client;
	//! The partitioned sunk data
	unique_ptr<SortStrategy> sort_strategy;
	//! The partitioned sunk data. With partitioning there may be more than one
	mutex lock;
	GlobalStatePtr strategy_sink;
	//! The number of sunk rows (for progress)
	atomic<idx_t> count;
	//! Keeps the sink progress monotonic
	MonotonicProgress sink_progress;
	//! The execution functions
	Executors executors;
	//! The shared expressions library
	WindowSharedExpressions shared;

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &context) override {
		// The sort strategy, executors, and shared expression layout are iteration-invariant. Only the
		// sort sink holds per-iteration materialized data, so replace that while preserving the setup.
		strategy_sink = sort_strategy->GetGlobalSinkState(context);
		count = 0;
		sink_progress.Reset();
		GlobalSinkState::Reset(context);
	}
};

//	Per-thread sink state
class WindowLocalSinkState : public LocalSinkState {
public:
	using GlobalStatePtr = optional_ptr<GlobalSinkState>;
	using LocalStatePtr = unique_ptr<LocalSinkState>;

	WindowLocalSinkState(ExecutionContext &context, const WindowGlobalSinkState &gstate)
	    : local_group(gstate.sort_strategy->GetLocalSinkState(context)) {
	}

	LocalStatePtr local_group;

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ExecutionContext &context, GlobalSinkState &gstate_p) override {
		auto &gstate = gstate_p.Cast<WindowGlobalSinkState>();
		local_group = gstate.sort_strategy->GetLocalSinkState(context);
	}
};

// this implements a sorted window functions variant
PhysicalWindow::PhysicalWindow(PhysicalPlan &physical_plan, vector<LogicalType> types,
                               vector<unique_ptr<Expression>> select_list_p, idx_t estimated_cardinality,
                               vector<column_t> partitions)
    : PhysicalOperator(physical_plan, TYPE, std::move(types), estimated_cardinality),
      select_list(std::move(select_list_p)), order_idx(0), is_order_dependent(false),
      partition_info(std::move(partitions)) {
	idx_t max_orders = 0;
	for (idx_t i = 0; i < select_list.size(); ++i) {
		auto &expr = select_list[i];
		D_ASSERT(expr->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
		auto &bound_window = expr->Cast<BoundWindowExpression>();
		if (bound_window.Partitions().empty() && bound_window.OrderBy().empty()) {
			is_order_dependent = true;
		}

		if (bound_window.OrderBy().size() > max_orders) {
			order_idx = i;
			max_orders = bound_window.OrderBy().size();
		}
	}
}

PhysicalWindow::PhysicalWindow(PhysicalPlan &physical_plan, vector<LogicalType> types,
                               vector<unique_ptr<Expression>> select_list, idx_t estimated_cardinality)
    : PhysicalWindow(physical_plan, std::move(types), std::move(select_list), estimated_cardinality,
                     vector<column_t>()) {
}

unique_ptr<WindowExecutor> WindowExecutor::Factory(BoundWindowExpression &wexpr, ClientContext &client,
                                                   WindowSharedExpressions &shared) {
	if (wexpr.GetExpressionType() == ExpressionType::WINDOW_AGGREGATE) {
		return make_uniq<WindowAggregateExecutor>(wexpr, client, shared);
	} else {
		if (!wexpr.WindowFunction().get()) {
			throw InternalException("Window expression type %s", ExpressionTypeToString(wexpr.GetExpressionType()));
		}
		return make_uniq<WindowExecutor>(wexpr, shared);
	}
}

WindowGlobalSinkState::WindowGlobalSinkState(const PhysicalWindow &op, ClientContext &client)
    : op(op), client(client), count(0) {
	D_ASSERT(op.select_list[op.order_idx]->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
	auto &wexpr = op.select_list[op.order_idx]->Cast<BoundWindowExpression>();

	for (idx_t expr_idx = 0; expr_idx < op.select_list.size(); ++expr_idx) {
		D_ASSERT(op.select_list[expr_idx]->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
		auto &wexpr = op.select_list[expr_idx]->Cast<BoundWindowExpression>();
		auto wexec = WindowExecutor::Factory(wexpr, client, shared);
		executors.emplace_back(std::move(wexec));
	}

	sort_strategy = SortStrategy::Factory(client, wexpr.Partitions(), wexpr.OrderBy(), op.children[0].get().GetTypes(),
	                                      wexpr.PartitionsStats(), op.partition_info, op.estimated_cardinality);
	strategy_sink = sort_strategy->GetGlobalSinkState(client);
}

OperatorPartitionInfo PhysicalWindow::RequiredPartitionInfo() const {
	if (!partition_info.RequiresPartitionColumns()) {
		return PhysicalOperator::RequiredPartitionInfo();
	}

	return partition_info;
}

//===--------------------------------------------------------------------===//
// NextBatch
//===--------------------------------------------------------------------===//
SinkNextBatchType PhysicalWindow::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &batch) const {
	//	Transfer the partition key (if any) to the strategy state
	auto &gstate = batch.global_state.Cast<WindowGlobalSinkState>();
	auto &lstate = batch.local_state.Cast<WindowLocalSinkState>();
	lstate.local_group->partition_info.batch_index = lstate.partition_info.batch_index;
	lstate.local_group->partition_info.partition_data = lstate.partition_info.partition_data;

	OperatorSinkNextBatchInput child_batch {*gstate.strategy_sink, *lstate.local_group, batch.interrupt_state};
	return gstate.sort_strategy->NextBatch(context, child_batch);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalWindow::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<WindowGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<WindowLocalSinkState>();
	gstate.count += chunk.size();

	OperatorSinkInput hsink {*gstate.strategy_sink, *lstate.local_group, sink.interrupt_state};
	return gstate.sort_strategy->Sink(context, chunk, hsink);
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
SinkCombineResultType PhysicalWindow::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<WindowGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<WindowLocalSinkState>();

	OperatorSinkCombineInput hcombine {*gstate.strategy_sink, *lstate.local_group, combine.interrupt_state};
	return gstate.sort_strategy->Combine(context, hcombine);
}

unique_ptr<LocalSinkState> PhysicalWindow::GetLocalSinkState(ExecutionContext &context) const {
	auto &gstate = sink_state->Cast<WindowGlobalSinkState>();
	return make_uniq<WindowLocalSinkState>(context, gstate);
}

unique_ptr<GlobalSinkState> PhysicalWindow::GetGlobalSinkState(ClientContext &client) const {
	return make_uniq<WindowGlobalSinkState>(*this, client);
}

SinkFinalizeType PhysicalWindow::Finalize(Pipeline &pipeline, Event &event, ClientContext &client,
                                          OperatorSinkFinalizeInput &input) const {
	auto &gsink = input.global_state.Cast<WindowGlobalSinkState>();
	auto &sort_strategy = *gsink.sort_strategy;
	lock_guard<mutex> sinks_guard(gsink.lock);

	OperatorSinkFinalizeInput hfinalize {*gsink.strategy_sink, input.interrupt_state};
	return sort_strategy.Finalize(client, hfinalize);
}

ProgressData PhysicalWindow::GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
                                             const ProgressData source_progress) const {
	auto &gsink = gstate.Cast<WindowGlobalSinkState>();
	auto progress = source_progress;
	lock_guard<mutex> sinks_guard(gsink.lock);
	progress.Add(gsink.sort_strategy->GetSinkProgress(context, *gsink.strategy_sink, progress));
	return gsink.sink_progress.Update(progress);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class WindowGlobalSourceState : public GlobalSourceState {
public:
	using GlobalStatePtr = unique_ptr<GlobalSourceState>;
	using WindowHashGroupPtr = unique_ptr<WindowHashGroup>;
	using ScannerPtr = unique_ptr<TupleDataChunkIterator>;
	using Task = WindowSourceTask;
	using TaskPtr = optional_ptr<Task>;
	using PartitionBlock = std::pair<idx_t, idx_t>;

	WindowGlobalSourceState(ClientContext &context_p, WindowGlobalSinkState &gsink_p);

	//! Are there any more tasks?
	bool HasMoreTasks() const {
		return !stopped && started < total_tasks;
	}
	bool HasUnfinishedTasks() const {
		return !stopped && finished < total_tasks;
	}
	//! Get the next task given the current state
	bool TryNextTask(TaskPtr &task, Task &task_local) DUCKDB_EXCLUDES(lock);
	//! Select a task with the lock held, after the previous task has been retired.
	bool TryNextTaskLocked(TaskPtr &task, Task &task_local) DUCKDB_REQUIRES(lock);

	//! Context for executing computations
	ClientContext &client;
	//! All the sunk data
	WindowGlobalSinkState &gsink;
	//! The hashed sort global source states for delayed sorting
	GlobalStatePtr hashed_source;
	//! The sorted hash groups
	vector<WindowHashGroupPtr> window_hash_groups;
	//! The total number of blocks to process;
	idx_t total_blocks = 0;
	//! The sorted list of (blocks, group_idx) pairs
	vector<PartitionBlock> partition_blocks;
	//! The ordered set of active groups
	vector<idx_t> active_groups;
	//! The number of started tasks
	atomic<idx_t> next_group;
	//! The number of local states
	atomic<idx_t> locals;
	//! The total number of tasks
	idx_t total_tasks = 0;
	//! The number of started tasks
	atomic<idx_t> started;
	//! The number of finished tasks
	atomic<idx_t> finished;
	//! Stop producing tasks
	atomic<bool> stopped;
	//! The number of tasks completed. This will combine both build and evaluate.
	atomic<idx_t> completed;

public:
	idx_t MaxThreads() override {
		return total_blocks;
	}

protected:
	//! Build task list
	void CreateTaskList();
	//! Finish a task and return any completed group for destruction outside the source lock.
	WindowHashGroupPtr FinishTask(TaskPtr task);
};

WindowGlobalSourceState::WindowGlobalSourceState(ClientContext &client, WindowGlobalSinkState &gsink_p)
    : client(client), gsink(gsink_p), next_group(0), locals(0), started(0), finished(0), stopped(false), completed(0) {
	auto &sort_strategy = *gsink.sort_strategy;

	hashed_source = sort_strategy.GetGlobalSourceState(client, *gsink.strategy_sink);
	auto &hash_groups = sort_strategy.GetHashGroups(*hashed_source);

	for (idx_t bin_idx = 0; bin_idx < hash_groups.size(); ++bin_idx) {
		const auto block_count = hash_groups[bin_idx].chunks;
		if (!block_count) {
			continue;
		}

		const idx_t group_idx = window_hash_groups.size();
		auto window_hash_group = make_uniq<WindowHashGroup>(gsink, hash_groups[bin_idx], group_idx, bin_idx);
		window_hash_group->batch_base = total_blocks;
		total_blocks += block_count;

		window_hash_groups.emplace_back(std::move(window_hash_group));
	}

	CreateTaskList();
}

void WindowGlobalSourceState::CreateTaskList() {
	//    Sort the groups from largest to smallest
	if (window_hash_groups.empty()) {
		return;
	}

	for (idx_t group_idx = 0; group_idx < window_hash_groups.size(); ++group_idx) {
		auto &window_hash_group = window_hash_groups[group_idx];
		if (!window_hash_group) {
			continue;
		}
		partition_blocks.emplace_back(window_hash_group->blocks, group_idx);
	}
	std::sort(partition_blocks.begin(), partition_blocks.end(), std::greater<PartitionBlock>());

	//	Schedule the largest group on as many threads as possible
	auto &ts = TaskScheduler::GetScheduler(client);
	const auto threads = ts.NumberOfThreads();

	const auto &max_block = partition_blocks.front();

	// To compute masks in parallel, we need to have the row count of the number of chunks per thread
	// be a multiple of the mask entry size.  Usually, this is not a problem because
	// STANDARD_VECTOR_SIZE >> ValidityMask::BITS_PER_VALUE, but if STANDARD_VECTOR_SIZE is say 2,
	// we need to align the chunk count to the mask width.
	const auto aligned_scale = MaxValue<idx_t>(ValidityMask::BITS_PER_VALUE / STANDARD_VECTOR_SIZE, 1);
	const auto aligned_count = WindowHashGroup::BinValue(max_block.first, aligned_scale);
	const auto per_thread = aligned_scale * WindowHashGroup::BinValue(aligned_count, threads);
	if (!per_thread) {
		throw InternalException("No blocks per thread! %ld threads, %ld groups, %ld blocks, %ld hash group", threads,
		                        partition_blocks.size(), max_block.first, max_block.second);
	}

	for (const auto &b : partition_blocks) {
		total_tasks += window_hash_groups[b.second]->InitTasks(per_thread);
	}
}

WindowHashGroup::WindowHashGroup(WindowGlobalSinkState &gsink, const ChunkRow &chunk_row, idx_t group_idx,
                                 idx_t bin_idx)
    : gsink(gsink), count(chunk_row.count), blocks(chunk_row.chunks), stage(WindowGroupStage::SORT),
      group_idx(group_idx), bin_idx(bin_idx), sorted(0), materialized(0), masked(0), sunk(0), finalized(0),
      completed(0), batch_base(0) {
	// There are three types of partitions:
	// 1. No partition (no sorting)
	// 2. One partition (sorting, but no hashing)
	// 3. Multiple partitions (sorting and hashing)

	// Set up the collection for any fully materialised data
	const auto &shared = WindowSharedExpressions::GetSortedExpressions(gsink.shared.coll_shared);
	vector<LogicalType> types;
	for (auto &expr : shared) {
		types.emplace_back(expr->GetReturnType());
	}
	auto &buffer_manager = BufferManager::GetBufferManager(gsink.client);
	collection = make_uniq<WindowCollection>(buffer_manager, count, types);
}

unique_ptr<WindowCollectionChunkScanner> WindowHashGroup::GetScanner(const idx_t begin_idx) const {
	if (!rows) {
		return nullptr;
	}

	auto &scan_ids = gsink.sort_strategy->scan_ids;
	return make_uniq<WindowCollectionChunkScanner>(*rows, scan_ids, begin_idx);
}

void WindowHashGroup::UpdateScanner(ScannerPtr &scanner, idx_t begin_idx) const {
	if (!scanner || &scanner->collection != rows.get()) {
		scanner.reset();
		scanner = GetScanner(begin_idx);
	} else {
		scanner->Seek(begin_idx);
	}
}

//! Backs a mask of `count` rows (all valid) with a block from the allocator, so that it counts against the memory limit
static void AllocateMask(Allocator &allocator, idx_t count, ValidityMask &mask, AllocatedData &storage) {
	storage = allocator.Allocate(ValidityMask::EntryCount(count) * sizeof(validity_t));
	memset(storage.get(), 0xFF, storage.GetSize());
	mask.Initialize(reinterpret_cast<validity_t *>(storage.get()), count);
}

void WindowHashGroup::AllocateMasks() {
	CallOnce(masks_initialized, [&] {
		auto &allocator = BufferAllocator::Get(gsink.client);
		ValidityMask new_partition_mask;
		AllocatedData new_partition_mask_data;
		AllocateMask(allocator, count, new_partition_mask, new_partition_mask_data);
		OrderMasks new_order_masks;
		unordered_map<idx_t, AllocatedData> new_order_mask_data;

		// All masks refer to the shared sort's partition columns, including duplicate expressions.
		const auto &sort_expr = gsink.op.select_list[gsink.op.order_idx]->Cast<BoundWindowExpression>();
		const auto order_begin = gsink.op.partition_info.RequiresPartitionColumns() ? 0 : sort_expr.Partitions().size();
		for (auto &wexec : gsink.executors) {
			const auto order_idx = order_begin + wexec->wexpr.OrderBy().size();
			auto &order_mask = new_order_masks[order_idx];
			if (!order_mask.IsMaskSet()) {
				AllocateMask(allocator, count, order_mask, new_order_mask_data[order_idx]);
			}
		}
		partition_mask = std::move(new_partition_mask);
		partition_mask_data = std::move(new_partition_mask_data);
		order_masks = std::move(new_order_masks);
		order_mask_data = std::move(new_order_mask_data);
	});
}

void WindowHashGroup::ComputeMasks(const idx_t block_begin, const idx_t block_end) {
	D_ASSERT(count > 0);

	//	Initialise our range
	AllocateMasks();
	const auto begin_entry = partition_mask.EntryCount(block_begin * STANDARD_VECTOR_SIZE);
	const auto end_entry = partition_mask.EntryCount(MinValue<idx_t>(block_end * STANDARD_VECTOR_SIZE, count));

	//	If the data is unsorted, then the chunk sizes may be < STANDARD_VECTOR_SIZE,
	//	and the entry range may be empty.
	if (begin_entry >= end_entry) {
		return;
	}

	partition_mask.SetRangeInvalid(count, begin_entry, end_entry);
	if (!block_begin) {
		partition_mask.SetValidUnsafe(0);
	}
	for (auto &order_mask : order_masks) {
		order_mask.second.SetRangeInvalid(count, begin_entry, end_entry);
		if (!block_begin) {
			order_mask.second.SetValidUnsafe(0);
		}
	}

	//	If we are not sorting, then only the partition boundaries are needed.
	const auto &wexpr = gsink.op.select_list[gsink.op.order_idx]->Cast<BoundWindowExpression>();

	//	Set up the partition compare structs
	const auto order_begin = gsink.op.partition_info.RequiresPartitionColumns() ? 0 : wexpr.Partitions().size();
	if (!order_begin && wexpr.OrderBy().empty()) {
		return;
	}

	//	Set up the order data structures
	auto &collection = *rows;
	auto &scan_cols = gsink.sort_strategy->sort_ids;
	WindowCollectionChunkScanner scanner(collection, scan_cols, block_begin);
	unordered_map<idx_t, DataChunk> prefixes;
	for (auto &order_mask : order_masks) {
		D_ASSERT(order_mask.first >= order_begin);
		auto order_type = scanner.PrefixStructType(order_mask.first, order_begin);
		vector<LogicalType> types(2, order_type);
		auto &keys = prefixes[order_mask.first];
		// We can't use InitializeEmpty here because it doesn't set up all of the STRUCT internals...
		keys.Initialize(collection.GetAllocator(), types);
	}

	WindowDeltaScanner(collection, block_begin, block_end, scan_cols, order_begin,
	                   [&](const idx_t row_idx, DataChunk &prev, DataChunk &curr, const idx_t ndistinct,
	                       SelectionVector &distinct, const SelectionVector &matching) {
		                   //	Process the partition boundaries
		                   for (idx_t i = 0; i < ndistinct; ++i) {
			                   const idx_t curr_index = row_idx + distinct.get_index(i);
			                   partition_mask.SetValidUnsafe(curr_index);
			                   for (auto &order_mask : order_masks) {
				                   order_mask.second.SetValidUnsafe(curr_index);
			                   }
		                   }

		                   //	Process the peers with each partition
		                   const auto count = MinValue<idx_t>(prev.size(), curr.size());
		                   const auto nmatch = count - ndistinct;
		                   if (!nmatch) {
			                   return;
		                   }

		                   for (auto &order_mask : order_masks) {
			                   // If there are no order columns, then all the partition elements are peers and we are
			                   // done
			                   if (order_begin == order_mask.first) {
				                   continue;
			                   }
			                   auto &prefix = prefixes[order_mask.first];
			                   prefix.Reset();
			                   auto &order_prev = prefix.data[0];
			                   auto &order_curr = prefix.data[1];
			                   scanner.ReferenceStructColumns(prev, order_prev, order_mask.first, order_begin);
			                   scanner.ReferenceStructColumns(curr, order_curr, order_mask.first, order_begin);
			                   if (ndistinct) {
				                   prefix.Slice(matching, nmatch);
			                   } else {
				                   prefix.SetChildCardinality(nmatch);
			                   }
			                   const auto m = VectorOperations::DistinctFrom(order_curr, order_prev, nullptr, nmatch,
			                                                                 &distinct, nullptr);
			                   for (idx_t i = 0; i < m; ++i) {
				                   const idx_t curr_index = row_idx + matching.get_index(distinct.get_index(i));
				                   order_mask.second.SetValidUnsafe(curr_index);
			                   }
		                   }
	                   });
}

// Per-thread scan state
class WindowLocalSourceState : public LocalSourceState {
public:
	using Task = WindowGlobalSourceState::Task;
	using TaskPtr = optional_ptr<Task>;

	explicit WindowLocalSourceState(WindowGlobalSourceState &gsource);

	void ReleaseLocalStates() {
		auto &local_states = window_hash_group->thread_states.at(task->thread_idx);
		local_states.clear();
	}

	//! Does the task have more work to do?
	bool TaskFinished() const {
		return !task || task->begin_idx == task->end_idx;
	}
	//! Assign the next task
	bool TryAssignTask();
	//! Execute a step in the current task
	void ExecuteTask(ExecutionContext &context, DataChunk &chunk, InterruptState &interrupt);

	//! The shared source state
	WindowGlobalSourceState &gsource;
	//! The current batch index (for output reordering)
	idx_t batch_index;
	//! The task this thread is working on
	TaskPtr task;
	//! The task storage
	Task task_local;
	//! The current source being processed
	optional_ptr<WindowHashGroup> window_hash_group;
	//! The scan cursor
	unique_ptr<WindowCollectionChunkScanner> scanner;
	//! Buffer for window results
	DataChunk output_chunk;

protected:
	//! Sort the partition
	void Sort(ExecutionContext &context, InterruptState &interrupt);
	//! Materialize the sorted run
	void Materialize(ExecutionContext &context, InterruptState &interrupt);
	//! Compute a mask range
	void Mask(ExecutionContext &context, InterruptState &interrupt);
	//! Sink tuples into function global states
	void Sink(ExecutionContext &context, InterruptState &interrupt);
	//! Post process function global state construction
	void Finalize(ExecutionContext &context, InterruptState &interrupt);
	//! Get a chunk by evaluating functions
	void GetData(ExecutionContext &context, DataChunk &chunk, InterruptState &interrupt);

	//! Storage and evaluation for the fully materialised data
	unique_ptr<WindowBuilder> builder;
	ExpressionExecutor coll_exec;
	DataChunk coll_chunk;

	//! Storage and evaluation for chunks used in the sink/build phase
	ExpressionExecutor sink_exec;
	DataChunk sink_chunk;

	//! Storage and evaluation for chunks used in the evaluate phase
	ExpressionExecutor eval_exec;
	DataChunk eval_chunk;
};

idx_t WindowHashGroup::InitTasks(idx_t per_thread_p) {
	per_thread = per_thread_p;
	group_threads = BinValue(ChunkCount(), per_thread);
	thread_states.resize(GetThreadCount());

	return GetTaskCount();
}

void WindowLocalSourceState::Sort(ExecutionContext &context, InterruptState &interrupt) {
	D_ASSERT(task);
	D_ASSERT(task->stage == WindowGroupStage::SORT);

	auto &gsink = gsource.gsink;
	auto &sort_strategy = *gsink.sort_strategy;
	OperatorSinkFinalizeInput finalize {*gsink.strategy_sink, interrupt};
	sort_strategy.SortColumnData(context, window_hash_group->bin_idx, finalize);

	//	Mark this range as done
	window_hash_group->sorted += (task->end_idx - task->begin_idx);
	task->begin_idx = task->end_idx;
}

void WindowLocalSourceState::Materialize(ExecutionContext &context, InterruptState &interrupt) {
	D_ASSERT(task);
	D_ASSERT(task->stage == WindowGroupStage::MATERIALIZE);

	auto unused = make_uniq<LocalSourceState>();
	auto &hashed_source = gsource.hashed_source;
	OperatorSourceInput source {*hashed_source, *unused, interrupt};
	auto &gsink = gsource.gsink;
	auto &sort_strategy = *gsink.sort_strategy;
	sort_strategy.MaterializeColumnData(context, window_hash_group->bin_idx, source);

	//	Mark this range as done
	window_hash_group->materialized += (task->end_idx - task->begin_idx);
	task->begin_idx = task->end_idx;

	// 	There is no good place to read the column data,
	//	and if we do it twice we can split the results.
	if (window_hash_group->materialized >= window_hash_group->blocks) {
		lock_guard<mutex> prepare_guard(window_hash_group->lock);
		if (!window_hash_group->rows) {
			window_hash_group->rows = sort_strategy.GetColumnData(window_hash_group->bin_idx, source);
		}
	}
}

void WindowLocalSourceState::Mask(ExecutionContext &context, InterruptState &interrupt) {
	D_ASSERT(task);
	D_ASSERT(task->stage == WindowGroupStage::MASK);

	window_hash_group->ComputeMasks(task->begin_idx, task->end_idx);

	//	Mark this range as done
	window_hash_group->masked += (task->end_idx - task->begin_idx);
	task->begin_idx = task->end_idx;
}

WindowHashGroup::ExecutorGlobalStates &WindowHashGroup::GetGlobalStates(ClientContext &client) {
	CallOnce(gestates_initialized, [&] {
		// These can be large so we defer building them until we are ready.
		ExecutorGlobalStates new_gestates;
		const auto &sort_expr = gsink.op.select_list[gsink.op.order_idx]->Cast<BoundWindowExpression>();
		const auto order_begin = gsink.op.partition_info.RequiresPartitionColumns() ? 0 : sort_expr.Partitions().size();
		for (auto &wexec : gsink.executors) {
			auto &order_mask = order_masks.at(order_begin + wexec->wexpr.OrderBy().size());
			new_gestates.emplace_back(wexec->GetGlobalState(client, count, partition_mask, order_mask));
		}
		gestates = std::move(new_gestates);
	});
	return gestates;
}

void WindowLocalSourceState::Sink(ExecutionContext &context, InterruptState &interrupt) {
	D_ASSERT(task);
	D_ASSERT(task->stage == WindowGroupStage::SINK);

	auto &gsink = gsource.gsink;
	const auto &executors = gsink.executors;

	// Create the global state for each function
	// These can be large so we defer building them until we are ready.
	auto &gestates = window_hash_group->GetGlobalStates(context.client);

	//	Set up the local states
	auto &local_states = window_hash_group->thread_states.at(task->thread_idx);
	if (local_states.empty()) {
		for (idx_t w = 0; w < executors.size(); ++w) {
			local_states.emplace_back(executors[w]->GetLocalState(context, *gestates[w]));
		}
	}

	//	The block range owned by this task. Counted whole so that the SINK -> FINALIZE
	//	transition waits for every SINK task to run, even ones whose blocks contain no rows.
	const idx_t task_blocks = task->end_idx - task->begin_idx;

	//	First pass over the input without flushing
	scanner = window_hash_group->GetScanner(task->begin_idx);
	if (!scanner) {
		window_hash_group->sunk += task_blocks;
		return;
	}
	for (; task->begin_idx < task->end_idx; ++task->begin_idx) {
		const idx_t input_idx = scanner->Scanned();
		if (!scanner->Scan()) {
			break;
		}
		auto &input_chunk = scanner->chunk;

		//	Compute fully materialised expressions
		if (coll_chunk.data.empty()) {
			coll_chunk.SetChildCardinality(input_chunk.size());
		} else {
			coll_chunk.Reset();
			coll_exec.Execute(input_chunk, coll_chunk);
			auto collection = window_hash_group->collection.get();
			if (!builder || &builder->collection != collection) {
				builder = make_uniq<WindowBuilder>(*collection);
			}

			builder->Sink(coll_chunk, input_idx);
		}

		// Compute sink expressions
		if (sink_chunk.data.empty()) {
			sink_chunk.SetChildCardinality(input_chunk.size());
		} else {
			sink_chunk.Reset();
			sink_exec.Execute(input_chunk, sink_chunk);
		}

		for (idx_t w = 0; w < executors.size(); ++w) {
			OperatorSinkInput sink {*gestates[w], *local_states[w], interrupt};
			executors[w]->Sink(context, sink_chunk, coll_chunk, input_idx, sink);
		}
	}
	scanner.reset();
	window_hash_group->sunk += task_blocks;
}

void WindowLocalSourceState::Finalize(ExecutionContext &context, InterruptState &interrupt) {
	D_ASSERT(task);
	D_ASSERT(task->stage == WindowGroupStage::FINALIZE);

	// First finalize the collection (so the executors can use it)
	auto &gsink = gsource.gsink;
	if (window_hash_group->collection) {
		window_hash_group->collection->Combine(gsink.shared.coll_validity);
	}

	// Finalize all the executors.
	// Parallel finalisation is handled internally by the executor,
	// and should not return until all threads have completed work.
	const auto &executors = gsink.executors;
	auto &gestates = window_hash_group->gestates;
	auto &local_states = window_hash_group->thread_states.at(task->thread_idx);
	for (idx_t w = 0; w < executors.size(); ++w) {
		OperatorSinkInput sink {*gestates[w], *local_states[w], interrupt};
		executors[w]->Finalize(context, window_hash_group->collection, sink);
	}

	//	Mark this range as done
	window_hash_group->finalized += (task->end_idx - task->begin_idx);
	task->begin_idx = task->end_idx;
}

WindowLocalSourceState::WindowLocalSourceState(WindowGlobalSourceState &gsource)
    : gsource(gsource), batch_index(0), coll_exec(gsource.client), sink_exec(gsource.client),
      eval_exec(gsource.client) {
	auto &gsink = gsource.gsink;

	vector<LogicalType> output_types;
	for (auto &wexec : gsink.executors) {
		auto &wexpr = wexec->wexpr;
		output_types.emplace_back(wexpr.GetReturnType());
	}
	output_chunk.Initialize(gsource.client, output_types);

	auto &shared = gsink.shared;
	shared.PrepareCollection(coll_exec, coll_chunk);
	shared.PrepareSink(sink_exec, sink_chunk);
	shared.PrepareEvaluate(eval_exec, eval_chunk);

	++gsource.locals;
}

bool WindowGlobalSourceState::TryNextTask(TaskPtr &task, Task &task_local) {
	// Destroy completed groups after releasing the source lock.
	WindowHashGroupPtr finished_group;
	annotated_lock_guard<annotated_mutex> guard(lock);
	finished_group = FinishTask(task);
	return TryNextTaskLocked(task, task_local);
}

bool WindowGlobalSourceState::TryNextTaskLocked(TaskPtr &task, Task &task_local) {
	if (!HasMoreTasks()) {
		task = nullptr;
		return false;
	}

	//	Run through the active groups looking for one that can assign a task
	for (const auto &group_idx : active_groups) {
		auto &window_hash_group = window_hash_groups[group_idx];
		if (window_hash_group->TryPrepareNextStage()) {
			UnblockTasks();
		}
		if (window_hash_group->TryNextTask(task_local)) {
			task = task_local;
			++started;
			return true;
		}
	}

	//	All active groups are busy or blocked, so start the next one (if any)
	while (next_group < partition_blocks.size()) {
		const auto group_idx = partition_blocks[next_group++].second;
		active_groups.emplace_back(group_idx);

		auto &window_hash_group = window_hash_groups[group_idx];
		if (window_hash_group->TryPrepareNextStage()) {
			UnblockTasks();
		}
		if (!window_hash_group->TryNextTask(task_local)) {
			//	Group has no tasks (empty?)
			continue;
		}

		task = task_local;
		++started;
		return true;
	}

	task = nullptr;

	return false;
}

WindowGlobalSourceState::WindowHashGroupPtr WindowGlobalSourceState::FinishTask(TaskPtr task) {
	if (!task) {
		return nullptr;
	}

	const auto group_idx = task->group_idx;
	auto &finished_hash_group = window_hash_groups[group_idx];
	D_ASSERT(finished_hash_group);

	WindowHashGroupPtr finished_group;
	if (++finished_hash_group->completed >= finished_hash_group->GetTaskCount()) {
		finished_group = std::move(finished_hash_group);
		//	Remove it from the active groups
		auto &v = active_groups;
		v.erase(std::remove(v.begin(), v.end(), group_idx), v.end());
	}

	//	Count the global tasks completed.
	++completed;
	return finished_group;
}

bool WindowLocalSourceState::TryAssignTask() {
	D_ASSERT(TaskFinished());
	// Because downstream operators may be using our internal buffers,
	// we can't "finish" a task until we are about to get the next one.

	// Scanner first, as it may be referencing sort blocks in the hash group
	scanner.reset();

	return gsource.TryNextTask(task, task_local);
}

void WindowLocalSourceState::ExecuteTask(ExecutionContext &context, DataChunk &result, InterruptState &interrupt) {
	// Update the hash group
	window_hash_group = gsource.window_hash_groups[task->group_idx].get();

	// Process the new state
	switch (task->stage) {
	case WindowGroupStage::SORT:
		Sort(context, interrupt);
		D_ASSERT(TaskFinished());
		break;
	case WindowGroupStage::MATERIALIZE:
		Materialize(context, interrupt);
		D_ASSERT(TaskFinished());
		break;
	case WindowGroupStage::MASK:
		Mask(context, interrupt);
		D_ASSERT(TaskFinished());
		break;
	case WindowGroupStage::SINK:
		Sink(context, interrupt);
		D_ASSERT(TaskFinished());
		break;
	case WindowGroupStage::FINALIZE:
		Finalize(context, interrupt);
		D_ASSERT(TaskFinished());
		break;
	case WindowGroupStage::GETDATA:
		D_ASSERT(!TaskFinished());
		GetData(context, result, interrupt);
		break;
	case WindowGroupStage::DONE:
		throw InternalException("Invalid window source state.");
	}

	// Count this task as finished.
	if (TaskFinished()) {
		++gsource.finished;
	}
}

void WindowLocalSourceState::GetData(ExecutionContext &context, DataChunk &result, InterruptState &interrupt) {
	D_ASSERT(window_hash_group->GetStage() == WindowGroupStage::GETDATA);

	window_hash_group->UpdateScanner(scanner, task->begin_idx);
	batch_index = window_hash_group->batch_base + task->begin_idx;

	const auto position = scanner->Scanned();
	auto &input_chunk = scanner->chunk;
	scanner->Scan();

	const auto &executors = gsource.gsink.executors;
	auto &gestates = window_hash_group->gestates;
	auto &local_states = window_hash_group->thread_states.at(task->thread_idx);
	output_chunk.Reset();
	for (idx_t expr_idx = 0; expr_idx < executors.size(); ++expr_idx) {
		auto &executor = *executors[expr_idx];
		auto &result = output_chunk.data[expr_idx];
		if (eval_chunk.data.empty()) {
			eval_chunk.SetChildCardinality(input_chunk.size());
		} else {
			eval_chunk.Reset();
			eval_exec.Execute(input_chunk, eval_chunk);
		}
		OperatorSinkInput sink {*gestates[expr_idx], *local_states[expr_idx], interrupt};
		executor.Evaluate(context, position, eval_chunk, result, sink, input_chunk.size());
	}
	output_chunk.Verify(context.client.db);

	idx_t out_idx = 0;
	// input_chunk's cardinality is authoritative (its column buffers may still be a larger, stale capacity), so set the
	// result cardinality explicitly here - before the references below alias input_chunk's/output_chunk's buffers.
	result.SetChildCardinality(input_chunk.size());
	for (idx_t col_idx = 0; col_idx < input_chunk.ColumnCount(); col_idx++) {
		result.data[out_idx++].Reference(input_chunk.data[col_idx]);
	}
	for (idx_t col_idx = 0; col_idx < output_chunk.ColumnCount(); col_idx++) {
		result.data[out_idx++].Reference(output_chunk.data[col_idx]);
	}

	// Move to the next chunk
	++task->begin_idx;

	result.Verify(context.client.db);
}

unique_ptr<LocalSourceState> PhysicalWindow::GetLocalSourceState(ExecutionContext &context,
                                                                 GlobalSourceState &gsource_p) const {
	auto &gsource = gsource_p.Cast<WindowGlobalSourceState>();
	return make_uniq<WindowLocalSourceState>(gsource);
}

unique_ptr<GlobalSourceState> PhysicalWindow::GetGlobalSourceState(ClientContext &client) const {
	auto &gsink = sink_state->Cast<WindowGlobalSinkState>();
	return make_uniq<WindowGlobalSourceState>(client, gsink);
}

bool PhysicalWindow::SupportsPartitioning(const OperatorPartitionInfo &partition_info) const {
	if (partition_info.RequiresPartitionColumns()) {
		return false;
	}
	//	We can only preserve order for single partitioning
	//	or work stealing causes out of order batch numbers
	auto &wexpr = select_list[order_idx]->Cast<BoundWindowExpression>();
	return wexpr.Partitions().empty(); // NOLINT
}

OrderPreservationType PhysicalWindow::SourceOrder() const {
	auto &wexpr = select_list[order_idx]->Cast<BoundWindowExpression>();
	if (!wexpr.Partitions().empty()) {
		// if we have partitions the window order is not defined
		return OrderPreservationType::NO_ORDER;
	}
	// without partitions we can maintain order
	if (wexpr.OrderBy().empty()) {
		// if we have no orders we maintain insertion order
		return OrderPreservationType::INSERTION_ORDER;
	}
	// otherwise we can maintain the fixed order
	return OrderPreservationType::FIXED_ORDER;
}

ProgressData PhysicalWindow::GetProgress(ClientContext &client, GlobalSourceState &gsource_p) const {
	auto &gsource = gsource_p.Cast<WindowGlobalSourceState>();
	auto &gsink = gsource.gsink;
	const auto count = gsink.count.load();
	const auto finished = gsource.finished.load();

	ProgressData res;
	if (count) {
		res.done = double(finished);
		res.total = double(gsource.total_tasks);
		//	Convert to tuples.
		res.Normalize(double(count));
	} else {
		res.SetInvalid();
	}

	return res;
}

OperatorPartitionData PhysicalWindow::GetPartitionData(ExecutionContext &context, DataChunk &chunk,
                                                       GlobalSourceState &gstate_p, LocalSourceState &lstate_p,
                                                       const OperatorPartitionInfo &partition_info) const {
	if (partition_info.RequiresPartitionColumns()) {
		throw InternalException("PhysicalWindow::GetPartitionData: partition columns not supported");
	}
	auto &lstate = lstate_p.Cast<WindowLocalSourceState>();
	return OperatorPartitionData(lstate.batch_index);
}

SourceResultType PhysicalWindow::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<WindowGlobalSourceState>();
	auto &lsource = source.local_state.Cast<WindowLocalSourceState>();

	while (gsource.HasUnfinishedTasks() && chunk.size() == 0) {
		if (!lsource.TaskFinished() || lsource.TryAssignTask()) {
			try {
				lsource.ExecuteTask(context, chunk, source.interrupt_state);
			} catch (...) {
				gsource.stopped = true;
				throw;
			}
		} else {
			annotated_lock_guard<annotated_mutex> guard(gsource.lock);
			if (!gsource.HasMoreTasks()) {
				// no more tasks - exit
				gsource.UnblockTasks();
				break;
			}
			// TryAssignTask released the lock: another thread may have prepared the next stage and unblocked the
			// blocked tasks in between, so look again before blocking, or no one would ever unblock this task.
			if (gsource.TryNextTaskLocked(lsource.task, lsource.task_local)) {
				continue;
			}
			// there are more tasks available, but we can't execute them yet
			// block the source
			return gsource.BlockSource(source.interrupt_state);
		}
	}

	if (chunk.size() == 0) {
		return SourceResultType::FINISHED;
	}
	return SourceResultType::HAVE_MORE_OUTPUT;
}

InsertionOrderPreservingMap<string> PhysicalWindow::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string projections;
	for (idx_t i = 0; i < select_list.size(); i++) {
		if (i > 0) {
			projections += "\n";
		}
		projections += select_list[i]->GetName();
	}
	result["Projections"] = projections;
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
