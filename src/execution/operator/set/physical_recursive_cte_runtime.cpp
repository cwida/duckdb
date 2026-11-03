#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_union_state.hpp"

#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_blockwise_nl_join.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/execution/operator/join/physical_recursive_cte_key_join.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_complete_event.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/parallel/pipeline_finish_event.hpp"
#include "duckdb/parallel/pipeline_prepare_finish_event.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include "duckdb/common/atomic.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Recursive CTE Task and Event for optimized execution
//===--------------------------------------------------------------------===//
//
// The normal pipeline scheduler is optimized for one-shot query execution: build the query-wide
// event graph, allocate PipelineExecutors/tasks, run the pipelines once, then tear that runtime
// state down. Recursive CTEs repeatedly re-enter the recursive member, often with tiny frontiers,
// so paying that setup/teardown cost every iteration quickly dominates the actual data processing.
//
// The helpers below keep the same pipeline/finalize semantics, but split out a recursive-specific
// runtime that can reset and reuse executors, selected operator state, and the recursive dependency
// topology across iterations.

// Recursive iterations often produce small batches. Aim for about half a vector of input per worker
// before increasing parallelism so scheduler overhead stays low on narrow recursive workloads.
static constexpr const idx_t RECURSIVE_ROWS_PER_THREAD = STANDARD_VECTOR_SIZE / 2;

struct RecursiveCTEParallelism {
	explicit RecursiveCTEParallelism(idx_t worker_count)
	    : frontier_worker_count(worker_count), source_task_worker_count(worker_count) {
	}

	RecursiveCTEParallelism(idx_t frontier_worker_count_p, idx_t source_task_worker_count_p)
	    : frontier_worker_count(frontier_worker_count_p), source_task_worker_count(source_task_worker_count_p) {
	}

	idx_t WorkerCount(bool has_source_tasks) const {
		return has_source_tasks ? source_task_worker_count : frontier_worker_count;
	}

	idx_t MaxWorkerCount() const {
		return MaxValue(frontier_worker_count, source_task_worker_count);
	}

	bool IsInline() const {
		return MaxWorkerCount() == 1;
	}

	idx_t frontier_worker_count;
	idx_t source_task_worker_count;
};

static idx_t GetRecursiveWorkUnits(const RecursiveCTEState &state, idx_t direct_probe_work_units) {
	auto &op = state.GetOperator();
	auto &references = op.recursive_references;
	idx_t work_units = 0;
	if (op.working_table && references.frontier_scans > 0) {
		work_units += state.CurrentInputTable().ChunkCount() * references.frontier_scans;
	}
	if (op.recurring_table && references.recurring_scans > 0) {
		work_units += state.RecurringStateChunks() * references.recurring_scans;
	}
	// Direct probes consume their visible input without scanning the frozen recurring state. Recursive probe inputs
	// are counted above, while independent probe inputs contribute their estimated chunks.
	work_units = MaxValue(work_units, direct_probe_work_units);
	return MaxValue<idx_t>(work_units, 1);
}

static idx_t GetRecursiveInputRows(const RecursiveCTEState &state) {
	auto &op = state.GetOperator();
	auto &references = op.recursive_references;
	idx_t recursive_rows = 0;
	if (op.working_table && references.frontier_scans > 0) {
		recursive_rows += state.CurrentInputTable().Count() * references.frontier_scans;
	}
	if (op.recurring_table && references.recurring_scans > 0) {
		recursive_rows += state.RecurringStateRows() * references.recurring_scans;
	}
	return recursive_rows;
}

static idx_t GetRecursiveRowLimit(idx_t input_rows) {
	return MaxValue<idx_t>((input_rows + RECURSIVE_ROWS_PER_THREAD - 1) / RECURSIVE_ROWS_PER_THREAD, 1);
}

static RecursiveCTEParallelism GetRecursiveParallelism(const RecursiveCTEState &state, idx_t work_units,
                                                       idx_t row_limit,
                                                       const RecursiveCTEPipelineSchedulePlan &schedule_plan) {
	D_ASSERT(work_units > 1);
	D_ASSERT(row_limit > 1);
	D_ASSERT(schedule_plan.execute_pipeline_count > 0);
	auto &op = state.GetOperator();
	const auto configured_threads =
	    TaskScheduler::GetScheduler(op.recursive_meta_pipeline->GetExecutor().context).NumberOfThreads();
	const auto worker_count = MinValue(MinValue(row_limit, work_units), configured_threads);
	if (worker_count <= 1) {
		return RecursiveCTEParallelism(1);
	}
	if (!op.UsingKey() && op.UnionAll() && worker_count > 1) {
		// Ordinary UNION ALL needs enough work to amortize every scheduled pipeline and private-output combine.
		auto estimated_work_units = work_units;
		if (op.working_table && schedule_plan.cte_scan_pipeline_count > 0) {
			// A materialized CTE can fan one recursive frontier out into several scans. Count those scans as
			// scheduled work instead of treating all of their pipelines as pure scheduling overhead.
			estimated_work_units += state.CurrentInputTable().ChunkCount() * schedule_plan.cte_scan_pipeline_count;
		}
		D_ASSERT(schedule_plan.execute_pipeline_count >= schedule_plan.cte_scan_pipeline_count);
		// The scan pipelines contribute the fan-out work above, so only the remaining pipelines are fixed overhead.
		const auto fixed_pipeline_count = schedule_plan.execute_pipeline_count - schedule_plan.cte_scan_pipeline_count;
		const auto minimum_work_units_per_worker = MaxValue<idx_t>(fixed_pipeline_count, 2);
		const auto amortized_frontier_workers =
		    MaxValue<idx_t>(MinValue(worker_count, estimated_work_units / minimum_work_units_per_worker), 1);
		// Source tasks can expose work that is not represented by recursive frontier chunks.
		const auto source_task_workers = schedule_plan.has_source_tasks ? worker_count : amortized_frontier_workers;
		return RecursiveCTEParallelism(amortized_frontier_workers, source_task_workers);
	}
	return RecursiveCTEParallelism(worker_count);
}

//! Combined worker bound of all producer pipelines that feed one sink
struct RecursiveSinkWorkerBound {
	idx_t worker_bound = 0;
	idx_t producer_count = 0;
	//! Whether the per-iteration reset already ran for this sink; shared sinks must reset exactly once
	bool reset_done = false;
};

//! A sink with a single producer learns the exact worker count here, before any of its data flows
static void AdjustRecursiveSinkWorkers(Pipeline &pipeline, idx_t max_threads) {
	auto sink = pipeline.GetSink();
	if (!sink || !sink->sink_state || !sink->sink_state->reschedule_single_producer) {
		return;
	}
	sink->sink_state->AdjustRescheduledWorkers(pipeline.executor.context, max_threads);
}

