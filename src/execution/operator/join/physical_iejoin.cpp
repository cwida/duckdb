#include "duckdb/execution/ie_join_union.hpp"
#include "duckdb/execution/operator/join/physical_iejoin.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include <utility>
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

PhysicalIEJoin::PhysicalIEJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op, PhysicalOperator &left,
                               PhysicalOperator &right, vector<JoinCondition> cond, JoinType join_type,
                               idx_t estimated_cardinality, unique_ptr<JoinFilterPushdownInfo> pushdown_info)
    : PhysicalRangeJoin(physical_plan, op, PhysicalOperatorType::IE_JOIN, left, right, std::move(cond), join_type,
                        estimated_cardinality, std::move(pushdown_info)) {
	// 1. let L1 (resp. L2) be the array of column X (resp. Y)
	D_ASSERT(conditions.size() >= 2);
	for (idx_t i = 0; i < 2; ++i) {
		auto &cond = conditions[i];
		D_ASSERT(cond.GetLHS().GetReturnType() == cond.GetRHS().GetReturnType());
		join_key_types.push_back(cond.GetLHS().GetReturnType());

		// Convert the conditions to sort orders
		auto left = cond.GetLHS().Copy();
		auto right = cond.GetRHS().Copy();
		auto sense = OrderType::INVALID;

		// 2. if (op1 ∈ {>, ≥}) sort L1 in descending order
		// 3. else if (op1 ∈ {<, ≤}) sort L1 in ascending order
		// 4. if (op2 ∈ {>, ≥}) sort L2 in ascending order
		// 5. else if (op2 ∈ {<, ≤}) sort L2 in descending order
		switch (cond.GetComparisonType()) {
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			sense = i ? OrderType::ASCENDING : OrderType::DESCENDING;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			sense = i ? OrderType::DESCENDING : OrderType::ASCENDING;
			break;
		default:
			throw NotImplementedException("Unimplemented join type for IEJoin");
		}
		lhs_orders.emplace_back(sense, OrderByNullType::NULLS_LAST, std::move(left));
		rhs_orders.emplace_back(sense, OrderByNullType::NULLS_LAST, std::move(right));
	}

	for (idx_t i = 2; i < conditions.size(); ++i) {
		auto &cond = conditions[i];
		D_ASSERT(cond.GetLHS().GetReturnType() == cond.GetRHS().GetReturnType());
		join_key_types.push_back(cond.GetLHS().GetReturnType());
	}
}

PhysicalIEJoin::PhysicalIEJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op, PhysicalOperator &left,
                               PhysicalOperator &right, vector<JoinCondition> cond, JoinType join_type,
                               idx_t estimated_cardinality)
    : PhysicalIEJoin(physical_plan, op, left, right, std::move(cond), join_type, estimated_cardinality, nullptr) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class IEJoinLocalState;

class IEJoinGlobalState : public GlobalSinkState {
public:
	using GlobalSortedTable = PhysicalRangeJoin::GlobalSortedTable;

public:
	IEJoinGlobalState(ClientContext &context, const PhysicalIEJoin &op) : child(1) {
		tables.resize(2);
		const auto &lhs_types = op.children[0].get().GetTypes();
		vector<BoundOrderByNode> lhs_order;
		lhs_order.emplace_back(op.lhs_orders[0].Copy());
		tables[0] = make_uniq<GlobalSortedTable>(context, lhs_order, lhs_types, op);

		const auto &rhs_types = op.children[1].get().GetTypes();
		vector<BoundOrderByNode> rhs_order;
		rhs_order.emplace_back(op.rhs_orders[0].Copy());
		tables[1] = make_uniq<GlobalSortedTable>(context, rhs_order, rhs_types, op);

		if (op.filter_pushdown) {
			skip_filter_pushdown = op.filter_pushdown->probe_info.empty();
			global_filter_state = op.filter_pushdown->GetGlobalState(context, op);
		}
	}

	void Sink(ExecutionContext &context, DataChunk &input, IEJoinLocalState &lstate);

	void Finalize(ClientContext &client, InterruptState &interrupt) {
		// Sort the current input child
		D_ASSERT(child < tables.size());
		tables[child]->Finalize(client, interrupt);
	};

	void Materialize(Pipeline &pipeline, Event &event) {
		// Sort the current input child
		D_ASSERT(child < tables.size());
		tables[child]->Materialize(pipeline, event);
		child = child ? 0 : 2;
		skip_filter_pushdown = true;
	};

	//! The two input tables (IEJoin materialises both sides)
	vector<unique_ptr<GlobalSortedTable>> tables;
	//! The child that is being materialised (right/1 then left/0)
	size_t child;
	//! Should we not bother pushing down filters?
	bool skip_filter_pushdown = false;
	//! The global filter states to push down (if any)
	unique_ptr<JoinFilterGlobalState> global_filter_state;
};

class IEJoinLocalState : public LocalSinkState {
public:
	using LocalSortedTable = PhysicalRangeJoin::LocalSortedTable;

	IEJoinLocalState(ExecutionContext &context, const PhysicalRangeJoin &op, IEJoinGlobalState &gstate)
	    : table(context, *gstate.tables[gstate.child], gstate.child) {
		if (op.filter_pushdown) {
			local_filter_state = op.filter_pushdown->GetLocalState(*gstate.global_filter_state);
		}
	}

	//! The local sort state
	LocalSortedTable table;
	//! Local state for accumulating filter statistics
	unique_ptr<JoinFilterLocalState> local_filter_state;
};

