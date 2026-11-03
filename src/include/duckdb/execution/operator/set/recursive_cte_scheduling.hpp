//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/recursive_cte_scheduling.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/table_index.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"

namespace duckdb {

class MetaPipeline;
class PhysicalColumnDataScan;
class PipelineExecutor;
class RecursiveCTEMetrics;
struct RecursiveCTEEpochMetrics;

//! Pipeline executors cached across epochs so re-executions reuse their operator state
using recursive_executor_cache_t = reference_map_t<Pipeline, vector<unique_ptr<PipelineExecutor>>>;

struct RecursiveExecutorPool {
	mutex lock;
	recursive_executor_cache_t executors;
};

enum class RecursiveCTEPipelineMetricType : uint8_t { RECURSIVE, INVARIANT_BUILD, INVARIANT_CTE_MATERIALIZATION };

struct RecursiveCTEScheduleStage {
	RecursiveCTEScheduleStage(PipelineScheduleStageType type_p, Pipeline &pipeline_p, bool has_source_tasks_p,
	                          bool has_parallel_source_p, RecursiveCTEPipelineMetricType metric_type_p,
	                          bool is_invariant_build_p)
	    : type(type_p), pipeline(pipeline_p), has_source_tasks(has_source_tasks_p),
	      has_parallel_source(has_parallel_source_p), is_invariant_build(is_invariant_build_p),
	      metric_type(metric_type_p), dependency_count(0) {
	}

	PipelineScheduleStageType type;
	reference<Pipeline> pipeline;
	bool has_source_tasks;
	//! The source parallelizes independently of recursive inputs (e.g. a table scan)
	bool has_parallel_source;
	bool is_invariant_build;
	RecursiveCTEPipelineMetricType metric_type;
	//! A FINISH stage the operator flagged as not needing the finish event machinery:
	//! inline execution marks the sink READY instead of running finalize
	bool trivial_finish = false;
	vector<idx_t> dependents;
	idx_t dependency_count;
};

struct RecursiveCTESchedulePipeline {
	RecursiveCTESchedulePipeline(Pipeline &pipeline_p, bool is_invariant_p)
	    : pipeline(pipeline_p), is_invariant(is_invariant_p) {
	}

	reference<Pipeline> pipeline;
	//! Whether the pipeline's meta-pipeline is invariant; invariant producers keep the configured thread count
	bool is_invariant;
};

struct RecursiveCTEPipelineSchedulePlan {
	vector<RecursiveCTEScheduleStage> stages;
	//! Every pipeline of the meta-pipelines this plan schedules, in traversal order, for the per-iteration resets
	//! and executor registration; a pipeline shared by two meta-pipelines appears once per meta-pipeline
	vector<RecursiveCTESchedulePipeline> pipelines;
	vector<reference<Pipeline>> initialize_on_schedule_pipelines;
	//! Deduplicated intermediate operators of this plan's execute stages. Their global operator
	//! state is invalidated exactly once per iteration, before any stage prepares or executes.
	vector<reference<PhysicalOperator>> invalidate_operators;
	idx_t execute_pipeline_count = 0;
	idx_t cte_scan_pipeline_count = 0;
	bool has_source_tasks = false;
	//! Any execute stage reads a source that parallelizes independently of recursive inputs
	bool has_parallel_source = false;
	bool source_tasks_write_recursive_output = false;
};

class RecursiveCTESchedulerState {
public:
	RecursiveCTESchedulerState(shared_ptr<RecursiveExecutorPool> executor_pool, bool allow_executor_reuse);
	~RecursiveCTESchedulerState();

