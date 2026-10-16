#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"

namespace duckdb {

class Logger;

struct RecursiveExecutorPool {
	mutex lock;
	PhysicalRecursiveCTE::executor_cache_t executors;
};

enum class RecursiveCTESourcePhase : uint8_t {
	INITIAL,
	SCANNING_UNION,
	RECURSING_KEY,
	DRAINING_FINAL_KEY_STATE,
	FINISHED
};

enum class RecursiveCTEPipelineMetricType : uint8_t { RECURSIVE, INVARIANT_BUILD, INVARIANT_CTE_MATERIALIZATION };

//! Epoch-stable secondary index over a proper subset of USING KEY columns.
class RecursiveCTEPartialKeyIndex {
public:
	struct Entry {
		hash_t hash;
		data_ptr_t address;
		idx_t next;
	};

	RecursiveCTEPartialKeyIndex(Allocator &allocator, const vector<LogicalType> &full_key_types,
	                            vector<idx_t> key_indices);

	//! Key and address selections can differ when addresses have already been compacted.
	void AddGroups(DataChunk &full_keys, const SelectionVector &key_selection, Vector &group_addresses,
	               const SelectionVector &address_selection, idx_t group_count);
	idx_t GetHead(hash_t hash) const;
	const Entry &GetEntry(idx_t entry_idx) const;
	idx_t Count() const;
	idx_t SizeInBytes() const;

	vector<idx_t> key_indices;

private:
	void Resize(idx_t capacity);

	DataChunk partial_keys;
	DataChunk selected_keys;
	Vector hashes;
	vector<idx_t> heads;
	vector<Entry> entries;
};

struct RecursiveCTEScheduleStage {
	RecursiveCTEScheduleStage(PipelineScheduleStageType type_p, Pipeline &pipeline_p, bool has_source_tasks_p,
	                          RecursiveCTEPipelineMetricType metric_type_p, bool is_invariant_build_p)
	    : type(type_p), pipeline(pipeline_p), has_source_tasks(has_source_tasks_p),
	      is_invariant_build(is_invariant_build_p), metric_type(metric_type_p), dependency_count(0) {
	}

	PipelineScheduleStageType type;
	reference<Pipeline> pipeline;
	bool has_source_tasks;
	bool is_invariant_build;
	RecursiveCTEPipelineMetricType metric_type;
	vector<idx_t> dependents;
	idx_t dependency_count;
};

struct RecursiveCTEPipelineSchedulePlan {
	vector<RecursiveCTEScheduleStage> stages;
	vector<reference<Pipeline>> initialize_on_schedule_pipelines;
	//! Deduplicated intermediate operators of this plan's execute stages. Their global operator
	//! state is invalidated exactly once per iteration, before any stage prepares or executes.
	vector<reference<PhysicalOperator>> invalidate_operators;
	idx_t execute_pipeline_count = 0;
	idx_t cte_scan_pipeline_count = 0;
	bool has_source_tasks = false;
	bool source_tasks_write_recursive_output = false;
};

struct RecursiveCTEMetricDistribution {
	static constexpr idx_t BIT_COUNT = sizeof(idx_t) * 8;
	static constexpr idx_t BUCKET_COUNT = BIT_COUNT + 1;

	void Add(idx_t value);
	idx_t MedianUpperBound() const;

	array<idx_t, BUCKET_COUNT> buckets {};
	idx_t count = 0;
	idx_t maximum = 0;
};

struct RecursiveCTEEpochMetrics {
	void RecordPipelineWorkers(idx_t workers);