unique_ptr<GlobalSinkState> PhysicalIEJoin::GetGlobalSinkState(ClientContext &context) const {
	D_ASSERT(!sink_state);
	return make_uniq<IEJoinGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalIEJoin::GetLocalSinkState(ExecutionContext &context) const {
	auto &ie_sink = sink_state->Cast<IEJoinGlobalState>();
	return make_uniq<IEJoinLocalState>(context, *this, ie_sink);
}

void IEJoinGlobalState::Sink(ExecutionContext &context, DataChunk &input, IEJoinLocalState &lstate) {
	// Sink the data into the local sort state
	lstate.table.Sink(context, input);
}

SinkResultType PhysicalIEJoin::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<IEJoinGlobalState>();
	auto &lstate = input.local_state.Cast<IEJoinLocalState>();

	if (gstate.child == 0 && gstate.tables[1]->Count() == 0 && EmptyResultIfRHSIsEmpty()) {
		return SinkResultType::FINISHED;
	}

	gstate.Sink(context, chunk, lstate);

	if (filter_pushdown && !gstate.skip_filter_pushdown && gstate.child == 1) {
		filter_pushdown->Sink(lstate.table.keys, *lstate.local_filter_state);
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalIEJoin::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<IEJoinGlobalState>();
	auto &lstate = input.local_state.Cast<IEJoinLocalState>();
	gstate.tables[gstate.child]->Combine(context, lstate.table);
	auto &client_profiler = QueryProfiler::Get(context.client);

	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);

	if (filter_pushdown && !gstate.skip_filter_pushdown && gstate.child == 1) {
		filter_pushdown->Combine(*gstate.global_filter_state, *lstate.local_filter_state);
	}

	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType PhysicalIEJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &client,
                                          OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<IEJoinGlobalState>();
	if (filter_pushdown && !gstate.skip_filter_pushdown && gstate.child == 1) {
		(void)filter_pushdown->Finalize(client, *gstate.global_filter_state, *this);
	}
	auto &table = *gstate.tables[gstate.child];

	if ((gstate.child == 1 && PropagatesBuildSide(join_type)) || (gstate.child == 0 && IsLeftOuterJoin(join_type))) {
		// for FULL/LEFT/RIGHT OUTER JOIN, initialize found_match to false for every tuple
		table.InitializeMatches();
	}

	SinkFinalizeType res;
	if (gstate.child == 1 && table.Count() == 0 && EmptyResultIfRHSIsEmpty()) {
		// Empty input!
		res = SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	} else {
		res = SinkFinalizeType::READY;
	}

	// Clean up the current table
	gstate.Finalize(client, input.interrupt_state);

	// Move to the next input child
	gstate.Materialize(pipeline, event);

	return res;
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
OperatorResultType PhysicalIEJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                   GlobalOperatorState &gstate, OperatorState &state) const {
	return OperatorResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
enum class IEJoinSourceStage : uint8_t {
	INIT = 0,
	SINK_L1 = 1,
	FINALIZE_L1 = 2,
	MATERIALIZE_L1 = 3,
	EXTRACT_LI = 4,
	SINK_L2 = 5,
	FINALIZE_L2 = 6,
	MATERIALIZE_L2 = 7,
	EXTRACT_P = 8,
	INNER = 9,
	OUTER = 10,
	ANTI = 11,
	DONE = 12
};

struct IEJoinSourceTask {
	using ChunkRange = std::pair<idx_t, idx_t>;

	IEJoinSourceTask() {
	}

	IEJoinSourceStage stage = IEJoinSourceStage::DONE;
	//! The thread index (for local state)
	idx_t thread_idx = 0;
	//! The chunk range
	ChunkRange range;
};

class IEJoinLocalSourceState;

class IEJoinGlobalSourceState : public GlobalSourceState {
public:
	using Task = IEJoinSourceTask;
	using TaskPtr = optional_ptr<Task>;
	using SortedTable = PhysicalRangeJoin::GlobalSortedTable;

	IEJoinGlobalSourceState(const PhysicalIEJoin &op, ClientContext &client, IEJoinGlobalState &gsink);

	template <typename T>
	static T BinValue(T n, T val) {
		return ((n + (val - 1)) / val);
	}

	idx_t GetStageCount(IEJoinSourceStage stage) const {
		return stage_tasks[size_t(stage)];
	}

	atomic<idx_t> &GetStageNext(IEJoinSourceStage stage) {
		return completed[size_t(stage)];
	}

	//! The processing stage for this group
	IEJoinSourceStage GetStage() const {
		return stage;
	}

	//! The total number of tasks we will execute
	idx_t GetTaskCount() const {
		return stage_begin[size_t(IEJoinSourceStage::DONE)];
	}

	//! Are there any more tasks?
	bool HasMoreTasks() const {
		return !stopped && started < total_tasks;
	}
	bool HasUnfinishedTasks() const {
		return !stopped && finished < total_tasks;
	}

	bool TryPrepareNextStage();
	//! Finish the current task (if any), advance the stage if that was its last task, and try to get the next task.
	//! Runs under the lock so that a caller can decide to block on failure without missing an unblock.
	bool TryNextTaskLocked(TaskPtr &task, Task &task_local) DUCKDB_REQUIRES(lock);

	void ExecuteLiTask(ClientContext &client);
	void ExecutePermutationTask(ClientContext &client);

public:
	idx_t MaxThreads() override;

	ProgressData GetProgress() const;

	const PhysicalIEJoin &op;
	IEJoinGlobalState &gsink;

	//! The processing stage
	atomic<IEJoinSourceStage> stage;
	//! The the number of tasks per stage.
	array<idx_t, static_cast<uint8_t>(IEJoinSourceStage::DONE) + 1> stage_tasks;
	//! The the first task in the stage, all values are initialized to 0.
	array<idx_t, static_cast<uint8_t>(IEJoinSourceStage::DONE) + 1> stage_begin = {};
	//! The next task to process
	idx_t next_task = 0;
	//! The total number of tasks
	idx_t total_tasks = 0;
	//! The number of started tasks
	atomic<idx_t> started;
	//! The number of tasks finished.
	atomic<idx_t> finished;
	//! Stop producing tasks
	atomic<bool> stopped;
	//! The number of completed tasks for each stage
	array<atomic<idx_t>, static_cast<size_t>(IEJoinSourceStage::DONE) + 1> completed;
	//! The progress of the tasks
	TaskProgress task_progress;

	//! L1
	unique_ptr<SortedTable> l1;
	//! L2
	unique_ptr<SortedTable> l2;
	//! Li
	unique_ptr<ColumnDataCollection> li;
	//! P
	unique_ptr<ColumnDataCollection> p;

	// Join queue state
	idx_t l2_blocks = 0;
	idx_t per_thread = 0;
	idx_t left_blocks = 0;
	idx_t right_blocks = 0;

	// Outer joins
	idx_t left_outers = 0;
	idx_t right_outers = 0;

protected:
	void Initialize();
	void FinishTask(TaskPtr task);
	bool TryNextTask(Task &task);
};

IEJoinGlobalSourceState::IEJoinGlobalSourceState(const PhysicalIEJoin &op, ClientContext &client,
                                                 IEJoinGlobalState &gsink)
    : op(op), gsink(gsink), stage(IEJoinSourceStage::INIT), started(0), finished(0), stopped(false) {
	auto &left_table = *gsink.tables[0];
	auto &right_table = *gsink.tables[1];

	left_blocks = left_table.BlockCount();
	if (left_table.found_match) {
		left_outers = left_blocks;
	}

	right_blocks = right_table.BlockCount();
	if (right_table.found_match) {
		right_outers = right_blocks;
	}

	IEJoinUnion::InitializeTables(client, op, op.conditions, l1, l2);

	//	The number of blocks in L2 is not quite the sum of the blocks in the two tables...
	const auto join_count = left_table.count.load() + right_table.count.load();
	l2_blocks = BinValue<idx_t>(join_count, STANDARD_VECTOR_SIZE);

	//	Schedule the largest group on as many threads as possible
	auto &ts = TaskScheduler::GetScheduler(client);
	const auto threads = ts.NumberOfThreads();
	per_thread = BinValue<idx_t>(l2_blocks, threads);

	Initialize();
}

void IEJoinGlobalSourceState::ExecuteLiTask(ClientContext &client) {
	// We don't actually need the L1 column, just its sort key, which is in the sort blocks
	l1->GetSortedRun(client);

	li = IEJoinUnion::ExtractColumn(*l1, 1, BufferManager::GetBufferManager(client));
}

void IEJoinGlobalSourceState::ExecutePermutationTask(ClientContext &client) {
	// We don't actually need the L2 column, just its sort key, which is in the sort blocks
	l2->GetSortedRun(client);

	// 6. compute the permutation array P of L2 w.r.t. L1
	p = IEJoinUnion::ExtractColumn(*l2, 0, BufferManager::GetBufferManager(client));
}

class IEJoinLocalSourceState : public LocalSourceState {
public:
	using Task = IEJoinSourceTask;
	using TaskPtr = optional_ptr<Task>;

	IEJoinLocalSourceState(ClientContext &client, IEJoinGlobalSourceState &gsource)
	    : gsource(gsource), true_sel(STANDARD_VECTOR_SIZE), left_executor(client), right_executor(client),
	      simple_sel(STANDARD_VECTOR_SIZE), pred_executor(client), left_matches(nullptr), right_matches(nullptr)

	{
		auto &op = gsource.op;
		auto &allocator = Allocator::Get(client);
		unprojected.InitializeEmpty(op.unprojected_types);
		lpayload.Initialize(allocator, op.children[0].get().GetTypes());
		rpayload.Initialize(allocator, op.children[1].get().GetTypes());

		auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();
		auto &left_table = *ie_sink.tables[0];
		auto &right_table = *ie_sink.tables[1];

		left_iterator = left_table.CreateIteratorState();
		right_iterator = right_table.CreateIteratorState();

		left_table.InitializePayloadState(left_chunk_state);
		right_table.InitializePayloadState(right_chunk_state);

		left_scan_state = left_table.CreateScanState(client);
		right_scan_state = right_table.CreateScanState(client);

		if (op.predicate) {
			pred_executor.AddExpression(*op.predicate);
			pred_matches.Initialize();
		}

		if (op.conditions.size() < 3) {
			return;
		}

		vector<LogicalType> left_types;
		vector<LogicalType> right_types;
		for (idx_t i = 2; i < op.conditions.size(); ++i) {
			const auto &cond = op.conditions[i];

			left_types.push_back(cond.GetLHS().GetReturnType());
			left_executor.AddExpression(cond.GetLHS());

			right_types.push_back(cond.GetLHS().GetReturnType());
			right_executor.AddExpression(cond.GetRHS());
		}

		left_keys.Initialize(allocator, left_types);
		right_keys.Initialize(allocator, right_types);
	}

	idx_t SelectOuterRows(bool *matches) {
		outer_sel.resize(0);
		for (; outer_idx < outer_count; ++outer_idx) {
			if (!matches[outer_idx]) {
				outer_sel.emplace_back(outer_idx);
				if (outer_sel.size() >= STANDARD_VECTOR_SIZE) {
					outer_idx++;
					break;
				}
			}
		}

		return outer_sel.size();
	}

	//	Are we executing a task?
	bool TaskFinished() const {
		return !joiner && !left_matches && !right_matches;
	}

	//! Release the local state of the current task (if any)
	void FinishTask();
	//! Set up the local state for executing a task acquired through IEJoinGlobalSourceState::TryNextTaskLocked
	void InitializeTask();
	//! Reports the progress of the current task
	void UpdateProgress() {
		if (!task) {
			return;
		}
		if (TaskFinished()) {
			task_progress.Finish(gsource.task_progress);
		} else if (task->stage == IEJoinSourceStage::INNER && joiner) {
			// the inner join advances over the rows of its range
			auto done = joiner->i > inner_begin ? joiner->i - inner_begin : 0;
			auto total = joiner->n > inner_begin ? joiner->n - inner_begin : 0;
			task_progress.Update(gsource.task_progress, done, total);
		}
	}
	//	Sort L1
	void ExecuteSinkL1Task(ExecutionContext &context, InterruptState &interrupt);
	//	Finalize L1 sort
	void ExecuteFinalizeL1Task(ExecutionContext &context, InterruptState &interrupt);
	//	Materialize L1
	void ExecuteMaterializeL1Task(ExecutionContext &context, InterruptState &interrupt);
	//	Sort L2
	void ExecuteSinkL2Task(ExecutionContext &context, InterruptState &interrupt);
	//	Finalize L2 sort
	void ExecuteFinalizeL2Task(ExecutionContext &context, InterruptState &interrupt);
	//	Materialize L2
	void ExecuteMaterializeL2Task(ExecutionContext &context, InterruptState &interrupt);
	// 	Resolve simple and complex inner joins
	void ResolveInnerJoin(ExecutionContext &context, DataChunk &result);
	//	Derive an SV that applies the remaining conditions
	const SelectionVector *ApplyTailConditions();
	//	Combine the two payloads into a single chunk that can be used for projection or predicate evaluation
	void MergePayloads(DataChunk &chunk);
	//	Separate a merged chunk back into the two payloads.
	void SplitPayloads(DataChunk &chunk);
	// Apply any arbitrary predicate and return the new SV
	const SelectionVector *ApplyArbitraryPredicate(DataChunk &chunk);
	// resolve joins that can potentially output N elements (SEMI, ANTI, MARK)
	void ResolveSimpleJoin(ExecutionContext &context);
	// Use the given SV to strip out LHS duplicates. Return the number of unique rows
	idx_t FilterSemiJoin(const SelectionVector *sel);
	//	Build a set of values and matches fror ANTI and MARK joins
	idx_t FindSimpleMatches(ExecutionContext &context, bool *found_match = nullptr);
	// 	Resolve SEMI joins
	void ResolveSemiJoin(ExecutionContext &context, DataChunk &result);
	// 	Resolve ANTI joins
	void ResolveAntiJoin(ExecutionContext &context, DataChunk &result);
	// 	Resolve MARK joins
	void ResolveMarkJoin(ExecutionContext &context, DataChunk &result);
	// resolve joins that can potentially output N*M elements (INNER, LEFT, RIGHT, FULL)
	void ResolveComplexJoin(ExecutionContext &context, DataChunk &result);
	//	Resolve left join results
	void ExecuteLeftTask(ExecutionContext &context, DataChunk &result);
	//	Resolve right join results
	void ExecuteRightTask(ExecutionContext &context, DataChunk &result);
	//	Resolve ANTI join results from NULL columns.
	void ExecuteAntiTask(ExecutionContext &context, DataChunk &result);
	//	Resolve MARK join results from NULL columns.
	void ExecuteMarkTask(ExecutionContext &context, DataChunk &result);
	//	Execute the current task
	void ExecuteTask(ExecutionContext &context, DataChunk &result, InterruptState &interrupt);

	IEJoinGlobalSourceState &gsource;

	//! The task this thread is working on
	TaskPtr task;
	//! The task storage
	Task task_local;
	//! The progress reported for the current task
	TaskProgressTracker task_progress;
	//! The first row of the current inner join task
	idx_t inner_begin = 0;

	// Joining
	unique_ptr<IEJoinUnion> joiner;

	idx_t left_base;
	idx_t left_block_index;
	unique_ptr<ExternalBlockIteratorState> left_iterator;
	TupleDataChunkState left_chunk_state;
	unsafe_vector<idx_t> lsel;
	DataChunk lpayload;
	unique_ptr<SortedRunScanState> left_scan_state;

	idx_t right_base;
	idx_t right_block_index;
	unique_ptr<ExternalBlockIteratorState> right_iterator;
	TupleDataChunkState right_chunk_state;
	unsafe_vector<idx_t> rsel;
	DataChunk rpayload;
	unique_ptr<SortedRunScanState> right_scan_state;

	// Trailing predicates
	SelectionVector true_sel;

	ExpressionExecutor left_executor;
	DataChunk left_keys;

	ExpressionExecutor right_executor;
	DataChunk right_keys;

	DataChunk unprojected;

	//! Simple join subselection
	SelectionVector simple_sel;

	//! Arbitrary expressions
	ExpressionExecutor pred_executor;
	SelectionVector pred_matches;

	//! Outer joins
	unsafe_vector<idx_t> outer_sel;
	idx_t outer_idx;
	idx_t outer_count;
	bool *left_matches;
	bool *right_matches;

	//! Simple Joins
	idx_t anti_lsel = 0;

	//! Whether this task has produced any rows yet.
	//! A task that has produced rows must not block waiting for other tasks: the sink may be waiting for it.
	bool produced = false;
};

void IEJoinLocalSourceState::FinishTask() {
	// Because downstream operators may be using our internal buffers,
	// we can't "finish" a task until we are about to get the next one.
	if (!task) {
		return;
	}
	task_progress.Finish(gsource.task_progress);
	left_matches = nullptr;
	right_matches = nullptr;
}

void IEJoinLocalSourceState::InitializeTask() {
	task_progress.Start();

	auto &gsink = gsource.gsink;
	auto &left_table = *gsink.tables[0];
	auto &right_table = *gsink.tables[1];

	switch (task->stage) {
	case IEJoinSourceStage::SINK_L1:
	case IEJoinSourceStage::FINALIZE_L1:
	case IEJoinSourceStage::MATERIALIZE_L1:
	case IEJoinSourceStage::EXTRACT_LI:
	case IEJoinSourceStage::SINK_L2:
	case IEJoinSourceStage::FINALIZE_L2:
	case IEJoinSourceStage::MATERIALIZE_L2:
	case IEJoinSourceStage::EXTRACT_P:
		break;
	case IEJoinSourceStage::INNER:
		//	The join can hit any block on either side
		left_block_index = 0;
		left_base = 0;

		right_block_index = 0;
		right_base = 0;

		joiner = make_uniq<IEJoinUnion>(*gsource.l2, *gsource.li, *gsource.p, gsource.op.conditions, task->range);
		inner_begin = joiner->i;
		break;
	case IEJoinSourceStage::OUTER:
		if (task->thread_idx < gsource.left_outers) {
			left_block_index = task->range.first;
			left_base = left_table.BlockStart(left_block_index);

			left_matches = left_table.found_match.get() + left_base;
			outer_idx = 0;
			outer_count = left_table.BlockSize(left_block_index);
		} else {
			right_block_index = task->range.first;
			right_base = right_table.BlockStart(right_block_index);

			right_matches = right_table.found_match.get() + right_base;
			outer_idx = 0;
			outer_count = right_table.BlockSize(right_block_index);
		}
		break;
	case IEJoinSourceStage::ANTI:
		left_block_index = task->range.first;
		outer_idx = 0;
		outer_count = left_table.BlockSize(left_block_index);
		//	Skip any non-NULL entries in the block
		left_base = left_table.BlockStart(left_block_index);
		right_base = left_table.count - left_table.has_null;
		if (left_base < right_base) {
			outer_idx = right_base - left_base;
		}
		break;
	case IEJoinSourceStage::INIT:
	case IEJoinSourceStage::DONE:
		break;
	}
}

void IEJoinLocalSourceState::ExecuteSinkL1Task(ExecutionContext &context, InterruptState &interrupt) {
	auto &gsink = gsource.gsink;
	auto &left_table = *gsink.tables[0];
	auto &right_table = *gsink.tables[1];

	auto &op = gsource.op;
	const auto &order1 = op.lhs_orders[0];
	const auto &order2 = op.lhs_orders[1];

	auto &l1 = gsource.l1;

	//	Process the LHS sub-range
	if (task->range.first < gsource.left_blocks) {
		auto range = task->range;
		range.second = MinValue(gsource.left_blocks, range.second);

		// LHS has positive rids
		ExpressionExecutor l_executor(context.client);
		l_executor.AddExpression(*order1.expression);
		// add const column true
		auto left_const = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		l_executor.AddExpression(*left_const);
		l_executor.AddExpression(*order2.expression);
		const auto rid = UnsafeNumericCast<int64_t>(left_table.BlockStart(range.first)) + 1;
		IEJoinUnion::AppendKey(context, interrupt, left_table, l_executor, *l1, 1, rid, range);
	}

	//	Process the RHS sub-range
	if (task->range.second > gsource.left_blocks) {
		auto range = task->range;
		range.first = MaxValue(gsource.left_blocks, range.first) - gsource.left_blocks;
		range.second -= gsource.left_blocks;

		// RHS has negative rids
		ExpressionExecutor r_executor(context.client);
		r_executor.AddExpression(*op.rhs_orders[0].expression);
		// add const column false
		auto right_const = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
		r_executor.AddExpression(*right_const);
		r_executor.AddExpression(*op.rhs_orders[1].expression);
		const auto rid = UnsafeNumericCast<int64_t>(right_table.BlockStart(range.first)) + 1;
		IEJoinUnion::AppendKey(context, interrupt, right_table, r_executor, *l1, -1, -rid, range);
	}
}

void IEJoinLocalSourceState::ExecuteFinalizeL1Task(ExecutionContext &context, InterruptState &interrupt) {
	gsource.l1->Finalize(context.client, interrupt);
}

void IEJoinLocalSourceState::ExecuteMaterializeL1Task(ExecutionContext &context, InterruptState &interrupt) {
	gsource.l1->MaterializeSortedRun(context, interrupt);
}

void IEJoinLocalSourceState::ExecuteSinkL2Task(ExecutionContext &context, InterruptState &interrupt) {
	auto &l1 = *gsource.l1;
	auto &l2 = *gsource.l2;

	auto &op = gsource.op;
	const auto &order2 = op.lhs_orders[1];
	auto ref = make_uniq<BoundReferenceExpression>(order2.expression->GetReturnType(), 0U);

	ExpressionExecutor executor(context.client);
	executor.AddExpression(*ref);
	const auto rid = UnsafeNumericCast<int64_t>(l1.BlockStart(task->range.first));
	IEJoinUnion::AppendKey(context, interrupt, l1, executor, l2, 1, rid, task->range);
}

void IEJoinLocalSourceState::ExecuteFinalizeL2Task(ExecutionContext &context, InterruptState &interrupt) {
	gsource.l2->Finalize(context.client, interrupt);
}

void IEJoinLocalSourceState::ExecuteMaterializeL2Task(ExecutionContext &context, InterruptState &interrupt) {
	gsource.l2->MaterializeSortedRun(context, interrupt);
}

void IEJoinLocalSourceState::ExecuteTask(ExecutionContext &context, DataChunk &result, InterruptState &interrupt) {
	switch (task->stage) {
	case IEJoinSourceStage::INIT:
	case IEJoinSourceStage::DONE:
		break;
	case IEJoinSourceStage::SINK_L1:
		ExecuteSinkL1Task(context, interrupt);
		break;
	case IEJoinSourceStage::FINALIZE_L1:
		ExecuteFinalizeL1Task(context, interrupt);
		break;
	case IEJoinSourceStage::MATERIALIZE_L1:
		ExecuteMaterializeL1Task(context, interrupt);
		break;
	case IEJoinSourceStage::EXTRACT_LI:
		gsource.ExecuteLiTask(context.client);
		break;
		break;
	case IEJoinSourceStage::SINK_L2:
		ExecuteSinkL2Task(context, interrupt);
		break;
	case IEJoinSourceStage::FINALIZE_L2:
		ExecuteFinalizeL2Task(context, interrupt);
		break;
	case IEJoinSourceStage::MATERIALIZE_L2:
		ExecuteMaterializeL2Task(context, interrupt);
		break;
	case IEJoinSourceStage::EXTRACT_P:
		gsource.ExecutePermutationTask(context.client);
		break;
	case IEJoinSourceStage::INNER:
		ResolveInnerJoin(context, result);
		break;
	case IEJoinSourceStage::OUTER:
		if (left_matches != nullptr) {
			ExecuteLeftTask(context, result);
		} else if (right_matches != nullptr) {
			ExecuteRightTask(context, result);
		}
		break;
	case IEJoinSourceStage::ANTI:
		if (gsource.op.join_type == JoinType::ANTI) {
			ExecuteAntiTask(context, result);
		} else {
			ExecuteMarkTask(context, result);
		}
		break;
	}
}

void IEJoinLocalSourceState::ResolveInnerJoin(ExecutionContext &context, DataChunk &result) {
	// simple joins can have max STANDARD_VECTOR_SIZE matches per chunk
	switch (gsource.op.join_type) {
	case JoinType::SEMI:
		ResolveSemiJoin(context, result);
		break;
	case JoinType::ANTI:
		ResolveAntiJoin(context, result);
		break;
	case JoinType::MARK:
		ResolveMarkJoin(context, result);
		break;
	case JoinType::LEFT:
	case JoinType::INNER:
	case JoinType::RIGHT:
	case JoinType::OUTER:
		ResolveComplexJoin(context, result);
		break;
	default:
		throw NotImplementedException("Unimplemented join type for IEJoin!");
	}

	if (result.size() == 0) {
		// exhausted this pair
		joiner.reset();
	}
}

const SelectionVector *IEJoinLocalSourceState::ApplyTailConditions() {
	auto &op = gsource.op;
	const auto &conditions = op.conditions;
	auto sel = FlatVector::IncrementalSelectionVector();
	if (conditions.size() <= 2) {
		return sel;
	}

	// If there are more expressions to compute,
	// use the left and right payloads
	// to we can compute the values for comparison.
	const auto tail_cols = conditions.size() - 2;

	left_executor.SetChunk(lpayload);
	right_executor.SetChunk(rpayload);

	auto result_count = lpayload.size();
	auto tail_count = result_count;
	auto match_sel = &true_sel;
	left_keys.Reset();
	right_keys.Reset();
	for (size_t cmp_idx = 0; cmp_idx < tail_cols; ++cmp_idx) {
		auto &left = left_keys.data[cmp_idx];
		left_executor.ExecuteExpression(cmp_idx, left);

		auto &right = right_keys.data[cmp_idx];
		right_executor.ExecuteExpression(cmp_idx, right);

		if (tail_count < result_count) {
			left.Slice(*sel, tail_count);
			right.Slice(*sel, tail_count);
		}
		tail_count =
		    op.SelectJoinTail(conditions[cmp_idx + 2].GetComparisonType(), left, right, sel, tail_count, match_sel);
		sel = match_sel;
	}

	if (tail_count < result_count) {
		lpayload.Slice(*sel, tail_count);
		rpayload.Slice(*sel, tail_count);
	}

	return sel;
}

void IEJoinLocalSourceState::MergePayloads(DataChunk &chunk) {
	//	Merge the payloads
	auto &op = gsource.op;
	const auto left_cols = op.children[0].get().GetTypes().size();
	chunk.Reset();
	for (column_t col_idx = 0; col_idx < chunk.ColumnCount(); ++col_idx) {
		if (col_idx < left_cols) {
			chunk.data[col_idx].Reference(lpayload.data[col_idx]);
		} else {
			chunk.data[col_idx].Reference(rpayload.data[col_idx - left_cols]);
		}
	}
	chunk.SetChildCardinality(lpayload.size());
}

void IEJoinLocalSourceState::SplitPayloads(DataChunk &chunk) {
	//	Extract the payloads from a merged chunk
	auto &op = gsource.op;
	const auto left_cols = op.children[0].get().GetTypes().size();
	lpayload.Reset();
	rpayload.Reset();
	for (column_t col_idx = 0; col_idx < chunk.ColumnCount(); ++col_idx) {
		if (col_idx < left_cols) {
			lpayload.data[col_idx].Reference(chunk.data[col_idx]);
		} else {
			rpayload.data[col_idx - left_cols].Reference(chunk.data[col_idx]);
		}
	}
}

const SelectionVector *IEJoinLocalSourceState::ApplyArbitraryPredicate(DataChunk &chunk) {
	//	Apply any arbitrary predicate
	D_ASSERT(gsource.op.predicate);

	const auto result_count = pred_executor.SelectExpression(chunk, pred_matches);
	chunk.Slice(pred_matches, result_count);
	return &pred_matches;
}

idx_t IEJoinLocalSourceState::FilterSemiJoin(const SelectionVector *sel) {
	//	Strip out remaining left side duplicates
	const auto result_count = lpayload.size();
	idx_t unique_count = 0;
	if (!result_count) {
		return unique_count;
	}

	simple_sel.set_index(unique_count, 0);
	lsel[unique_count++] = lsel[sel->get_index(0)];
	for (idx_t i = 1; i < result_count; ++i) {
		const auto iprev = sel->get_index(i - 1);
		const auto icurr = sel->get_index(i - 0);
		if (lsel[icurr] == lsel[iprev]) {
			continue;
		}
		simple_sel.set_index(unique_count, i);
		lsel[unique_count++] = lsel[icurr];
	}
	lsel.resize(unique_count);

	//	Slice the join pairs down to the unique rows
	if (unique_count < result_count) {
		lpayload.Slice(simple_sel, unique_count);
		rpayload.Slice(simple_sel, unique_count);
	}

	return unique_count;
}

void IEJoinLocalSourceState::ResolveSimpleJoin(ExecutionContext &context) {
	auto &op = gsource.op;
	const auto &conditions = op.conditions;
	D_ASSERT(conditions.size() >= 2);

	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();
	auto &left_table = *ie_sink.tables[0];

	lpayload.Reset();

	do {
		auto result_count = joiner->JoinBlocks(lsel, rsel);
		if (result_count == 0) {
			// exhausted this pair
			return;
		}

		// Apply any remaining filters
		if (conditions.size() > 2 || op.predicate) {
			//	Read all the LHS rows because we need to apply the filters to every possible match
			left_table.Repin(*left_iterator);
			op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, lsel,
			                      *left_scan_state);

			auto &right_table = *ie_sink.tables[1];
			right_table.Repin(*right_iterator);
			op.SliceSortedPayload(rpayload, right_table, *right_iterator, right_chunk_state, right_block_index, rsel,
			                      *right_scan_state);

			//	Apply any tail conditions
			if (conditions.size() > 2) {
				//	Strip out remaining left side duplicates
				auto sel = ApplyTailConditions();
				result_count = FilterSemiJoin(sel);

				//	If we filtered out everything, go back for more
				if (result_count == 0) {
					continue;
				}
			}

			//	Apply any arbitrary predicate
			if (op.predicate) {
				//	Arbitrary predicates require a merged chunk,
				//	but we need to make sure no one is reusing SVs so we resplit.
				auto &chunk = unprojected;
				MergePayloads(chunk);

				auto sel = ApplyArbitraryPredicate(chunk);
				SplitPayloads(chunk);

				//	Strip out remaining left side duplicates
				result_count = FilterSemiJoin(sel);

				//	If we filtered out everything, go back for more
				if (!result_count) {
					continue;
				}
			}
		} else {
			//	Fast track for only two predicates
			//	Strip out left side duplicates
			idx_t unique_count = 1;
			for (idx_t i = 1; i < result_count; ++i) {
				if (lsel[i] == lsel[i - 1]) {
					continue;
				}
				lsel[unique_count++] = lsel[i];
			}
			lsel.resize(unique_count);
			result_count = unique_count;

			// Read the remaining unique rows
			left_table.Repin(*left_iterator);
			op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, lsel,
			                      *left_scan_state);
		}

		//	Handle chunk boundaries: If we found a match for the last value
		//	then move to the next rid (which might be negative).
		//	Note that the saved rid is offset by 1.
		if (joiner->lrid > 0 && lsel[result_count - 1] == UnsafeNumericCast<idx_t>(joiner->lrid - 1)) {
			joiner->FinishRow();
		}
	} while (lpayload.size() == 0);
}