	void PrepareExecutorEntry(Pipeline &pipeline);
	void PrepareExecutors(Pipeline &pipeline, idx_t max_threads);
	vector<unique_ptr<PipelineExecutor>> &GetExecutors(Pipeline &pipeline);
	bool HasExecutorEntries() const;
	void ClearExecutors();
	void InitializeInlinePlan(const RecursiveCTEPipelineSchedulePlan &plan);
	idx_t ReadyStageCount() const;
	idx_t ReadyStage(idx_t index) const;
	void CompleteInlineStage(const RecursiveCTEPipelineSchedulePlan &plan, idx_t stage_idx);

private:
	shared_ptr<RecursiveExecutorPool> executor_pool;
	bool allow_executor_reuse;
	recursive_executor_cache_t cached_executors;
	vector<idx_t> remaining_schedule_dependencies;
	vector<idx_t> ready_schedule_stages;
};

//! Worker counts of one recursive epoch; frontier work and independent source tasks can differ
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

//! What the shared event-based scheduling of recursive plans needs from its operator:
//! the executor, the cached-executor state, and optional metrics
struct RecursiveSchedulingContext {
	RecursiveSchedulingContext(Executor &executor_p, RecursiveCTESchedulerState &scheduler_p,
	                           optional_ptr<RecursiveCTEMetrics> metrics_p,
	                           optional_ptr<RecursiveCTEEpochMetrics> epoch_metrics_p,
	                           bool scan_sources_use_source_workers_p = false)
	    : executor(executor_p), scheduler(scheduler_p), metrics(metrics_p), epoch_metrics(epoch_metrics_p),
	      scan_sources_use_source_workers(scan_sources_use_source_workers_p) {
		// invocation metrics and epoch metrics are recorded together or not at all
		D_ASSERT((metrics_p.get() == nullptr) == (epoch_metrics_p.get() == nullptr));
	}

	Executor &executor;
	RecursiveCTESchedulerState &scheduler;
	optional_ptr<RecursiveCTEMetrics> metrics;
	optional_ptr<RecursiveCTEEpochMetrics> epoch_metrics;
	//! Give stages with independent parallel sources the source-task worker count;
	//! off by default so recursive CTE scheduling is unchanged
	bool scan_sources_use_source_workers = false;
};

//! Shared with the trampoline operator: schedule-plan construction and the single-threaded inline execution helpers
unique_ptr<RecursiveCTEPipelineSchedulePlan>
BuildRecursivePipelineSchedulePlan(const vector<shared_ptr<MetaPipeline>> &meta_pipelines,
                                   const reference_set_t<const MetaPipeline> &invariant_meta_pipelines,
                                   const PhysicalOperator &recursive_sink);
void ExecuteRecursivePipelineInline(PipelineExecutor &pipeline_executor);
void ExecuteRecursivePipelineFinishInline(Pipeline &pipeline, Executor &executor);
//! Run a schedule plan single-threaded with the shared operator-state lifecycle: missing state is
//! created per ready stage, after its dependencies finished. Stages flagged trivial_finish skip the
//! finish event machinery and mark the sink READY.
void ExecuteRecursiveInlinePlan(const RecursiveSchedulingContext &context,
                                const RecursiveCTEPipelineSchedulePlan &plan);
//! Retention accounting beside its state transition, shared by recursive CTE and trampoline
void RecordInvariantRetentionTransition(RecursiveCTEMetrics &metrics,
                                        const reference_set_t<const MetaPipeline> &invariant_meta_pipelines);
void RecordInvariantRetentionReuse(RecursiveCTEMetrics &metrics,
                                   const reference_set_t<const MetaPipeline> &invariant_meta_pipelines);
void GatherRecursiveScans(PhysicalOperator &op, TableIndex cte_index,
                          vector<reference<PhysicalColumnDataScan>> &recursive_scans);
reference_set_t<const MetaPipeline>
FindInvariantRecursiveMetaPipelines(const vector<shared_ptr<MetaPipeline>> &meta_pipelines, TableIndex cte_index,
                                    const reference_set_t<const PhysicalOperator> &non_repeatable_operators);
void ConfigureInvariantRecursiveBuildReuse(const reference_set_t<const MetaPipeline> &invariant_meta_pipelines,
                                           bool preserve_build);
bool InvariantRecursiveBuildsRemainReusable(const reference_set_t<const MetaPipeline> &invariant_meta_pipelines);
void ScheduleRecursivePlan(const RecursiveCTEPipelineSchedulePlan &plan, const RecursiveSchedulingContext &context,
                           vector<shared_ptr<Event>> &events, const RecursiveCTEParallelism &parallelism);
void ExecuteRecursivePipelineEventInline(Pipeline &pipeline, const RecursiveSchedulingContext &context,
                                         idx_t worker_limit, idx_t max_threads,
                                         RecursiveCTEPipelineMetricType metric_type);
void WaitForRecursiveEvents(Executor &executor, vector<shared_ptr<Event>> &events);

} // namespace duckdb