//! Conservative upper bound on the workers the schedule can assign to one producer pipeline.
//! Only sources whose contents are already final for this iteration may refine the epoch limit.
static idx_t GetRecursiveProducerWorkerBound(Pipeline &pipeline, idx_t worker_limit) {
	auto source = pipeline.GetSource();
	if (!source || (source->type != PhysicalOperatorType::RECURSIVE_CTE_SCAN &&
	                source->type != PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN)) {
		return worker_limit;
	}
	auto &scan = source->Cast<PhysicalColumnDataScan>();
	if (!scan.collection) {
		return worker_limit;
	}
	// The scan hands out batches of at least STANDARD_VECTOR_SIZE rows, so this bounds its MaxThreads
	const auto count = scan.collection->Count();
	const auto scan_batches = MaxValue<idx_t>(count / STANDARD_VECTOR_SIZE + (count % STANDARD_VECTOR_SIZE != 0), 1);
	return MinValue(worker_limit, scan_batches);
}

static idx_t GetRecursivePipelineMaxThreads(Pipeline &pipeline, idx_t worker_limit) {
	auto max_threads = pipeline.GetMaxThreads();
	if (max_threads < 1) {
		max_threads = 1;
	}
	return MinValue(max_threads, worker_limit);
}

static void ExecuteRecursivePipelineInline(PipelineExecutor &pipeline_executor) {
	auto signal_state = make_shared_ptr<InterruptDoneSignalState>();
	pipeline_executor.SetInterruptState(InterruptState(weak_ptr<InterruptDoneSignalState>(signal_state)));
	while (true) {
		auto result = pipeline_executor.Execute();
		switch (result) {
		case PipelineExecuteResult::FINISHED:
			return;
		case PipelineExecuteResult::NOT_FINISHED:
			throw InternalException("Execute without limit should not return NOT_FINISHED");
		case PipelineExecuteResult::INTERRUPTED:
			signal_state->Await();
			break;
		}
	}
}

template <bool COLLECT_METRICS>
struct RecursiveCTETaskMetrics {
	RecursiveCTETaskMetrics(RecursiveCTEState &, RecursiveCTEPipelineMetricType) {
	}
};

template <>
struct RecursiveCTETaskMetrics<true> {
	RecursiveCTETaskMetrics(RecursiveCTEState &state_p, RecursiveCTEPipelineMetricType metric_type_p)
	    : state(state_p), metric_type(metric_type_p) {
	}

	RecursiveCTEState &state;
	RecursiveCTEPipelineMetricType metric_type;
};

//! A task that executes a cached PipelineExecutor
template <bool COLLECT_METRICS>
class RecursiveCTETask : public ExecutorTask, private RecursiveCTETaskMetrics<COLLECT_METRICS> {
public:
	RecursiveCTETask(Pipeline &pipeline_p, shared_ptr<Event> event_p, PipelineExecutor &executor_p,
	                 RecursiveCTEState &state_p, RecursiveCTEPipelineMetricType metric_type_p)
	    : ExecutorTask(pipeline_p.executor, std::move(event_p)), RecursiveCTETaskMetrics<COLLECT_METRICS>(
	                                                                 state_p, metric_type_p),
	      pipeline(pipeline_p), pipeline_executor(executor_p) {
	}

	Pipeline &pipeline;
	PipelineExecutor &pipeline_executor;

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		pipeline_executor.SetTaskForInterrupts(shared_from_this());
		const auto execute_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		const auto result = mode == TaskExecutionMode::PROCESS_PARTIAL ? pipeline_executor.Execute(PARTIAL_CHUNK_COUNT)
		                                                               : pipeline_executor.Execute();
		if constexpr (COLLECT_METRICS) {
			const auto execute_end = std::chrono::steady_clock::now();
			this->state.GetEpochMetrics().RecordPipelineExecution(
			    this->metric_type,
			    NumericCast<idx_t>(
			        std::chrono::duration_cast<std::chrono::nanoseconds>(execute_end - execute_start).count()));
		}
		switch (result) {
		case PipelineExecuteResult::NOT_FINISHED:
			if (mode != TaskExecutionMode::PROCESS_PARTIAL) {
				throw InternalException("Execute without limit should not return NOT_FINISHED");
			}
			return TaskExecutionResult::TASK_NOT_FINISHED;
		case PipelineExecuteResult::INTERRUPTED:
			return TaskExecutionResult::TASK_BLOCKED;
		case PipelineExecuteResult::FINISHED:
			break;
		}

		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

private:
	// Keep partial execution reasonably coarse so blocked pipelines make progress without creating tiny tasks.
	static constexpr const idx_t PARTIAL_CHUNK_COUNT = 50;
};

//! Recursive execute event.
//! We still use BasePipelineEvent for dependency tracking and task bookkeeping, but the stock
//! pipeline scheduling path is too expensive here: it assumes a one-shot execution, creates fresh
//! PipelineExecutors/tasks every time, and resets shared pipeline state immediately before launch.
//! Recursive CTEs need the same dependency semantics while reusing cached PipelineExecutors.
//! Operator state is invalidated once per iteration on the main thread; events lazily recreate
//! missing state through the ordinary initialization path once their dependencies have finished.
class RecursiveCTEPipelineEvent : public BasePipelineEvent {
public:
	RecursiveCTEPipelineEvent(shared_ptr<Pipeline> pipeline_p, RecursiveCTEState &state_p, idx_t worker_limit_p,
	                          RecursiveCTEPipelineMetricType metric_type_p)
	    : BasePipelineEvent(std::move(pipeline_p)), state(state_p), worker_limit(worker_limit_p),
	      metric_type(metric_type_p) {
	}

	RecursiveCTEState &state;
	idx_t worker_limit;
	RecursiveCTEPipelineMetricType metric_type;
	bool prepared_for_schedule = false;

	void PrepareForSchedule() {
		// Sink state was reset up-front on the main thread for the whole schedule. A root event
		// prepares its operator and source state before its own tasks launch; roots scheduled
		// earlier may already be executing, with shared state guarded by the operator lock.
		pipeline->InitializeOperatorState();
		pipeline->ResetSourceForReschedule();
		prepared_for_schedule = true;
	}

	void Schedule() override {
		// A dependent event schedules once all of its dependencies have finished, so dependency
		// outputs such as join builds and refilled CTE materializations are final here. Operator
		// state invalidated at iteration start is recreated through the shared create-if-absent
		// path: the first scheduling pipeline initializes shared state under the operator lock,
		// and competing pipelines observe the initialized state.
		if (!prepared_for_schedule) {
			pipeline->InitializeOperatorState();
			pipeline->ResetSourceForReschedule();
		}

		SchedulePrepared(GetRecursivePipelineMaxThreads(*pipeline, worker_limit));
	}