void IEJoinLocalSourceState::ResolveSemiJoin(ExecutionContext &context, DataChunk &result) {
	//	SEMI JOINs return all the columns from the LHS
	ResolveSimpleJoin(context);
	result.Reference(lpayload);
	result.Verify(context.client.db);
}

idx_t IEJoinLocalSourceState::FindSimpleMatches(ExecutionContext &context, bool *found_match) {
	//	Merge lsel and unmatched LHS rids into (the unused) rsel, tracking the matches
	auto &msel = rsel;
	idx_t result_count = 0;
	msel.resize(STANDARD_VECTOR_SIZE);

	//	Scan through Li, merging anything in lsel.
	//	They are both ordered in the same way, so we can ratchet
	const auto i = MinValue(joiner->i, joiner->n);
	auto &anti_i = joiner->anti_i;
	auto &p = joiner->p;
	auto &li = joiner->li;

	//	Scan through the SEMI JOIN rids
	//	Note that if there are no more matches, then lsel will be empty()
	//	and i will be at the end of the range
	while (anti_i < i) {
		//	Get the next lrid
		auto pos = p[anti_i++];
		auto rid = li[pos];
		if (rid <= 0) {
			continue;
		}
		const auto lrid = UnsafeNumericCast<idx_t>(rid - 1);

		//	If the lrid is in the SEMI JOIN, then merge it and mark the row as matched.
		if (anti_lsel < lsel.size() && lsel[anti_lsel] == lrid) {
			++anti_lsel;
			//	If we are not tracking matches, then only keep the unmatched rows
			if (!found_match) {
				continue;
			}
			found_match[result_count] = true;
		} else if (found_match) {
			//	Found an unmatched rid, so mark it as unmatched
			found_match[result_count] = false;
		}
		msel[result_count++] = lrid;

		//	If we have a full vector, stop
		if (result_count >= STANDARD_VECTOR_SIZE) {
			break;
		}
	}
	msel.resize(result_count);

	return result_count;
}