	void Record(idx_t frontier_rows, idx_t workers, idx_t tasks, idx_t elapsed_us, idx_t frontier_storage_bytes,
	            idx_t frontier_allocation_bytes);
	void RecordDirectProbeLookup(idx_t elapsed_ns);
	void RecordDirectProbeKeyGather(idx_t elapsed_ns);
	void RecordDirectProbePayloadFinalize(idx_t elapsed_ns);
	void RecordKeyedHashCommit(idx_t elapsed_ns);
	void RecordKeyPreaggregationClassification(idx_t elapsed_ns);
	void RecordKeyPreaggregation(idx_t candidate_rows, idx_t groups, idx_t elapsed_ns);
	void RecordKeyPreaggregationCombine(idx_t elapsed_ns);
	void RecordLocalKeyPreaggregationClassification(idx_t elapsed_ns);
	void RecordLocalKeyPreaggregation(idx_t candidate_rows, idx_t groups, idx_t elapsed_ns);
	void RecordLocalKeyPreaggregationResidual(idx_t candidate_rows);
	void RecordPartialIndexMaintenance(idx_t elapsed_ns);
	void RecordKeyDelta(idx_t candidate_rows, idx_t touched_keys, idx_t new_keys, idx_t changed_keys, idx_t elapsed_ns);
	void RecordRecurringScan(idx_t elapsed_ns);
	void RecordFinalStateDrain(idx_t elapsed_ns);
	void RecordDistinctGrouping(idx_t candidate_rows, idx_t inserted_rows, idx_t elapsed_ns);
	void RecordPipelineExecution(RecursiveCTEPipelineMetricType metric_type, idx_t elapsed_ns);

	//! Maximum scheduled worker count of one EXECUTE stage across this invocation.
	atomic<idx_t> max_pipeline_workers {1};
	RecursiveCTEMetricDistribution frontier_rows;
	RecursiveCTEMetricDistribution workers;
	RecursiveCTEMetricDistribution tasks;
	RecursiveCTEMetricDistribution elapsed_us;
	idx_t frontier_storage_byte_epochs = 0;
	idx_t peak_frontier_storage_bytes = 0;
	idx_t frontier_allocation_byte_epochs = 0;
	idx_t peak_frontier_allocation_bytes = 0;
	atomic<idx_t> direct_probe_lookup_work_ns {0};
	atomic<idx_t> direct_probe_key_gather_work_ns {0};
	atomic<idx_t> direct_probe_payload_finalize_work_ns {0};
	atomic<idx_t> keyed_hash_commit_work_ns {0};
	atomic<idx_t> key_preaggregation_classification_work_ns {0};
	atomic<idx_t> key_preaggregation_work_ns {0};
	atomic<idx_t> key_preaggregation_combine_work_ns {0};
	atomic<idx_t> key_preaggregation_candidate_rows {0};
	atomic<idx_t> key_preaggregation_groups {0};
	atomic<idx_t> local_key_preaggregation_classification_work_ns {0};
	atomic<idx_t> local_key_preaggregation_work_ns {0};
	atomic<idx_t> local_key_preaggregation_candidate_rows {0};
	atomic<idx_t> local_key_preaggregation_groups {0};
	atomic<idx_t> local_key_preaggregation_states {0};
	atomic<idx_t> local_key_preaggregation_residual_rows {0};
	atomic<idx_t> partial_index_maintenance_work_ns {0};
	atomic<idx_t> key_delta_work_ns {0};
	atomic<idx_t> key_delta_candidate_rows {0};
	atomic<idx_t> key_delta_touched_keys {0};
	atomic<idx_t> key_delta_new_keys {0};
	atomic<idx_t> key_delta_changed_keys {0};
	atomic<idx_t> key_delta_unchanged_keys {0};
	atomic<idx_t> recurring_scan_work_ns {0};
	atomic<idx_t> final_state_drain_work_ns {0};
	atomic<idx_t> distinct_grouping_work_ns {0};
	atomic<idx_t> distinct_candidate_rows {0};
	atomic<idx_t> distinct_inserted_rows {0};
	atomic<idx_t> recursive_pipeline_execute_work_ns {0};
	atomic<idx_t> invariant_build_execute_work_ns {0};
	atomic<idx_t> invariant_cte_materialization_execute_work_ns {0};
};

struct RecursiveCTELogIdentity {
	RecursiveCTELogIdentity(PhysicalOperatorType operator_type_p, idx_t invocation_id_p)
	    : operator_type(operator_type_p), invocation_id(invocation_id_p) {
	}