	// The caller has prepared the pipeline and determined its worker count.
	void SchedulePrepared(idx_t max_threads) {
		D_ASSERT(max_threads > 0);
		D_ASSERT(max_threads <= worker_limit);
		prepared_for_schedule = true;
		if (state.GetMetrics().Enabled()) {
			state.GetMetrics().RecordTasks(max_threads);
			state.GetEpochMetrics().RecordPipelineWorkers(max_threads);
		}
		AdjustRecursiveSinkWorkers(*pipeline, max_threads);
		state.GetScheduler().PrepareExecutors(*pipeline, max_threads);
		auto &executors = state.GetScheduler().GetExecutors(*pipeline);
		D_ASSERT(executors.size() >= max_threads);

		// Create tasks using cached executors
		vector<shared_ptr<Task>> tasks;
		for (idx_t i = 0; i < max_threads; i++) {
			executors[i]->PrepareForExecution();
			if (state.GetMetrics().Enabled()) {
				tasks.push_back(make_uniq<RecursiveCTETask<true>>(*pipeline, shared_from_this(), *executors[i], state,
				                                                  metric_type));
			} else {
				tasks.push_back(make_uniq<RecursiveCTETask<false>>(*pipeline, shared_from_this(), *executors[i], state,
				                                                   metric_type));
			}
		}
		SetTasks(std::move(tasks));
	}
};

//! Inline finish event for the single-thread recursive fast path.
//! PipelineFinishEvent is correct for the general scheduler, but it is intentionally scheduler-centric:
//! it expects finish work to go back through the task/event machinery. In the single-thread recursive
//! path that would mostly bounce control back into the scheduler just to run the finish work on the
//! current thread. This event preserves the same finalize contract, including BLOCKED handling, while
//! keeping the finish step inline with the cached execute/prepare stages.
class RecursiveCTEFinishEvent : public BasePipelineEvent {
public:
	explicit RecursiveCTEFinishEvent(shared_ptr<Pipeline> pipeline_p) : BasePipelineEvent(std::move(pipeline_p)) {
	}

	void Schedule() override {
		// This event is only used by the single-thread inline recursive fast path.
		// Blocking here is safe because the caller runs it synchronously and explicitly waits for completion.
		D_ASSERT(total_tasks == 0);
		total_tasks = 1;

		auto signal_state = make_shared_ptr<InterruptDoneSignalState>();
		InterruptState interrupt_state {weak_ptr<InterruptDoneSignalState>(signal_state)};
		auto sink = pipeline->GetSink();
		auto &operators = pipeline->GetIntermediateOperators();
		idx_t operator_idx = 0;

		while (true) {
			bool blocked = false;
			for (; operator_idx < operators.size(); operator_idx++) {
				auto &op = operators[operator_idx].get();
				if (!op.RequiresOperatorFinalize()) {
					continue;
				}
				OperatorFinalizeInput op_finalize_input {*op.op_state, interrupt_state};
				auto op_state = op.OperatorFinalize(*pipeline, *this, executor.context, op_finalize_input);
				if (op_state == OperatorFinalResultType::BLOCKED) {
					signal_state->Await();
					blocked = true;
					break;
				}
			}
			if (blocked) {
				continue;
			}

			OperatorSinkFinalizeInput finalize_input {*sink->sink_state, interrupt_state};
			auto sink_state = sink->Finalize(*pipeline, *this, executor.context, finalize_input);
			if (sink_state == SinkFinalizeType::BLOCKED) {
				signal_state->Await();
				continue;
			}

			sink->sink_state->state = sink_state;
			FinishTask();
			return;
		}
	}
};

static bool IsDirectRecursiveKeyProbe(const PhysicalOperator &op, TableIndex cte_index) {
	if (op.type != PhysicalOperatorType::RECURSIVE_KEY_JOIN) {
		return false;
	}
	auto &key_join = op.Cast<PhysicalRecursiveCTEKeyJoin>();
	return key_join.StateScan().cte_index == cte_index;
}

static unique_ptr<RecursiveCTEPipelineSchedulePlan>
BuildRecursivePipelineSchedulePlan(const vector<shared_ptr<MetaPipeline>> &meta_pipelines,
                                   const PhysicalRecursiveCTE &recursive_cte) {
	// The generic schedule is the single source of truth for pipeline lifecycle and dependencies.
	// Recursive execution projects out initialize/complete stages because it resets operator state before each
	// epoch and does not participate in the outer executor's query-completion accounting. Dependencies through
	// those stages are folded onto their nearest retained predecessors. The resulting topology is cached in two forms:
	// 1. a scheduler-free stage plan for the single-thread inline fast path
	// 2. a per-iteration Event graph for the multi-threaded path
	auto plan = make_uniq<RecursiveCTEPipelineSchedulePlan>();
	reference_map_t<const Pipeline, RecursiveCTEPipelineMetricType> pipeline_metric_types;
	reference_set_t<const Pipeline> invariant_build_pipelines;
	for (auto &meta_pipeline : meta_pipelines) {
		auto metric_type = RecursiveCTEPipelineMetricType::RECURSIVE;
		bool is_invariant_build = false;
		const auto is_invariant =
		    recursive_cte.invariant_meta_pipelines.find(*meta_pipeline) != recursive_cte.invariant_meta_pipelines.end();
		if (is_invariant) {
			auto sink = meta_pipeline->GetSink();
			is_invariant_build = !sink || sink->type != PhysicalOperatorType::CTE;
			metric_type = sink && sink->type == PhysicalOperatorType::CTE
			                  ? RecursiveCTEPipelineMetricType::INVARIANT_CTE_MATERIALIZATION
			                  : RecursiveCTEPipelineMetricType::INVARIANT_BUILD;
		}
		vector<shared_ptr<Pipeline>> pipelines;
		meta_pipeline->GetPipelines(pipelines, false);
		for (auto &pipeline : pipelines) {
			pipeline_metric_types.emplace(*pipeline, metric_type);
			if (is_invariant_build) {
				invariant_build_pipelines.insert(*pipeline);
			}
			plan->pipelines.emplace_back(*pipeline, is_invariant);
		}
	}
	// Retained invariant meta-pipelines are absent after the first epoch, so build the induced schedule for
	// the active subset and ignore dependencies whose other endpoint is already materialized.
	auto schedule = BuildPipelineSchedule(meta_pipelines, PipelineScheduleMode::ACTIVE_SUBSET);
	plan->stages.reserve(schedule->stages.size());
	reference_set_t<PhysicalOperator> invalidate_dedup;
	vector<idx_t> recursive_stage_indices(schedule->stages.size(), DConstants::INVALID_INDEX);
	for (idx_t stage_idx = 0; stage_idx < schedule->stages.size(); stage_idx++) {
		auto &stage = schedule->stages[stage_idx];
		if (stage.type == PipelineScheduleStageType::INITIALIZE || stage.type == PipelineScheduleStageType::COMPLETE) {
			continue;
		}
		auto &pipeline = *stage.pipeline;
		bool has_source_tasks = false;
		if (stage.type == PipelineScheduleStageType::EXECUTE) {
			plan->execute_pipeline_count++;
			auto source = pipeline.GetSource();
			if (source && source->type == PhysicalOperatorType::CTE_SCAN) {
				plan->cte_scan_pipeline_count++;
			}
			has_source_tasks = source && source->HasSourceTasks();
			if (has_source_tasks) {
				plan->has_source_tasks = true;
				plan->source_tasks_write_recursive_output =
				    plan->source_tasks_write_recursive_output || pipeline.GetSink().get() == &recursive_cte;
			}
			for (auto &op_ref : pipeline.GetIntermediateOperators()) {
				if (invalidate_dedup.insert(op_ref.get()).second) {
					plan->invalidate_operators.push_back(op_ref);
				}
			}
		}
		recursive_stage_indices[stage_idx] = plan->stages.size();
		auto metric_type = pipeline_metric_types.find(pipeline);
		D_ASSERT(metric_type != pipeline_metric_types.end());
		plan->stages.emplace_back(stage.type, pipeline, has_source_tasks, metric_type->second,
		                          invariant_build_pipelines.find(pipeline) != invariant_build_pipelines.end());
	}
	for (idx_t stage_idx = 0; stage_idx < schedule->stages.size(); stage_idx++) {
		const auto recursive_stage_idx = recursive_stage_indices[stage_idx];
		if (recursive_stage_idx == DConstants::INVALID_INDEX) {
			continue;
		}
		unordered_set<idx_t> visited_dependencies;
		unordered_set<idx_t> resolved_dependencies;
		vector<idx_t> pending_dependencies = schedule->stages[stage_idx].dependencies;
		while (!pending_dependencies.empty()) {
			const auto dependency = pending_dependencies.back();
			pending_dependencies.pop_back();
			if (!visited_dependencies.insert(dependency).second) {
				continue;
			}
			const auto recursive_dependency = recursive_stage_indices[dependency];
			if (recursive_dependency != DConstants::INVALID_INDEX) {
				resolved_dependencies.insert(recursive_dependency);
				continue;
			}
			for (auto transitive_dependency : schedule->stages[dependency].dependencies) {
				pending_dependencies.push_back(transitive_dependency);
			}
		}
		for (auto recursive_dependency : resolved_dependencies) {
			plan->stages[recursive_stage_idx].dependency_count++;
			plan->stages[recursive_dependency].dependents.push_back(recursive_stage_idx);
		}
	}
	for (auto &pipeline : schedule->initialize_on_schedule_pipelines) {
		plan->initialize_on_schedule_pipelines.push_back(pipeline);
	}
	return plan;
}