void IEJoinLocalSourceState::ResolveAntiJoin(ExecutionContext &context, DataChunk &result) {
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();
	auto &left_table = *ie_sink.tables[0];

	//	Generate the SEMI JOIN and then invert it.
	//	This can result in more than one chunk at a time,
	//	so we only call ResolveSemiJoin to "refill" the buffer (find the next batch of matches)
	//	when we run out of anti-matches
	do {
		if (anti_lsel >= lsel.size()) {
			ResolveSimpleJoin(context);
			anti_lsel = 0;
		}

		//	Put all the non-matches into rsel
		const idx_t result_count = FindSimpleMatches(context);

		if (result_count == 0) {
			//	If there were no SEMI rows and we didn't find any past the last one,
			//	then we are done.
			if (lsel.empty()) {
				return;
			}
			continue;
		}

		// 	Read the remaining unique rows
		left_table.Repin(*left_iterator);
		op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, rsel,
		                      *left_scan_state);

		result.Reference(lpayload);
		result.Verify(context.client.db);
	} while (result.size() == 0);
}

void IEJoinLocalSourceState::ResolveMarkJoin(ExecutionContext &context, DataChunk &result) {
	//	MARK JOINs return all the columns from the LHS plus the mark column
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();
	auto &left_table = *ie_sink.tables[0];
	auto &right_table = *ie_sink.tables[1];

	//	We need to have _all_ the rows, so we perform the SEMI JOIN,
	//	then go back and merge in the ANTI JOIN rows.
	//	This means we might not consume all the SEMI rows, so only get more when we have run out.
	if (anti_lsel >= lsel.size()) {
		ResolveSimpleJoin(context);
		anti_lsel = 0;
	}

	//	Merge lsel and unmatched LHS rids into (the unused) rsel, tracking the matches
	bool found_match[STANDARD_VECTOR_SIZE];
	FindSimpleMatches(context, found_match);

	//	Read the lhs rows
	left_table.Repin(*left_iterator);
	op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, rsel,
	                      *left_scan_state);

	//	Compute the residual keys
	//	We know here that the two sort columns are not NULL, so the tail columns are all we need
	left_executor.Execute(&lpayload, left_keys);

	//	Now hand it off to code that knows the rules...
	//	Note that it can handle left_keys.ColumnCount().empty()
	PhysicalJoin::ConstructMarkJoinResult(left_keys, lpayload, result, found_match, right_table.has_null);

	result.Verify(context.client.db);
}