	PhysicalOperatorType operator_type;
	vector<pair<string, string>> operator_parameters;
	idx_t invocation_id;
};

class RecursiveCTEMetrics {
public:
	RecursiveCTEMetrics(ClientContext &context, const PhysicalRecursiveCTE &op);

	bool Enabled() const {
		return enabled;
	}
	void RecordTasks(idx_t count);
	idx_t TaskCount() const;
	void RecordEpoch(idx_t workers, idx_t elapsed_us, idx_t frontier_rows, idx_t frontier_chunks,
	                 idx_t scheduler_input_rows);
	void RecordSink(idx_t wait_ns, idx_t work_ns, idx_t rows);
	void RecordHashRows(idx_t rows);
	void RecordRecurringScanRows(idx_t rows);
	void RecordDirectProbeRows(idx_t rows);
	void RecordDirectProbeMatches(idx_t rows);
	void RecordPartialProbeChainVisits(idx_t count);
	void RecordPartialIndexBuild(idx_t elapsed_us);
	void RecordFinalStateRows(idx_t rows);
	void RecordRetainedBuild();
	void RecordRetainedCTEMaterialization();
	void RecordRetainedCTEReuse();
	void LogDistinctPromotion(idx_t partitions, idx_t migrated_rows, idx_t elapsed_us) const;
	void Log(const vector<unique_ptr<RecursiveCTEPartialKeyIndex>> &partial_key_indexes) const;
	void LogEpochSummary(const RecursiveCTEEpochMetrics &epoch_metrics) const;

private:
	unique_ptr<RecursiveCTELogIdentity> identity;
	shared_ptr<Logger> logger;
	bool enabled;
	idx_t epochs = 0;
	idx_t scheduled_workers = 0;
	atomic<idx_t> scheduled_tasks {0};
	idx_t elapsed_us = 0;
	idx_t frontier_rows = 0;
	idx_t frontier_chunks = 0;
	idx_t scheduler_input_rows = 0;
	atomic<idx_t> sink_wait_ns {0};
	atomic<idx_t> sink_work_ns {0};
	atomic<idx_t> sink_rows {0};
	atomic<idx_t> sink_calls {0};
	atomic<idx_t> hash_rows {0};
	atomic<idx_t> recurring_scan_rows {0};
	atomic<idx_t> direct_probe_rows {0};
	atomic<idx_t> direct_probe_matches {0};
	atomic<idx_t> partial_probe_chain_visits {0};
	idx_t partial_index_build_us = 0;
	idx_t final_state_rows = 0;
	idx_t retained_build_executions = 0;
	idx_t retained_cte_materializations = 0;
	idx_t retained_cte_reuses = 0;
};

class RecursiveCTESchedulerState {
public:
	RecursiveCTESchedulerState(shared_ptr<RecursiveExecutorPool> executor_pool, bool allow_executor_reuse);
	~RecursiveCTESchedulerState();