static bool ContainsVisibleRecursiveScanInternal(const PhysicalOperator &op, TableIndex cte_index,
                                                 reference_set_t<const PhysicalOperator> &visited) {
	if (!visited.insert(op).second) {
		return false;
	}
	if (op.type == PhysicalOperatorType::RECURSIVE_CTE_SCAN ||
	    op.type == PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN) {
		auto &scan = op.Cast<PhysicalColumnDataScan>();
		if (scan.cte_index == cte_index) {
			return true;
		}
	}
	for (auto &child : op.GetChildren()) {
		if (ContainsVisibleRecursiveScanInternal(child.get(), cte_index, visited)) {
			return true;
		}
	}
	return false;
}

static bool ContainsVisibleRecursiveScan(const PhysicalOperator &op, TableIndex cte_index) {
	reference_set_t<const PhysicalOperator> visited;
	return ContainsVisibleRecursiveScanInternal(op, cte_index, visited);
}

static void CountDirectRecursiveReferences(const PhysicalOperator &op, TableIndex cte_index,
                                           RecursiveCTEReferenceInfo &references) {
	if (IsDirectRecursiveKeyProbe(op, cte_index)) {
		auto &key_join = op.Cast<PhysicalRecursiveCTEKeyJoin>();
		if (key_join.Layout().IsPartial()) {
			references.partial_key_probes++;
		} else {
			references.exact_key_probes++;
		}
		auto &probe = key_join.children[0].get();
		if (!ContainsVisibleRecursiveScan(probe, cte_index)) {
			const auto probe_work_units = probe.estimated_cardinality / STANDARD_VECTOR_SIZE +
			                              (probe.estimated_cardinality % STANDARD_VECTOR_SIZE != 0);
			references.direct_probe_work_units = MaxValue(references.direct_probe_work_units, probe_work_units);
		}
		return;
	}
	if (op.type != PhysicalOperatorType::RECURSIVE_CTE_SCAN &&
	    op.type != PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN) {
		return;
	}
	auto &scan = op.Cast<PhysicalColumnDataScan>();
	if (scan.cte_index != cte_index) {
		return;
	}
	if (op.type == PhysicalOperatorType::RECURSIVE_CTE_SCAN) {
		references.frontier_scans++;
	} else {
		references.recurring_scans++;
	}
}

static bool OperatorDirectlyDependsOnRecursiveInput(const PhysicalOperator &op, TableIndex cte_index) {
	if (op.type == PhysicalOperatorType::DELIM_SCAN) {
		return true;
	}
	RecursiveCTEReferenceInfo references;
	CountDirectRecursiveReferences(op, cte_index, references);
	if (references.HasReferences()) {
		return true;
	}
	if (op.type == PhysicalOperatorType::LEFT_DELIM_JOIN || op.type == PhysicalOperatorType::RIGHT_DELIM_JOIN) {
		auto &delim_join = op.Cast<PhysicalDelimJoin>();
		for (auto &scan : delim_join.delim_scans) {
			if (OperatorDirectlyDependsOnRecursiveInput(scan.get(), cte_index)) {
				return true;
			}
		}
	}
	return false;
}

static bool PipelineDirectlyDependsOnRecursiveInput(Pipeline &pipeline, TableIndex cte_index) {
	auto source = pipeline.GetSource();
	if (source && OperatorDirectlyDependsOnRecursiveInput(*source, cte_index)) {
		return true;
	}
	for (auto &op : pipeline.GetIntermediateOperators()) {
		if (OperatorDirectlyDependsOnRecursiveInput(op.get(), cte_index)) {
			return true;
		}
	}
	return false;
}

static bool PipelineIsRepeatable(Pipeline &pipeline,
                                 const reference_set_t<const PhysicalOperator> &non_repeatable_operators) {
	auto source = pipeline.GetSource();
	if (source && non_repeatable_operators.find(*source) != non_repeatable_operators.end()) {
		return false;
	}
	for (auto &op : pipeline.GetIntermediateOperators()) {
		if (non_repeatable_operators.find(op) != non_repeatable_operators.end()) {
			return false;
		}
	}
	auto sink = pipeline.GetSink();
	return !sink || non_repeatable_operators.find(*sink) == non_repeatable_operators.end();
}