void IEJoinLocalSourceState::ResolveComplexJoin(ExecutionContext &context, DataChunk &result) {
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();

	auto &chunk = unprojected;

	auto &left_table = *ie_sink.tables[0];
	auto &right_table = *ie_sink.tables[1];

	do {
		auto result_count = joiner->JoinBlocks(lsel, rsel);
		if (result_count == 0) {
			// exhausted this pair
			return;
		}

		// found matches: extract them

		left_table.Repin(*left_iterator);
		right_table.Repin(*right_iterator);

		op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, lsel,
		                      *left_scan_state);
		op.SliceSortedPayload(rpayload, right_table, *right_iterator, right_chunk_state, right_block_index, rsel,
		                      *right_scan_state);

		auto sel = ApplyTailConditions();

		//	If we filtered out everything, go back for more
		result_count = lpayload.size();
		if (!result_count) {
			continue;
		}

		//	Apply any arbitrary predicate
		MergePayloads(chunk);
		SelectionVector match_sel;
		if (op.predicate) {
			//	The predicate selection is relative to the rows that passed the tail conditions,
			//	so combine the two to index the original join pairs
			auto tail_sel = sel;
			auto predicate_sel = ApplyArbitraryPredicate(chunk);
			result_count = chunk.size();
			if (!result_count) {
				continue;
			}
			match_sel.Initialize(result_count);
			for (idx_t i = 0; i < result_count; i++) {
				match_sel.set_index(i, tail_sel->get_index(predicate_sel->get_index(i)));
			}
			sel = &match_sel;
		}

		//	We need all of the data to compute other predicates,
		//	but we only return what is in the projection map
		op.ProjectResult(chunk, result);

		// found matches: mark the found matches if required
		// NOTE: threadsan reports this as a data race because this can be set concurrently by separate
		// threads Technically it is, but it does not matter, since the only value that can be written is
		// "true"
		if (left_table.found_match) {
			for (idx_t i = 0; i < result_count; i++) {
				left_table.found_match[left_base + lsel[sel->get_index(i)]] = true;
			}
		}
		if (right_table.found_match) {
			for (idx_t i = 0; i < result_count; i++) {
				right_table.found_match[right_base + rsel[sel->get_index(i)]] = true;
			}
		}
		result.Verify(context.client.db);
	} while (result.size() == 0);
}

