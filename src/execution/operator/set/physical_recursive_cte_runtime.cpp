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
		RecordInvariantRetentionReuse(gstate.GetMetrics(), invariant_meta_pipelines);
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

	ConfigureInvariantRecursiveBuildReuse(invariant_meta_pipelines, can_cache_invariant_meta_pipelines);

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

	RecursiveSchedulingContext scheduling_context {executor, gstate.GetScheduler(),
	                                               collect_metrics ? &gstate.GetMetrics() : nullptr,
	                                               collect_metrics ? &gstate.GetEpochMetrics() : nullptr};
	if (inline_execution) {
		ExecuteRecursiveInlinePlan(scheduling_context, *schedule_plan);
	} else {
		scheduling_context.completion_signal = make_shared_ptr<RecursiveInterruptSignalState>();
		vector<shared_ptr<Event>> events;
		ScheduleRecursivePlan(*schedule_plan, scheduling_context, events, parallelism);
		WaitForRecursiveEvents(executor, events, scheduling_context.completion_signal.get());
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
	if (can_cache_invariant_meta_pipelines && InvariantRecursiveBuildsRemainReusable(invariant_meta_pipelines)) {
		if (collect_metrics && !gstate.HasMaterializedInvariantPipelines()) {
			RecordInvariantRetentionTransition(gstate.GetMetrics(), invariant_meta_pipelines);
		}
		gstate.MarkInvariantPipelinesMaterialized();
	}
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//

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

	recursive_schedule_plan =
	    BuildRecursivePipelineSchedulePlan(recursive_meta_pipelines, invariant_meta_pipelines, *this);
	if (!invariant_meta_pipelines.empty()) {
		vector<shared_ptr<MetaPipeline>> active_meta_pipelines;
		active_meta_pipelines.reserve(recursive_meta_pipelines.size() - invariant_meta_pipelines.size());
		for (auto &meta_pipeline_entry : recursive_meta_pipelines) {
			if (invariant_meta_pipelines.find(*meta_pipeline_entry) == invariant_meta_pipelines.end()) {
				active_meta_pipelines.push_back(meta_pipeline_entry);
			}
		}
		invariant_recursive_schedule_plan =
		    BuildRecursivePipelineSchedulePlan(active_meta_pipelines, invariant_meta_pipelines, *this);
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