static reference_set_t<const MetaPipeline>
FindInvariantRecursiveMetaPipelines(const vector<shared_ptr<MetaPipeline>> &meta_pipelines, TableIndex cte_index,
                                    const reference_set_t<const PhysicalOperator> &non_repeatable_operators) {
	// By default the recursive executor reruns every meta-pipeline in the recursive member each
	// iteration because the generic scheduling infrastructure does not know which build-side subplans
	// are independent of the current recursive frontier. For small recursive workloads that repeats
	// expensive setup for static subplans. Classify only JOIN_BUILD meta-pipelines that are
	// transitively independent from the recursive input and whose build side does not propagate rows
	// back into the recursive result, so their materialized state can safely survive later iterations.
	reference_map_t<const Pipeline, reference<const MetaPipeline>> pipeline_to_meta_pipeline;
	reference_set_t<const MetaPipeline> variant_meta_pipelines;

	for (auto &meta_pipeline : meta_pipelines) {
		vector<shared_ptr<Pipeline>> pipelines;
		meta_pipeline->GetPipelines(pipelines, false);
		for (auto &pipeline : pipelines) {
			pipeline_to_meta_pipeline.emplace(*pipeline, *meta_pipeline);
			if (PipelineDirectlyDependsOnRecursiveInput(*pipeline, cte_index) ||
			    !PipelineIsRepeatable(*pipeline, non_repeatable_operators)) {
				variant_meta_pipelines.insert(*meta_pipeline);
			}
		}
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto &meta_pipeline : meta_pipelines) {
			if (variant_meta_pipelines.find(*meta_pipeline) != variant_meta_pipelines.end()) {
				continue;
			}

			bool depends_on_variant = false;
			vector<shared_ptr<Pipeline>> pipelines;
			meta_pipeline->GetPipelines(pipelines, false);
			for (auto &pipeline : pipelines) {
				for (auto &dependency : pipeline->GetAllDependencies()) {
					auto dep_entry = pipeline_to_meta_pipeline.find(*dependency);
					if (dep_entry == pipeline_to_meta_pipeline.end()) {
						continue;
					}
					if (variant_meta_pipelines.find(dep_entry->second) != variant_meta_pipelines.end()) {
						depends_on_variant = true;
						break;
					}
				}
				if (depends_on_variant) {
					break;
				}
			}
			if (depends_on_variant) {
				variant_meta_pipelines.insert(*meta_pipeline);
				changed = true;
			}
		}
	}

	reference_set_t<const MetaPipeline> result;
	for (auto &meta_pipeline : meta_pipelines) {
		auto sink = meta_pipeline->GetSink();
		if (!sink) {
			continue;
		}

		bool can_cache_build = false;
		switch (sink->type) {
		case PhysicalOperatorType::CTE: {
			auto &cte = sink->Cast<PhysicalCTE>();
			can_cache_build = meta_pipeline->Type() == MetaPipelineType::REGULAR && !cte.cte_body_has_side_effects &&
			                  cte.GetExecutionMode() == CTEExecutionMode::MATERIALIZED;
			break;
		}
		case PhysicalOperatorType::HASH_JOIN: {
			if (meta_pipeline->Type() != MetaPipelineType::JOIN_BUILD) {
				break;
			}
			auto &hash_join = sink->Cast<PhysicalHashJoin>();
			can_cache_build = !PropagatesBuildSide(hash_join.join_type);
			break;
		}
		case PhysicalOperatorType::NESTED_LOOP_JOIN: {
			if (meta_pipeline->Type() != MetaPipelineType::JOIN_BUILD) {
				break;
			}
			auto &nested_loop_join = sink->Cast<PhysicalNestedLoopJoin>();
			can_cache_build = !PropagatesBuildSide(nested_loop_join.join_type);
			break;
		}
		case PhysicalOperatorType::BLOCKWISE_NL_JOIN: {
			if (meta_pipeline->Type() != MetaPipelineType::JOIN_BUILD) {
				break;
			}
			auto &blockwise_nl_join = sink->Cast<PhysicalBlockwiseNLJoin>();
			can_cache_build = !PropagatesBuildSide(blockwise_nl_join.join_type);
			break;
		}
		case PhysicalOperatorType::CROSS_PRODUCT:
			can_cache_build = meta_pipeline->Type() == MetaPipelineType::JOIN_BUILD;
			break;
		default:
			break;
		}
		if (!can_cache_build) {
			continue;
		}
		if (variant_meta_pipelines.find(*meta_pipeline) == variant_meta_pipelines.end()) {
			result.insert(*meta_pipeline);
		}
	}
	return result;
}

static void ConfigureInvariantRecursiveBuildReuse(const PhysicalRecursiveCTE &op, bool preserve_build) {
	for (auto &meta_pipeline_ref : op.invariant_meta_pipelines) {
		auto sink = meta_pipeline_ref.get().GetSink();
		if (!sink || sink->type != PhysicalOperatorType::HASH_JOIN) {
			continue;
		}
		sink->Cast<PhysicalHashJoin>().SetPreserveBuildForRecursiveReuse(preserve_build);
	}
}

static bool InvariantRecursiveBuildsRemainReusable(const PhysicalRecursiveCTE &op) {
	// Recursive invariant caching may omit these meta-pipelines entirely after the first materialization.
	// That is only safe if every invariant HASH_JOIN actually kept a reusable in-memory build side.
	// External/spilled hash joins run through mutable partition/probe-spill rounds and must therefore
	// stay in the active recursive schedule instead of being treated as "materialized once".
	for (auto &meta_pipeline_ref : op.invariant_meta_pipelines) {
		auto sink = meta_pipeline_ref.get().GetSink();
		if (!sink || sink->type != PhysicalOperatorType::HASH_JOIN) {
			continue;
		}
		if (!sink->Cast<PhysicalHashJoin>().CanPreserveBuildForRecursiveReuse()) {
			return false;
		}
	}
	return true;
}

static void ProcessRecursiveExecutorTasks(Executor &executor) {
	// Only drain the recursive executor here. Re-entering the outer query executor while waiting for
	// recursive work can run unrelated tasks and used to break recursive completion tracking.
	if (!executor.WorkOnTasks()) {
		executor.WaitForTask();
	}
	if (executor.HasError()) {
		executor.ThrowException();
	}
}

static void WaitForRecursiveEvents(Executor &executor, vector<shared_ptr<Event>> &events) {
	while (true) {
		ProcessRecursiveExecutorTasks(executor);
		bool finished = true;
		for (auto &event : events) {
			if (!event->IsFinished()) {
				finished = false;
				break;
			}
		}
		if (finished) {
			break;
		}
	}
}

static void WaitForRecursiveEvent(Executor &executor, Event &event) {
	while (!event.IsFinished()) {
		ProcessRecursiveExecutorTasks(executor);
	}
}