void IEJoinGlobalSourceState::Initialize() {
	//	INIT
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::INIT)] = 0;

	//	SINK_L1
	idx_t l1_tasks = 0;
	if (per_thread) {
		l1_tasks = BinValue<idx_t>(left_blocks + right_blocks, per_thread);
	}
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::SINK_L1)] = l1_tasks;

	//	FINALIZE_L1
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::FINALIZE_L1)] = 1;

	//	MATERIALIZE_L1
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::MATERIALIZE_L1)] = MaxValue<idx_t>(l1_tasks, 1);

	//	EXTRACT_LI
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::EXTRACT_LI)] = 1;

	//	SINK_L2
	idx_t l2_tasks = 0;
	if (per_thread) {
		l2_tasks = BinValue<idx_t>(l2_blocks, per_thread);
	}
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::SINK_L2)] = l2_tasks;

	//	FINALIZE_L2
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::FINALIZE_L2)] = 1;

	//	MATERIALIZE_L2
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::MATERIALIZE_L2)] = MaxValue<idx_t>(l2_tasks, 1);

	//	EXTRACT_P
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::EXTRACT_P)] = 1;

	//	INNER
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::INNER)] = l2_tasks;

	//	OUTER
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::OUTER)] = left_outers + right_outers;

	//	ANTI
	idx_t anti_tasks = 0;
	if (op.join_type == JoinType::ANTI || op.join_type == JoinType::MARK) {
		auto &left_table = *gsink.tables[0];
		const auto null_block = (left_table.count - left_table.has_null) / STANDARD_VECTOR_SIZE;
		anti_tasks = left_blocks - null_block;
	}
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::ANTI)] = anti_tasks;

	//	DONE
	stage_tasks[static_cast<uint8_t>(IEJoinSourceStage::DONE)] = 0;

	//	Accumulate task counts so we can find boundaries reliably
	idx_t begin = 0;
	for (idx_t i = 0; i < stage_tasks.size(); i++) {
		auto &stage_task = stage_tasks[i];
		stage_begin[i] = begin;
		begin += stage_task;
	}

	total_tasks = stage_begin.back();

	//	Set all the stage atomic counts to 0
	for (auto &stage_next : completed) {
		stage_next = 0;
	}

	// Ready for action
	stage = IEJoinSourceStage(1);
}