	void PrepareExecutorEntry(Pipeline &pipeline);
	void PrepareExecutors(Pipeline &pipeline, idx_t max_threads);
	vector<shared_ptr<PipelineExecutor>> &GetExecutors(Pipeline &pipeline);
	bool HasExecutorEntries() const;
	void ClearExecutors();
	void InitializeInlinePlan(const RecursiveCTEPipelineSchedulePlan &plan);
	idx_t ReadyStageCount() const;
	idx_t ReadyStage(idx_t index) const;
	void CompleteInlineStage(const RecursiveCTEPipelineSchedulePlan &plan, idx_t stage_idx);

private:
	shared_ptr<RecursiveExecutorPool> executor_pool;
	bool allow_executor_reuse;
	PhysicalRecursiveCTE::executor_cache_t cached_executors;
	vector<idx_t> remaining_schedule_dependencies;
	vector<idx_t> ready_schedule_stages;
};

//! The source state of a recursive CTE - tracks the rows emitted to estimate the progress, as the number of recursive
//! iterations is not known up front
class RecursiveCTEGlobalSourceState : public GlobalSourceState {
public:
	void StartIteration(idx_t row_count);
	void StartFinalDrain(idx_t row_count);
	void AddEmittedRows(idx_t row_count) {
		emitted_rows.fetch_add(row_count, std::memory_order_relaxed);
	}
	void Finish() {
		finished.store(true, std::memory_order_relaxed);
	}
	ProgressData GetProgress();

private:
	atomic<idx_t> emitted_rows {0};
	atomic<bool> finished {false};
	//! Protects the iteration statistics, which are updated once per iteration
	mutex iteration_lock;
	//! Whether the final state of a USING KEY recursion is being emitted (its size is then known)
	bool draining = false;
	idx_t iterations = 0;
	//! Rows of the first, the current and the previous iteration
	idx_t anchor_rows = 0;
	idx_t iteration_rows = 0;
	idx_t previous_iteration_rows = 0;
	//! Rows emitted before the current iteration started
	idx_t iteration_start_rows = 0;
	MonotonicProgress progress;
};

//! The part of the recursion every mode shares: the operator, the scheduler and the metrics of one invocation.
//! RecursiveCTEUnionState and RecursiveCTEKeyedState own the state of their mode and implement the sink and source
//! sides of the operator on top of it.
class RecursiveCTEState : public GlobalSinkState {
public:
	RecursiveCTEState(ClientContext &context, const PhysicalRecursiveCTE &op);
	~RecursiveCTEState() override;

	//! Sink side of the operator in this mode; the local state is private to the mode's source file
	virtual unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const = 0;
	virtual SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) = 0;
	virtual SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) = 0;
	//! Source side: hands out result rows and drives the recursive iterations
	virtual SourceResultType GetData(ExecutionContext &context, DataChunk &chunk,
	                                 RecursiveCTEGlobalSourceState &source) = 0;
	//! Frontier the recursive member scans in the current iteration
	virtual const ColumnDataCollection &CurrentInputTable() const = 0;
	//! Rows and chunks of the recurring state the member can scan in the current iteration
	virtual idx_t RecurringStateRows() const = 0;
	virtual idx_t RecurringStateChunks() const = 0;
	idx_t CurrentInputCount() const {
		return CurrentInputTable().Count();
	}

	void RecordSinkMetrics(idx_t wait_ns, idx_t work_ns, idx_t rows);

	const PhysicalRecursiveCTE &GetOperator() const {
		return op;
	}
	RecursiveCTEMetrics &GetMetrics() {
		return metrics;
	}
	const RecursiveCTEMetrics &GetMetrics() const {
		return metrics;
	}
	RecursiveCTEEpochMetrics &GetEpochMetrics() {
		D_ASSERT(epoch_metrics);
		return *epoch_metrics;
	}
	RecursiveCTESchedulerState &GetScheduler() {
		return scheduler;
	}
	const RecursiveCTESchedulerState &GetScheduler() const {
		return scheduler;
	}
	bool AllowsExecutorReuse() const {
		return allow_executor_reuse;
	}
	bool HasMaterializedInvariantPipelines() const {
		return invariant_meta_pipelines_materialized;
	}
	void MarkInvariantPipelinesMaterialized() {
		invariant_meta_pipelines_materialized = true;
	}

protected:
	//! Logs the invocation; the mode destructors call it while their state is still alive
	void LogMetrics(const vector<unique_ptr<RecursiveCTEPartialKeyIndex>> &partial_key_indexes);

	const PhysicalRecursiveCTE &op;
	ClientContext &context;
	const bool allow_executor_reuse;
	RecursiveCTEMetrics metrics;
	RecursiveCTESchedulerState scheduler;
	RecursiveCTESourcePhase source_phase = RecursiveCTESourcePhase::INITIAL;
	//! Whether invariant recursive meta-pipelines have already been materialized for this state
	bool invariant_meta_pipelines_materialized = false;
	//! Optional epoch distributions and capacity metrics, allocated only when structured logging is active
	unique_ptr<RecursiveCTEEpochMetrics> epoch_metrics;
};

} // namespace duckdb