static void ScheduleRecursivePlan(const RecursiveCTEPipelineSchedulePlan &plan, RecursiveCTEState &state,
                                  vector<shared_ptr<Event>> &events, const RecursiveCTEParallelism &parallelism) {
	for (auto &pipeline : plan.initialize_on_schedule_pipelines) {
		pipeline.get().ResetSource(true);
	}

	const auto configured_threads =
	    TaskScheduler::GetScheduler(state.GetOperator().recursive_meta_pipeline->GetExecutor().context)
	        .NumberOfThreads();
	events.reserve(plan.stages.size());
	for (auto &stage : plan.stages) {
		auto pipeline = stage.pipeline.get().shared_from_this();
		switch (stage.type) {
		case PipelineScheduleStageType::EXECUTE:
			events.push_back(make_shared_ptr<RecursiveCTEPipelineEvent>(
			    std::move(pipeline), state,
			    stage.is_invariant_build ? configured_threads : parallelism.WorkerCount(stage.has_source_tasks),
			    stage.metric_type));
			break;
		case PipelineScheduleStageType::PREPARE_FINISH:
			events.push_back(make_shared_ptr<PipelinePrepareFinishEvent>(std::move(pipeline)));
			break;
		case PipelineScheduleStageType::FINISH:
			events.push_back(make_shared_ptr<PipelineFinishEvent>(std::move(pipeline)));
			break;
		default:
			throw InternalException("Unsupported recursive schedule stage");
		}
	}

	for (idx_t stage_idx = 0; stage_idx < plan.stages.size(); stage_idx++) {
		for (auto dependent_stage : plan.stages[stage_idx].dependents) {
			events[dependent_stage]->AddDependency(*events[stage_idx]);
		}
	}

	for (idx_t stage_idx = 0; stage_idx < plan.stages.size(); stage_idx++) {
		auto &event = events[stage_idx];
		if (event->HasDependencies()) {
			continue;
		}
		if (plan.stages[stage_idx].type == PipelineScheduleStageType::EXECUTE) {
			event->Cast<RecursiveCTEPipelineEvent>().PrepareForSchedule();
		}
		event->Schedule();
		if (!event->HasTasks() && !event->IsFinished() && event->AutoFinishWithoutTasks()) {
			event->Finish();
		}
	}
}

static void ExecuteRecursivePipelineFinishInline(Pipeline &pipeline, Executor &executor) {
	auto finish_event = make_shared_ptr<RecursiveCTEFinishEvent>(pipeline.shared_from_this());
	auto complete_event = make_shared_ptr<PipelineCompleteEvent>(executor, false);
	complete_event->AddDependency(*finish_event);
	finish_event->Schedule();
	if (!complete_event->IsFinished()) {
		WaitForRecursiveEvent(executor, *complete_event);
	}
}

template <bool COLLECT_METRICS>
static void ExecuteRecursiveInlinePlan(RecursiveCTEState &state, Executor &executor,
                                       const RecursiveCTEPipelineSchedulePlan &plan) {
	for (auto &pipeline : plan.initialize_on_schedule_pipelines) {
		pipeline.get().ResetSource(true);
	}

	state.GetScheduler().InitializeInlinePlan(plan);

	for (idx_t ready_idx = 0; ready_idx < state.GetScheduler().ReadyStageCount(); ready_idx++) {
		auto stage_idx = state.GetScheduler().ReadyStage(ready_idx);
		auto &stage = plan.stages[stage_idx];
		auto &pipeline = stage.pipeline.get();
		switch (stage.type) {
		case PipelineScheduleStageType::EXECUTE: {
			// Stages become ready in dependency order, so missing operator state is created here
			// through the same initialization path the ordinary scheduler and the event schedule
			// use; state shared with an earlier stage is already initialized and kept.
			pipeline.InitializeOperatorState();
			pipeline.ResetSourceForReschedule();
			// Invariant builds have independent source work even when the recursive frontier is tiny.
			const auto worker_limit =
			    stage.is_invariant_build ? TaskScheduler::GetScheduler(executor.context).NumberOfThreads() : idx_t(1);
			const auto max_threads = GetRecursivePipelineMaxThreads(pipeline, worker_limit);
			if (max_threads > 1) {
				auto event = make_shared_ptr<RecursiveCTEPipelineEvent>(pipeline.shared_from_this(), state,
				                                                        worker_limit, stage.metric_type);
				event->SchedulePrepared(max_threads);
				WaitForRecursiveEvent(executor, *event);
				break;
			}
			D_ASSERT(max_threads == 1);
			AdjustRecursiveSinkWorkers(pipeline, max_threads);
			state.GetScheduler().PrepareExecutors(pipeline, max_threads);
			auto &executors = state.GetScheduler().GetExecutors(pipeline);
			D_ASSERT(executors.size() >= max_threads);
			executors[0]->PrepareForExecution();
			if constexpr (COLLECT_METRICS) {
				state.GetMetrics().RecordTasks(1);
			}
			const auto execute_start =
			    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			ExecuteRecursivePipelineInline(*executors[0]);
			if constexpr (COLLECT_METRICS) {
				const auto execute_end = std::chrono::steady_clock::now();
				state.GetEpochMetrics().RecordPipelineExecution(
				    stage.metric_type,
				    NumericCast<idx_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(execute_end - execute_start).count()));
			}
			break;
		}
		case PipelineScheduleStageType::PREPARE_FINISH:
			pipeline.PrepareFinalize();
			break;
		case PipelineScheduleStageType::FINISH:
			ExecuteRecursivePipelineFinishInline(pipeline, executor);
			break;
		default:
			throw InternalException("Unsupported recursive inline stage");
		}

		state.GetScheduler().CompleteInlineStage(plan, stage_idx);
	}

	if (state.GetScheduler().ReadyStageCount() != plan.stages.size()) {
		throw InternalException("Recursive inline plan did not schedule every stage");
	}
}