bool IEJoinGlobalSourceState::TryPrepareNextStage() {
	//	Inside lock
	const auto stage_count = GetStageCount(stage);
	const auto stage_next = GetStageNext(stage).load();
	if (stage_next >= stage_count) {
		auto stage_curr = stage.load();
		while (stage_curr < IEJoinSourceStage::DONE) {
			stage_curr = IEJoinSourceStage(size_t(stage_curr) + 1);
			if (GetStageCount(stage_curr)) {
				break;
			}
		}
		stage = stage_curr;
		return true;
	}

	return false;
}

idx_t IEJoinGlobalSourceState::MaxThreads() {
	// We can't leverage any more threads than tasks.
	return *std::max_element(stage_tasks.begin(), stage_tasks.end());
}

void IEJoinGlobalSourceState::FinishTask(TaskPtr task) {
	//	Inside the lock
	if (!task) {
		return;
	}

	++finished;
	++GetStageNext(task->stage);
}

bool IEJoinGlobalSourceState::TryNextTaskLocked(TaskPtr &task, Task &task_local) {
	FinishTask(task);
	task = nullptr;

	//	If that was the last task of the stage, move on and wake up the tasks waiting for the next stage
	if (TryPrepareNextStage() || stage == IEJoinSourceStage::DONE) {
		UnblockTasks();
	}

	if (!HasMoreTasks()) {
		return false;
	}

	if (TryNextTask(task_local)) {
		task = task_local;
		++started;
		return true;
	}

	return false;
}

bool IEJoinGlobalSourceState::TryNextTask(Task &task) {
	//	Inside lock
	if (next_task >= GetTaskCount()) {
		return false;
	}

	//	Search for where we are in the task list
	for (idx_t stage = idx_t(IEJoinSourceStage::INIT); stage <= idx_t(IEJoinSourceStage::DONE); ++stage) {
		if (next_task < stage_begin[stage]) {
			task.stage = IEJoinSourceStage(stage - 1);
			task.thread_idx = next_task - stage_begin[size_t(task.stage)];
			break;
		}
	}

	if (task.stage != stage) {
		return false;
	}

	switch (stage.load()) {
	case IEJoinSourceStage::SINK_L1:
		task.range.first = MinValue(task.thread_idx * per_thread, left_blocks + right_blocks);
		task.range.second = MinValue(task.range.first + per_thread, left_blocks + right_blocks);
		break;
	case IEJoinSourceStage::FINALIZE_L1:
	case IEJoinSourceStage::MATERIALIZE_L1:
	case IEJoinSourceStage::EXTRACT_LI:
		break;
	case IEJoinSourceStage::SINK_L2:
		task.range.first = MinValue(task.thread_idx * per_thread, l1->BlockCount());
		task.range.second = MinValue(task.range.first + per_thread, l1->BlockCount());
		break;
	case IEJoinSourceStage::FINALIZE_L2:
	case IEJoinSourceStage::MATERIALIZE_L2:
	case IEJoinSourceStage::EXTRACT_P:
		break;
	case IEJoinSourceStage::INNER: {
		task.range.first = task.thread_idx * per_thread;
		task.range.second = MinValue(task.range.first + per_thread, l2_blocks);
		break;
	}
	case IEJoinSourceStage::OUTER:
		if (task.thread_idx < left_outers) {
			// Left outer blocks
			const auto left_task = task.thread_idx;
			task.range = {left_task, left_task + 1};
		} else {
			// Right outer blocks
			const auto right_task = task.thread_idx - left_outers;
			task.range = {right_task, right_task + 1};
		}
		break;
	case IEJoinSourceStage::ANTI: {
		//	ANTI JOINs return every row with NULL comparisons.
		const auto &left_table = *gsink.tables[0];
		const auto null_start = (left_table.count - left_table.has_null);
		const auto null_block = null_start / STANDARD_VECTOR_SIZE;
		const auto anti_task = task.thread_idx;
		task.range = {null_block + anti_task, null_block + anti_task + 1};
		break;
	}
	case IEJoinSourceStage::INIT:
	case IEJoinSourceStage::DONE:
		break;
	}

	++next_task;

	return true;
}

ProgressData IEJoinGlobalSourceState::GetProgress() const {
	const auto count = GetTaskCount();
	if (!count) {
		ProgressData res;
		res.SetInvalid();
		return res;
	}
	return task_progress.GetProgress(count);
}
unique_ptr<GlobalSourceState> PhysicalIEJoin::GetGlobalSourceState(ClientContext &client) const {
	auto &gsink = sink_state->Cast<IEJoinGlobalState>();
	return make_uniq<IEJoinGlobalSourceState>(*this, client, gsink);
}

unique_ptr<LocalSourceState> PhysicalIEJoin::GetLocalSourceState(ExecutionContext &context,
                                                                 GlobalSourceState &gstate) const {
	auto &gsource = gstate.Cast<IEJoinGlobalSourceState>();
	return make_uniq<IEJoinLocalSourceState>(context.client, gsource);
}

ProgressData PhysicalIEJoin::GetProgress(ClientContext &context, GlobalSourceState &gsource_p) const {
	auto &gsource = gsource_p.Cast<IEJoinGlobalSourceState>();
	return gsource.GetProgress();
}

SourceResultType PhysicalIEJoin::GetDataInternal(ExecutionContext &context, DataChunk &result,
                                                 OperatorSourceInput &input) const {
	auto &gsource = input.global_state.Cast<IEJoinGlobalSourceState>();
	auto &lsource = input.local_state.Cast<IEJoinLocalSourceState>();

	// Any call to GetData must produce tuples, otherwise the pipeline executor thinks that we're done
	// Therefore, we loop until we've produced tuples, or until the operator is actually done
	while (gsource.stage != IEJoinSourceStage::DONE && result.size() == 0) {
		if (lsource.TaskFinished()) {
			{
				// Finish the current task, advance the stage if it was the last one, look for the next task and
				// decide what to do without one, all under a single acquisition of the lock: the decision to block
				// has to be made under the same lock as the failed search, or the unblock could slip in between
				// and no one would ever wake this task up again.
				annotated_lock_guard<annotated_mutex> guard(gsource.lock);
				lsource.FinishTask();
				if (!gsource.TryNextTaskLocked(lsource.task, lsource.task_local)) {
					if (gsource.stage == IEJoinSourceStage::DONE) {
						break;
					}
					if (lsource.produced) {
						// The remaining tasks can only start once the other tasks of this stage have finished.
						// This task has already handed rows to the sink, which may in turn be waiting for this task
						// (e.g., a partitioned COPY cannot flush until every producer has combined), so blocking
						// here could deadlock: let the tasks that are still running finish the join instead.
						return SourceResultType::FINISHED;
					}
					// there are more tasks available, but we can't execute them yet
					// block the source
					return gsource.BlockSource(input.interrupt_state);
				}
			}
			// Set up the task outside the lock: it allocates per-task state
			lsource.InitializeTask();
		}
		lsource.ExecuteTask(context, result, input.interrupt_state);
		lsource.UpdateProgress();
		if (result.size() != 0) {
			lsource.produced = true;
		}
	}
	return result.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void IEJoinLocalSourceState::ExecuteLeftTask(ExecutionContext &context, DataChunk &result) {
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();

	const auto left_cols = op.children[0].get().GetTypes().size();
	auto &chunk = unprojected;

	const idx_t count = SelectOuterRows(left_matches);
	if (!count) {
		left_matches = nullptr;
		return;
	}

	auto &left_table = *ie_sink.tables[0];

	left_table.Repin(*left_iterator);
	op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, outer_sel,
	                      *left_scan_state);

	// Fill in NULLs to the right
	chunk.Reset();
	for (column_t col_idx = 0; col_idx < chunk.ColumnCount(); ++col_idx) {
		if (col_idx < left_cols) {
			chunk.data[col_idx].Reference(lpayload.data[col_idx]);
		} else {
			ConstantVector::SetNull(chunk.data[col_idx], count_t(count));
		}
	}

	op.ProjectResult(chunk, result);
	result.Verify(context.client.db);
}

void IEJoinLocalSourceState::ExecuteRightTask(ExecutionContext &context, DataChunk &result) {
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();
	const auto left_cols = op.children[0].get().GetTypes().size();

	auto &chunk = unprojected;

	const idx_t count = SelectOuterRows(right_matches);
	if (!count) {
		right_matches = nullptr;
		return;
	}

	auto &right_table = *ie_sink.tables[1];
	right_table.Repin(*right_iterator);
	op.SliceSortedPayload(rpayload, right_table, *right_iterator, right_chunk_state, right_block_index, outer_sel,
	                      *right_scan_state);

	// Fill in NULLs to the left
	chunk.Reset();
	for (column_t col_idx = 0; col_idx < chunk.ColumnCount(); ++col_idx) {
		if (col_idx < left_cols) {
			ConstantVector::SetNull(chunk.data[col_idx], count_t(count));
		} else {
			chunk.data[col_idx].Reference(rpayload.data[col_idx - left_cols]);
		}
	}

	op.ProjectResult(chunk, result);
	result.Verify(context.client.db);
}

void IEJoinLocalSourceState::ExecuteAntiTask(ExecutionContext &context, DataChunk &result) {
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();

	outer_sel.reserve(STANDARD_VECTOR_SIZE);
	outer_sel.resize(0);
	while (outer_idx < outer_count) {
		outer_sel.emplace_back(outer_idx++);
		if (outer_sel.size() >= STANDARD_VECTOR_SIZE) {
			break;
		}
	}
	const idx_t count = outer_sel.size();
	if (!count) {
		return;
	}

	auto &left_table = *ie_sink.tables[0];

	left_table.Repin(*left_iterator);
	op.SliceSortedPayload(result, left_table, *left_iterator, left_chunk_state, left_block_index, outer_sel,
	                      *left_scan_state);
	result.Verify(context.client.db);
}

void IEJoinLocalSourceState::ExecuteMarkTask(ExecutionContext &context, DataChunk &result) {
	auto &op = gsource.op;
	auto &ie_sink = op.sink_state->Cast<IEJoinGlobalState>();

	outer_sel.reserve(STANDARD_VECTOR_SIZE);
	outer_sel.resize(0);
	while (outer_idx < outer_count) {
		outer_sel.emplace_back(outer_idx++);
		if (outer_sel.size() >= STANDARD_VECTOR_SIZE) {
			break;
		}
	}
	const idx_t count = outer_sel.size();
	if (!count) {
		return;
	}

	auto &left_table = *ie_sink.tables[0];
	left_table.Repin(*left_iterator);
	op.SliceSortedPayload(lpayload, left_table, *left_iterator, left_chunk_state, left_block_index, outer_sel,
	                      *left_scan_state);

	// for the initial set of columns we just reference the left side
	result.SetChildCardinality(lpayload.size());
	for (idx_t i = 0; i < lpayload.ColumnCount(); i++) {
		result.data[i].Reference(lpayload.data[i]);
	}

	//	One of the two main keys is NULL, so the result is NULL
	auto &mark_vector = result.data.back();
	mark_vector.SetVectorType(VectorType::FLAT_VECTOR);
	auto &mask = FlatVector::ValidityMutable(mark_vector);
	mask.SetAllInvalid(count);

	result.Verify(context.client.db);
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalIEJoin::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	D_ASSERT(children.size() == 2);
	if (meta_pipeline.HasRecursiveCTE()) {
		throw NotImplementedException("IEJoins are not supported in recursive CTEs yet");
	}

	// becomes a source after both children fully sink their data
	meta_pipeline.GetState().SetPipelineSource(current, *this);

	// Create one child meta pipeline that will hold the LHS and RHS pipelines
	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);

	// Build out RHS first because that is the order the join planner expects.
	auto rhs_pipeline = child_meta_pipeline.GetBasePipeline();
	children[1].get().BuildPipelines(*rhs_pipeline, child_meta_pipeline);

	// Build out LHS
	auto &lhs_pipeline = child_meta_pipeline.CreatePipeline();
	children[0].get().BuildPipelines(lhs_pipeline, child_meta_pipeline);

	// Despite having the same sink, LHS and everything created after it need their own (same) PipelineFinishEvent
	child_meta_pipeline.AddFinishEvent(lhs_pipeline);
}

} // namespace duckdb