void PhysicalRecursiveCTE::ExecuteRecursivePipelines(ExecutionContext &context) const {
	if (!recursive_meta_pipeline) {
		throw InternalException("Missing meta pipeline for recursive CTE");
	}
	D_ASSERT(recursive_meta_pipeline->HasRecursiveCTE());

	auto &gstate = sink_state->Cast<RecursiveCTEState>();
	auto &executor = recursive_meta_pipeline->GetExecutor();
	const auto allow_reuse = gstate.AllowsExecutorReuse();
	const auto collect_metrics = gstate.GetMetrics().Enabled();
	auto can_cache_invariant_meta_pipelines = allow_reuse && !invariant_meta_pipelines.empty();
	if (collect_metrics && gstate.HasMaterializedInvariantPipelines()) {
		for (auto &meta_pipeline : invariant_meta_pipelines) {
			auto sink = meta_pipeline.get().GetSink();
			if (sink && sink->type == PhysicalOperatorType::CTE) {
				gstate.GetMetrics().RecordRetainedCTEReuse();
			}
		}
	}

	// The generic executor path would rebuild the recursive event graph and allocate fresh
	// PipelineExecutors/tasks every iteration. Recursive execution keeps the already-built recursive
	// MetaPipeline, resets only the state that must change, optionally skips invariant build pipelines
	// after they have been materialized once, and then picks between:
	// - a cached inline dependency plan when the iteration is effectively single-threaded
	// - a custom recursive Event graph when the iteration still benefits from parallel execution

	const auto &schedule_plan =
	    gstate.HasMaterializedInvariantPipelines() ? invariant_recursive_schedule_plan : recursive_schedule_plan;
	if (!schedule_plan) {
		throw InternalException("Missing recursive pipeline schedule");
	}

	const auto scheduler_input_rows = GetRecursiveInputRows(gstate);
	const auto direct_probe_work_units = recursive_references.direct_probe_work_units;
	const auto row_limit = MaxValue(GetRecursiveRowLimit(scheduler_input_rows), direct_probe_work_units);
	idx_t work_units = 1;
	RecursiveCTEParallelism parallelism(1);
	if (row_limit > 1) {
		work_units = GetRecursiveWorkUnits(gstate, direct_probe_work_units);
		if (work_units > 1) {
			parallelism = GetRecursiveParallelism(gstate, work_units, row_limit, *schedule_plan);
		}
	}

	// Establish a conservative per-sink worker bound before any sink state is mutated: several
	// producer pipelines can share one sink, so the bound is the maximum over all of its producers.
	// A producer's bound defaults to its epoch worker limit, where invariant meta pipelines keep the
	// configured thread count because their source work is independent of the recursive frontier.
	// Frontier and recurring-table contents are final for this iteration at this point, so a producer
	// scanning them refines its bound to that scan's parallelism. Any other source, such as a
	// materialized CTE that this iteration refills, stays at the epoch limit.
	const auto configured_threads = TaskScheduler::GetScheduler(executor.context).NumberOfThreads();
	reference_map_t<const PhysicalOperator, RecursiveSinkWorkerBound> sink_worker_bounds;
	for (auto &entry : schedule_plan->pipelines) {
		auto &pipeline = entry.pipeline.get();
		auto sink = pipeline.GetSink();
		if (sink.get() != this) {
			const auto worker_limit = entry.is_invariant ? configured_threads : parallelism.MaxWorkerCount();
			auto &sink_bound = sink_worker_bounds[*sink];
			sink_bound.worker_bound =
			    MaxValue(sink_bound.worker_bound, GetRecursiveProducerWorkerBound(pipeline, worker_limit));
			sink_bound.producer_count++;
		}
	}

	// Reset sink state from the main thread so recursive iterations can reuse or recreate
	// pipeline-local global sinks without tearing down the rest of the runtime state graph. A sink
	// with a single producer additionally learns the exact worker count at schedule time.
	for (auto &entry : schedule_plan->pipelines) {
		auto &pipeline = entry.pipeline.get();
		auto sink = pipeline.GetSink();
		if (sink.get() != this) {
			auto &sink_bound = sink_worker_bounds[*sink];
			if (sink_bound.reset_done) {
				// several producer pipelines feed this sink; it was already reset for this iteration
				continue;
			}
			sink_bound.reset_done = true;
			pipeline.ResetSinkForReschedule(sink_bound.worker_bound, sink_bound.producer_count == 1);
		}
	}

	// Invalidate global operator state from the main thread while nothing executes: reusable
	// state is reset in place, everything else is destroyed. Both recursive schedules recreate
	// missing state through the ordinary create-if-absent initialization once a pipeline's
	// dependencies have finished, because state construction may consume dependency-produced
	// sink state. The plan's deduplicated list visits each shared operator exactly once.
	for (auto &op_ref : schedule_plan->invalidate_operators) {
		Pipeline::InvalidateOperatorStateForReschedule(context.client, op_ref.get());
	}

	ConfigureInvariantRecursiveBuildReuse(*this, can_cache_invariant_meta_pipelines);

	if (!allow_reuse) {
		gstate.GetScheduler().ClearExecutors();
	}

	const auto prepare_cached_executor_entries = !allow_reuse || !gstate.GetScheduler().HasExecutorEntries();

	// Materialize every state-local cache entry before events can run concurrently. Individual
	// pipelines grow their stable executor vector after their source state has been reset. Reusable
	// entries only need to be registered once for each recursive state.
	if (prepare_cached_executor_entries) {
		reference_set_t<Pipeline> prepared_pipelines;
		for (auto &entry : schedule_plan->pipelines) {
			if (prepared_pipelines.insert(entry.pipeline.get()).second) {
				gstate.GetScheduler().PrepareExecutorEntry(entry.pipeline.get());
			}
		}
	}
	idx_t frontier_rows = 0;
	idx_t frontier_chunks = 0;
	idx_t frontier_storage_bytes = 0;
	idx_t frontier_allocation_bytes = 0;
	idx_t epoch_task_start = 0;
	if (collect_metrics) {
		const auto &frontier = static_cast<const RecursiveCTEState &>(gstate).CurrentInputTable();
		frontier_rows = frontier.Count();
		frontier_chunks = frontier.ChunkCount();
		frontier_storage_bytes = frontier.SizeInBytes();
		frontier_allocation_bytes = frontier.AllocationSize();
		epoch_task_start = gstate.GetMetrics().TaskCount();
	}
	if (!UsingKey() && UnionAll()) {
		auto &union_state = gstate.Cast<RecursiveCTEUnionState>();
		const auto source_tasks_write_recursive_output = parallelism.frontier_worker_count == 1 &&
		                                                 parallelism.source_task_worker_count > 1 &&
		                                                 schedule_plan->source_tasks_write_recursive_output;
		// Materialized fan-out already provides pipeline concurrency; private output multiplies collections per scan.
		const auto has_materialized_fanout = schedule_plan->cte_scan_pipeline_count > 1;
		union_state.SetUseLocalUnionAllOutput(source_tasks_write_recursive_output ||
		                                      (parallelism.frontier_worker_count > 1 && !has_materialized_fanout &&
		                                       work_units / parallelism.frontier_worker_count >= 2));
		if (!union_state.UsesLocalUnionAllOutput()) {
			union_state.InitializeSharedOutputAppend();
		}
	}
	const auto max_worker_count = parallelism.MaxWorkerCount();
	if (max_worker_count > 1 && !UsingKey() && !UnionAll()) {
		const auto partition_count = MinValue<idx_t>(NextPowerOfTwo(max_worker_count), 4);
		gstate.Cast<RecursiveCTEUnionState>().PromoteDistinctState(context.client, partition_count);
	}
	const auto epoch_start =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	auto inline_execution = allow_reuse && parallelism.IsInline();

	if (inline_execution) {
		if (collect_metrics) {
			ExecuteRecursiveInlinePlan<true>(gstate, executor, *schedule_plan);
		} else {
			ExecuteRecursiveInlinePlan<false>(gstate, executor, *schedule_plan);
		}
	} else {
		vector<shared_ptr<Event>> events;
		ScheduleRecursivePlan(*schedule_plan, gstate, events, parallelism);
		WaitForRecursiveEvents(executor, events);
	}

	if (collect_metrics) {
		const auto epoch_end = std::chrono::steady_clock::now();
		const auto elapsed_us =
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::microseconds>(epoch_end - epoch_start).count());
		gstate.GetMetrics().RecordEpoch(max_worker_count, elapsed_us, frontier_rows, frontier_chunks,
		                                scheduler_input_rows);
		const auto epoch_tasks = gstate.GetMetrics().TaskCount() - epoch_task_start;
		gstate.GetEpochMetrics().Record(frontier_rows, max_worker_count, epoch_tasks, elapsed_us,
		                                frontier_storage_bytes, frontier_allocation_bytes);
	}
	if (can_cache_invariant_meta_pipelines && InvariantRecursiveBuildsRemainReusable(*this)) {
		if (collect_metrics && !gstate.HasMaterializedInvariantPipelines()) {
			for (auto &meta_pipeline : invariant_meta_pipelines) {
				auto sink = meta_pipeline.get().GetSink();
				if (sink && sink->type == PhysicalOperatorType::CTE) {
					gstate.GetMetrics().RecordRetainedCTEMaterialization();
				} else {
					gstate.GetMetrics().RecordRetainedBuild();
				}
			}
		}
		gstate.MarkInvariantPipelinesMaterialized();
	}
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//

static void GatherColumnDataScans(const PhysicalOperator &op, vector<const_reference<PhysicalOperator>> &delim_scans) {
	if (op.type == PhysicalOperatorType::DELIM_SCAN || op.type == PhysicalOperatorType::CTE_SCAN) {
		delim_scans.push_back(op);
	}
	for (auto child : op.GetChildren()) {
		GatherColumnDataScans(child.get(), delim_scans);
	}
}

static void GatherRecursiveScansInternal(PhysicalOperator &op, TableIndex cte_index,
                                         vector<reference<PhysicalColumnDataScan>> &recursive_scans,
                                         reference_set_t<const PhysicalOperator> &visited) {
	if (!visited.insert(op).second) {
		return;
	}
	if (op.type == PhysicalOperatorType::RECURSIVE_CTE_SCAN) {
		auto &scan = op.Cast<PhysicalColumnDataScan>();
		if (scan.cte_index == cte_index) {
			recursive_scans.push_back(scan);
		}
	}
	for (auto &child : op.children) {
		GatherRecursiveScansInternal(child.get(), cte_index, recursive_scans, visited);
	}
	if (op.type == PhysicalOperatorType::LEFT_DELIM_JOIN || op.type == PhysicalOperatorType::RIGHT_DELIM_JOIN) {
		auto &delim_join = op.Cast<PhysicalDelimJoin>();
		GatherRecursiveScansInternal(delim_join.join, cte_index, recursive_scans, visited);
		GatherRecursiveScansInternal(delim_join.distinct, cte_index, recursive_scans, visited);
	}
}

static void GatherRecursiveScans(PhysicalOperator &op, TableIndex cte_index,
                                 vector<reference<PhysicalColumnDataScan>> &recursive_scans) {
	reference_set_t<const PhysicalOperator> visited;
	GatherRecursiveScansInternal(op, cte_index, recursive_scans, visited);
}

static void CountRecursiveReferencesInternal(const PhysicalOperator &op, TableIndex cte_index,
                                             RecursiveCTEReferenceInfo &references,
                                             reference_set_t<const PhysicalOperator> &visited) {
	if (!visited.insert(op).second) {
		return;
	}
	CountDirectRecursiveReferences(op, cte_index, references);
	for (auto child : op.GetChildren()) {
		CountRecursiveReferencesInternal(child.get(), cte_index, references, visited);
	}
}

static RecursiveCTEReferenceInfo CountRecursiveReferences(const PhysicalOperator &op, TableIndex cte_index) {
	reference_set_t<const PhysicalOperator> visited;
	RecursiveCTEReferenceInfo result;
	CountRecursiveReferencesInternal(op, cte_index, result, visited);
	return result;
}

void PhysicalRecursiveCTE::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();
	recursive_schedule_plan.reset();
	invariant_recursive_schedule_plan.reset();
	recursive_meta_pipeline.reset();
	{
		D_ASSERT(shared_executor_pool);
		lock_guard<mutex> guard(shared_executor_pool->lock);
		shared_executor_pool->executors.clear();
	}
	recursive_references = RecursiveCTEReferenceInfo();
	recursive_scans.clear();
	invariant_meta_pipelines.clear();

	auto &state = meta_pipeline.GetState();
	state.SetPipelineSource(current, *this);

	auto &executor = meta_pipeline.GetExecutor();
	executor.AddRecursiveCTE(*this);

	// the LHS of the recursive CTE is our initial state
	auto &initial_state_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	initial_state_pipeline.Build(children[0]);

	// the RHS is the recursive pipeline
	recursive_meta_pipeline = make_shared_ptr<MetaPipeline>(executor, state, this);
	recursive_meta_pipeline->SetRecursiveCTE();
	recursive_meta_pipeline->Build(children[1]);
	recursive_references = CountRecursiveReferences(children[1], table_index);
	GatherRecursiveScans(children[1], table_index, recursive_scans);

	vector<const_reference<PhysicalOperator>> ops;
	GatherColumnDataScans(children[1], ops);
	bool has_delim_scan = false;
	for (auto op : ops) {
		if (op.get().type == PhysicalOperatorType::DELIM_SCAN) {
			has_delim_scan = true;
			break;
		}
	}
	vector<shared_ptr<MetaPipeline>> recursive_meta_pipelines;
	recursive_meta_pipeline->GetMetaPipelines(recursive_meta_pipelines, true, false);
	if (!has_delim_scan) {
		invariant_meta_pipelines =
		    FindInvariantRecursiveMetaPipelines(recursive_meta_pipelines, table_index, non_repeatable_operators);
	}

	recursive_schedule_plan = BuildRecursivePipelineSchedulePlan(recursive_meta_pipelines, *this);
	if (!invariant_meta_pipelines.empty()) {
		vector<shared_ptr<MetaPipeline>> active_meta_pipelines;
		active_meta_pipelines.reserve(recursive_meta_pipelines.size() - invariant_meta_pipelines.size());
		for (auto &meta_pipeline_entry : recursive_meta_pipelines) {
			if (invariant_meta_pipelines.find(*meta_pipeline_entry) == invariant_meta_pipelines.end()) {
				active_meta_pipelines.push_back(meta_pipeline_entry);
			}
		}
		invariant_recursive_schedule_plan = BuildRecursivePipelineSchedulePlan(active_meta_pipelines, *this);
	}

	for (auto op : ops) {
		auto entry = state.cte_dependencies.find(op);
		if (entry == state.cte_dependencies.end()) {
			continue;
		}
		// this chunk scan introduces a dependency to the current pipeline
		// namely a dependency on the CTE pipeline to finish
		auto cte_dependency = entry->second.get().shared_from_this();
		current.AddDependency(cte_dependency);
	}
}

} // namespace duckdb
