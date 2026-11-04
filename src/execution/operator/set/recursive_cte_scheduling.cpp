#include "duckdb/execution/operator/set/recursive_cte_scheduling.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_blockwise_nl_join.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/execution/operator/join/physical_recursive_cte_key_join.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_complete_event.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/parallel/pipeline_finish_event.hpp"
#include "duckdb/parallel/pipeline_prepare_finish_event.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

RecursiveExecutorPool::RecursiveExecutorPool() = default;
RecursiveExecutorPool::~RecursiveExecutorPool() = default;

void RecursiveCTEMetricDistribution::Add(idx_t value) {
	const auto bucket = value == 0 ? idx_t(0) : BIT_COUNT - CountZeros<uint64_t>::Leading(value);
	D_ASSERT(bucket < buckets.size());
	buckets[bucket]++;
	count++;
	maximum = MaxValue(maximum, value);
}

idx_t RecursiveCTEMetricDistribution::MedianUpperBound() const {
	if (count == 0) {
		return 0;
	}
	const auto target = count / 2 + count % 2;
	idx_t cumulative = 0;
	for (idx_t bucket = 0; bucket < buckets.size(); bucket++) {
		cumulative += buckets[bucket];
		if (cumulative < target) {
			continue;
		}
		if (bucket == 0) {
			return 0;
		}
		if (bucket == BIT_COUNT) {
			return ~idx_t(0);
		}
		return (idx_t(1) << bucket) - 1;
	}
	throw InternalException("Recursive CTE metric distribution is inconsistent");
}

void RecursiveCTEEpochMetrics::Record(idx_t frontier_rows_p, idx_t workers_p, idx_t tasks_p, idx_t elapsed_us_p,
                                      idx_t frontier_storage_bytes, idx_t frontier_allocation_bytes) {
	frontier_rows.Add(frontier_rows_p);
	workers.Add(workers_p);
	tasks.Add(tasks_p);
	elapsed_us.Add(elapsed_us_p);
	frontier_storage_byte_epochs += frontier_storage_bytes;
	peak_frontier_storage_bytes = MaxValue(peak_frontier_storage_bytes, frontier_storage_bytes);
	frontier_allocation_byte_epochs += frontier_allocation_bytes;
	peak_frontier_allocation_bytes = MaxValue(peak_frontier_allocation_bytes, frontier_allocation_bytes);
}

void RecursiveCTEEpochMetrics::RecordDirectProbeLookup(idx_t elapsed_ns) {
	direct_probe_lookup_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordDirectProbeKeyGather(idx_t elapsed_ns) {
	direct_probe_key_gather_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordDirectProbePayloadFinalize(idx_t elapsed_ns) {
	direct_probe_payload_finalize_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordKeyedHashCommit(idx_t elapsed_ns) {
	keyed_hash_commit_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordKeyPreaggregationClassification(idx_t elapsed_ns) {
	key_preaggregation_classification_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordKeyPreaggregation(idx_t candidate_rows, idx_t groups, idx_t elapsed_ns) {
	D_ASSERT(groups <= candidate_rows);
	key_preaggregation_candidate_rows.fetch_add(candidate_rows);
	key_preaggregation_groups.fetch_add(groups);
	key_preaggregation_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordKeyPreaggregationCombine(idx_t elapsed_ns) {
	key_preaggregation_combine_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordLocalKeyPreaggregationClassification(idx_t elapsed_ns) {
	local_key_preaggregation_classification_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordLocalKeyPreaggregation(idx_t candidate_rows, idx_t groups, idx_t elapsed_ns) {
	D_ASSERT(groups <= candidate_rows);
	local_key_preaggregation_candidate_rows.fetch_add(candidate_rows);
	local_key_preaggregation_groups.fetch_add(groups);
	local_key_preaggregation_states.fetch_add(1);
	local_key_preaggregation_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordLocalKeyPreaggregationResidual(idx_t candidate_rows) {
	local_key_preaggregation_residual_rows.fetch_add(candidate_rows);
}

void RecursiveCTEEpochMetrics::RecordPartialIndexMaintenance(idx_t elapsed_ns) {
	partial_index_maintenance_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordKeyDelta(idx_t candidate_rows, idx_t touched_keys, idx_t new_keys,
                                              idx_t changed_keys, idx_t elapsed_ns) {
	D_ASSERT(new_keys + changed_keys <= touched_keys);
	key_delta_work_ns.fetch_add(elapsed_ns);
	key_delta_candidate_rows.fetch_add(candidate_rows);
	key_delta_touched_keys.fetch_add(touched_keys);
	key_delta_new_keys.fetch_add(new_keys);
	key_delta_changed_keys.fetch_add(changed_keys);
	key_delta_unchanged_keys.fetch_add(touched_keys - new_keys - changed_keys);
}

void RecursiveCTEEpochMetrics::RecordRecurringScan(idx_t elapsed_ns) {
	recurring_scan_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordFinalStateDrain(idx_t elapsed_ns) {
	final_state_drain_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordDistinctGrouping(idx_t candidate_rows, idx_t inserted_rows, idx_t elapsed_ns) {
	D_ASSERT(inserted_rows <= candidate_rows);
	distinct_candidate_rows.fetch_add(candidate_rows);
	distinct_inserted_rows.fetch_add(inserted_rows);
	distinct_grouping_work_ns.fetch_add(elapsed_ns);
}

void RecursiveCTEEpochMetrics::RecordPipelineWorkers(idx_t workers) {
	auto current = max_pipeline_workers.load();
	while (current < workers && !max_pipeline_workers.compare_exchange_weak(current, workers)) {
	}
}

void RecursiveCTEEpochMetrics::RecordPipelineExecution(RecursiveCTEPipelineMetricType metric_type, idx_t elapsed_ns) {
	switch (metric_type) {
	case RecursiveCTEPipelineMetricType::RECURSIVE:
		recursive_pipeline_execute_work_ns.fetch_add(elapsed_ns);
		break;
	case RecursiveCTEPipelineMetricType::INVARIANT_BUILD:
		invariant_build_execute_work_ns.fetch_add(elapsed_ns);
		break;
	case RecursiveCTEPipelineMetricType::INVARIANT_CTE_MATERIALIZATION:
		invariant_cte_materialization_execute_work_ns.fetch_add(elapsed_ns);
		break;
	}
}

RecursiveCTEMetrics::RecursiveCTEMetrics(ClientContext &context, const PhysicalRecursiveCTE &op_p)
    : RecursiveCTEMetrics(context, op_p, 0) {
	if (enabled) {
		identity->invocation_id = op_p.NextMetricsInvocation();
	}
}

RecursiveCTEMetrics::RecursiveCTEMetrics(ClientContext &context, const PhysicalOperator &op_p, idx_t invocation_id)
    : logger(context.GetContext().GetLoggerPtr()),
      enabled(logger && logger->ShouldLog(PhysicalOperatorLogType::NAME, PhysicalOperatorLogType::LEVEL)) {
	if (enabled) {
		identity = make_uniq<RecursiveCTELogIdentity>(op_p.type, invocation_id);
		if (op_p.type == PhysicalOperatorType::TRAMPOLINE) {
			identity->class_name = "PhysicalTrampoline";
		}
		for (const auto &entry : op_p.ParamsToString()) {
			identity->operator_parameters.emplace_back(entry.first, entry.second);
		}
	}
}

void RecursiveCTEMetrics::RecordTasks(idx_t count) {
	scheduled_tasks.fetch_add(count);
}

idx_t RecursiveCTEMetrics::TaskCount() const {
	return scheduled_tasks.load();
}

void RecursiveCTEMetrics::RecordEpoch(idx_t workers, idx_t elapsed_us_p, idx_t frontier_rows_p, idx_t frontier_chunks_p,
                                      idx_t scheduler_input_rows_p) {
	epochs++;
	scheduled_workers += workers;
	elapsed_us += elapsed_us_p;
	frontier_rows += frontier_rows_p;
	frontier_chunks += frontier_chunks_p;
	scheduler_input_rows += scheduler_input_rows_p;
}

void RecursiveCTEMetrics::RecordSink(idx_t wait_ns, idx_t work_ns, idx_t rows) {
	sink_wait_ns.fetch_add(wait_ns);
	sink_work_ns.fetch_add(work_ns);
	sink_rows.fetch_add(rows);
	sink_calls.fetch_add(1);
}

void RecursiveCTEMetrics::RecordHashRows(idx_t rows) {
	hash_rows.fetch_add(rows);
}

void RecursiveCTEMetrics::RecordRecurringScanRows(idx_t rows) {
	recurring_scan_rows.fetch_add(rows);
}

void RecursiveCTEMetrics::RecordDirectProbeRows(idx_t rows) {
	direct_probe_rows.fetch_add(rows);
}

void RecursiveCTEMetrics::RecordDirectProbeMatches(idx_t rows) {
	direct_probe_matches.fetch_add(rows);
}

void RecursiveCTEMetrics::RecordPartialProbeChainVisits(idx_t count) {
	partial_probe_chain_visits.fetch_add(count);
}

void RecursiveCTEMetrics::RecordPartialIndexBuild(idx_t elapsed_us_p) {
	partial_index_build_us += elapsed_us_p;
}

void RecursiveCTEMetrics::RecordFinalStateRows(idx_t rows) {
	final_state_rows += rows;
}

void RecursiveCTEMetrics::RecordRetainedBuild() {
	retained_build_executions++;
}

void RecursiveCTEMetrics::RecordRetainedCTEMaterialization() {
	retained_cte_materializations++;
}

void RecursiveCTEMetrics::RecordRetainedCTEReuse() {
	retained_cte_reuses++;
}

void RecursiveCTEMetrics::LogDistinctPromotion(idx_t partitions, idx_t migrated_rows, idx_t elapsed_us_p) const {
	if (!enabled) {
		return;
	}
	D_ASSERT(identity);
	DUCKDB_LOG(logger, PhysicalOperatorLogType, identity->operator_type, identity->operator_parameters,
	           identity->class_name, "DistinctPromoted",
	           {{"invocation_id", to_string(identity->invocation_id)},
	            {"partitions", to_string(partitions)},
	            {"migrated_rows", to_string(migrated_rows)},
	            {"elapsed_us", to_string(elapsed_us_p)}});
}

//! For operators without partial key indexes; the vector type needs the complete
//! key-index class, which callers of this overload do not see
void RecursiveCTEMetrics::Log() const {
	Log(vector<unique_ptr<RecursiveCTEPartialKeyIndex>>());
}

void RecursiveCTEMetrics::Log(const vector<unique_ptr<RecursiveCTEPartialKeyIndex>> &partial_key_indexes) const {
	if (!enabled) {
		return;
	}
	D_ASSERT(identity);
	idx_t partial_index_rows = 0;
	idx_t partial_index_bytes = 0;
	for (auto &index : partial_key_indexes) {
		partial_index_rows += index->Count();
		partial_index_bytes += index->SizeInBytes();
	}
	DUCKDB_LOG(logger, PhysicalOperatorLogType, identity->operator_type, identity->operator_parameters,
	           identity->class_name, "RuntimeMetrics",
	           {{"invocation_id", to_string(identity->invocation_id)},
	            {"epochs", to_string(epochs)},
	            {"scheduled_workers", to_string(scheduled_workers)},
	            {"scheduled_tasks", to_string(scheduled_tasks.load())},
	            {"elapsed_us", to_string(elapsed_us)},
	            {"frontier_rows", to_string(frontier_rows)},
	            {"frontier_chunks", to_string(frontier_chunks)},
	            {"scheduler_input_rows", to_string(scheduler_input_rows)},
	            {"sink_lock_wait_ns", to_string(sink_wait_ns.load())},
	            {"sink_lock_work_ns", to_string(sink_work_ns.load())},
	            {"sink_lock_rows", to_string(sink_rows.load())},
	            {"sink_lock_calls", to_string(sink_calls.load())},
	            {"hash_rows", to_string(hash_rows.load())},
	            {"recurring_scan_rows", to_string(recurring_scan_rows.load())},
	            {"direct_probe_rows", to_string(direct_probe_rows.load())},
	            {"direct_probe_matches", to_string(direct_probe_matches.load())},
	            {"partial_probe_chain_visits", to_string(partial_probe_chain_visits.load())},
	            {"partial_index_build_us", to_string(partial_index_build_us)},
	            {"partial_index_rows", to_string(partial_index_rows)},
	            {"partial_index_bytes", to_string(partial_index_bytes)},
	            {"final_state_rows", to_string(final_state_rows)},
	            {"retained_build_executions", to_string(retained_build_executions)},
	            {"retained_cte_materializations", to_string(retained_cte_materializations)},
	            {"retained_cte_reuses", to_string(retained_cte_reuses)}});
}

void RecursiveCTEMetrics::LogEpochSummary(const RecursiveCTEEpochMetrics &epoch_metrics) const {
	if (!enabled) {
		return;
	}
	D_ASSERT(identity);
	const auto distinct_candidate_rows = epoch_metrics.distinct_candidate_rows.load();
	const auto distinct_inserted_rows = epoch_metrics.distinct_inserted_rows.load();
	D_ASSERT(distinct_inserted_rows <= distinct_candidate_rows);
	DUCKDB_LOG(
	    logger, PhysicalOperatorLogType, identity->operator_type, identity->operator_parameters, identity->class_name,
	    "EpochSummary",
	    {{"invocation_id", to_string(identity->invocation_id)},
	     {"epochs", to_string(epoch_metrics.frontier_rows.count)},
	     {"frontier_rows_p50_upper_bound", to_string(epoch_metrics.frontier_rows.MedianUpperBound())},
	     {"frontier_rows_max", to_string(epoch_metrics.frontier_rows.maximum)},
	     {"workers_p50_upper_bound", to_string(epoch_metrics.workers.MedianUpperBound())},
	     {"workers_max", to_string(epoch_metrics.workers.maximum)},
	     {"max_pipeline_workers", to_string(epoch_metrics.max_pipeline_workers.load())},
	     {"tasks_p50_upper_bound", to_string(epoch_metrics.tasks.MedianUpperBound())},
	     {"tasks_max", to_string(epoch_metrics.tasks.maximum)},
	     {"elapsed_us_p50_upper_bound", to_string(epoch_metrics.elapsed_us.MedianUpperBound())},
	     {"elapsed_us_max", to_string(epoch_metrics.elapsed_us.maximum)},
	     {"frontier_storage_byte_epochs", to_string(epoch_metrics.frontier_storage_byte_epochs)},
	     {"peak_frontier_storage_bytes", to_string(epoch_metrics.peak_frontier_storage_bytes)},
	     {"frontier_allocation_byte_epochs", to_string(epoch_metrics.frontier_allocation_byte_epochs)},
	     {"peak_frontier_allocation_bytes", to_string(epoch_metrics.peak_frontier_allocation_bytes)},
	     {"direct_probe_lookup_work_ns", to_string(epoch_metrics.direct_probe_lookup_work_ns.load())},
	     {"direct_probe_key_gather_work_ns", to_string(epoch_metrics.direct_probe_key_gather_work_ns.load())},
	     {"direct_probe_payload_finalize_work_ns",
	      to_string(epoch_metrics.direct_probe_payload_finalize_work_ns.load())},
	     {"keyed_hash_commit_work_ns", to_string(epoch_metrics.keyed_hash_commit_work_ns.load())},
	     {"key_preaggregation_classification_work_ns",
	      to_string(epoch_metrics.key_preaggregation_classification_work_ns.load())},
	     {"key_preaggregation_work_ns", to_string(epoch_metrics.key_preaggregation_work_ns.load())},
	     {"key_preaggregation_combine_work_ns", to_string(epoch_metrics.key_preaggregation_combine_work_ns.load())},
	     {"key_preaggregation_candidate_rows", to_string(epoch_metrics.key_preaggregation_candidate_rows.load())},
	     {"key_preaggregation_groups", to_string(epoch_metrics.key_preaggregation_groups.load())},
	     {"local_key_preaggregation_classification_work_ns",
	      to_string(epoch_metrics.local_key_preaggregation_classification_work_ns.load())},
	     {"local_key_preaggregation_work_ns", to_string(epoch_metrics.local_key_preaggregation_work_ns.load())},
	     {"local_key_preaggregation_candidate_rows",
	      to_string(epoch_metrics.local_key_preaggregation_candidate_rows.load())},
	     {"local_key_preaggregation_groups", to_string(epoch_metrics.local_key_preaggregation_groups.load())},
	     {"local_key_preaggregation_states", to_string(epoch_metrics.local_key_preaggregation_states.load())},
	     {"local_key_preaggregation_residual_rows",
	      to_string(epoch_metrics.local_key_preaggregation_residual_rows.load())},
	     {"partial_index_maintenance_work_ns", to_string(epoch_metrics.partial_index_maintenance_work_ns.load())},
	     {"key_delta_work_ns", to_string(epoch_metrics.key_delta_work_ns.load())},
	     {"key_delta_candidate_rows", to_string(epoch_metrics.key_delta_candidate_rows.load())},
	     {"key_delta_touched_keys", to_string(epoch_metrics.key_delta_touched_keys.load())},
	     {"key_delta_new_keys", to_string(epoch_metrics.key_delta_new_keys.load())},
	     {"key_delta_changed_keys", to_string(epoch_metrics.key_delta_changed_keys.load())},
	     {"key_delta_unchanged_keys", to_string(epoch_metrics.key_delta_unchanged_keys.load())},
	     {"recurring_scan_work_ns", to_string(epoch_metrics.recurring_scan_work_ns.load())},
	     {"final_state_drain_work_ns", to_string(epoch_metrics.final_state_drain_work_ns.load())},
	     {"distinct_grouping_work_ns", to_string(epoch_metrics.distinct_grouping_work_ns.load())},
	     {"distinct_candidate_rows", to_string(distinct_candidate_rows)},
	     {"distinct_inserted_rows", to_string(distinct_inserted_rows)},
	     {"distinct_duplicate_rows", to_string(distinct_candidate_rows - distinct_inserted_rows)},
	     {"recursive_pipeline_execute_work_ns", to_string(epoch_metrics.recursive_pipeline_execute_work_ns.load())},
	     {"invariant_build_execute_work_ns", to_string(epoch_metrics.invariant_build_execute_work_ns.load())},
	     {"invariant_cte_materialization_execute_work_ns",
	      to_string(epoch_metrics.invariant_cte_materialization_execute_work_ns.load())}});
}

RecursiveCTESchedulerState::RecursiveCTESchedulerState(shared_ptr<RecursiveExecutorPool> executor_pool_p,
                                                       bool allow_executor_reuse_p)
    : executor_pool(std::move(executor_pool_p)), allow_executor_reuse(allow_executor_reuse_p) {
}

RecursiveCTESchedulerState::~RecursiveCTESchedulerState() {
	ClearExecutors();
}

void RecursiveCTESchedulerState::InitializeInlinePlan(const RecursiveCTEPipelineSchedulePlan &plan) {
	remaining_schedule_dependencies.clear();
	remaining_schedule_dependencies.reserve(plan.stages.size());
	ready_schedule_stages.clear();
	ready_schedule_stages.reserve(plan.stages.size());
	for (idx_t stage_idx = 0; stage_idx < plan.stages.size(); stage_idx++) {
		auto dependency_count = plan.stages[stage_idx].dependency_count;
		remaining_schedule_dependencies.push_back(dependency_count);
		if (dependency_count == 0) {
			ready_schedule_stages.push_back(stage_idx);
		}
	}
}

idx_t RecursiveCTESchedulerState::ReadyStageCount() const {
	return ready_schedule_stages.size();
}

idx_t RecursiveCTESchedulerState::ReadyStage(idx_t index) const {
	return ready_schedule_stages[index];
}

void RecursiveCTESchedulerState::CompleteInlineStage(const RecursiveCTEPipelineSchedulePlan &plan, idx_t stage_idx) {
	for (auto dependent_stage : plan.stages[stage_idx].dependents) {
		auto &remaining = remaining_schedule_dependencies[dependent_stage];
		if (remaining == 0) {
			throw InternalException("Recursive inline schedule dependency underflow");
		}
		remaining--;
		if (remaining == 0) {
			ready_schedule_stages.push_back(dependent_stage);
		}
	}
}

void RecursiveCTESchedulerState::PrepareExecutorEntry(Pipeline &pipeline) {
	cached_executors.emplace(reference<Pipeline>(pipeline), vector<unique_ptr<PipelineExecutor>>());
}

void RecursiveCTESchedulerState::PrepareExecutors(Pipeline &pipeline, idx_t max_threads) {
	// Recursive CTEs re-enter pipelines repeatedly, so retain executors locally and recycle them across states.
	auto entry = cached_executors.find(pipeline);
	if (entry == cached_executors.end()) {
		throw InternalException("Missing recursive pipeline executor cache entry");
	}
	auto &executors = entry->second;
	if (executors.size() >= max_threads) {
		return;
	}
	if (!allow_executor_reuse) {
		while (executors.size() < max_threads) {
			executors.push_back(make_uniq<PipelineExecutor>(pipeline.GetClientContext(), pipeline));
		}
		return;
	}
	D_ASSERT(executor_pool);
	lock_guard<mutex> pool_guard(executor_pool->lock);
	auto pool_entry = executor_pool->executors.find(pipeline);
	if (pool_entry == executor_pool->executors.end()) {
		pool_entry =
		    executor_pool->executors.emplace(reference<Pipeline>(pipeline), vector<unique_ptr<PipelineExecutor>>())
		        .first;
	}
	auto &shared_executors = pool_entry->second;
	while (executors.size() < max_threads) {
		if (!shared_executors.empty()) {
			executors.push_back(std::move(shared_executors.back()));
			shared_executors.pop_back();
		} else {
			executors.push_back(make_uniq<PipelineExecutor>(pipeline.GetClientContext(), pipeline));
		}
	}
}

vector<unique_ptr<PipelineExecutor>> &RecursiveCTESchedulerState::GetExecutors(Pipeline &pipeline) {
	auto entry = cached_executors.find(pipeline);
	if (entry == cached_executors.end()) {
		throw InternalException("Missing recursive pipeline executor cache entry");
	}
	return entry->second;
}

bool RecursiveCTESchedulerState::HasExecutorEntries() const {
	return !cached_executors.empty();
}

void RecursiveCTESchedulerState::ClearExecutors() {
	if (cached_executors.empty()) {
		return;
	}
	if (!allow_executor_reuse) {
		cached_executors.clear();
		return;
	}
	D_ASSERT(executor_pool);
	lock_guard<mutex> pool_guard(executor_pool->lock);
	for (auto &entry : cached_executors) {
		auto pool_entry = executor_pool->executors.find(entry.first.get());
		if (pool_entry == executor_pool->executors.end()) {
			pool_entry = executor_pool->executors.emplace(entry.first, vector<unique_ptr<PipelineExecutor>>()).first;
		}
		auto &shared_executors = pool_entry->second;
		for (auto &executor : entry.second) {
			shared_executors.push_back(std::move(executor));
		}
	}
	cached_executors.clear();
}

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
idx_t GetRecursiveProducerWorkerBound(Pipeline &pipeline, idx_t worker_limit) {
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

static bool PumpRecursiveExecutor(Executor &executor);

static constexpr std::chrono::milliseconds RECURSIVE_BOUNDED_WAIT {Executor::WAIT_TIME};

bool RecursiveInterruptSignalState::AwaitFor(std::chrono::milliseconds timeout) {
	annotated_unique_lock<annotated_mutex> guard(lock);
	cv.wait_for(guard, timeout, [&]() { return done; });
	if (!done) {
		return false;
	}
	done = false;
	return true;
}

void RecursiveInterruptSignalState::AwaitWithExecutorProgress(Executor &executor) {
	// A completion callback notifies this signal directly and wakes the wait immediately.
	// The timeout bounds the remaining wake source: another thread enqueueing an executor
	// task instead of signalling, which the next pump iteration picks up.
	while (true) {
		PumpRecursiveExecutor(executor);
		if (AwaitFor(RECURSIVE_BOUNDED_WAIT)) {
			return;
		}
	}
}

void ExecuteRecursivePipelineInline(PipelineExecutor &pipeline_executor, Executor &executor) {
	auto signal_state = make_shared_ptr<RecursiveInterruptSignalState>();
	pipeline_executor.SetInterruptState(InterruptState(weak_ptr<InterruptDoneSignalState>(signal_state)));
	while (true) {
		auto result = pipeline_executor.Execute();
		switch (result) {
		case PipelineExecuteResult::FINISHED:
			return;
		case PipelineExecuteResult::NOT_FINISHED:
			throw InternalException("Execute without limit should not return NOT_FINISHED");
		case PipelineExecuteResult::INTERRUPTED:
			signal_state->AwaitWithExecutorProgress(executor);
			break;
		}
	}
}

template <bool COLLECT_METRICS>
struct RecursiveCTETaskMetrics {
	RecursiveCTETaskMetrics(RecursiveSchedulingContext &, RecursiveCTEPipelineMetricType) {
	}
};

template <>
struct RecursiveCTETaskMetrics<true> {
	RecursiveCTETaskMetrics(RecursiveSchedulingContext &context_p, RecursiveCTEPipelineMetricType metric_type_p)
	    : epoch_metrics(*context_p.epoch_metrics), metric_type(metric_type_p) {
	}

	RecursiveCTEEpochMetrics &epoch_metrics;
	RecursiveCTEPipelineMetricType metric_type;
};

//! A task that executes a cached PipelineExecutor
template <bool COLLECT_METRICS>
class RecursiveCTETask : public ExecutorTask, private RecursiveCTETaskMetrics<COLLECT_METRICS> {
public:
	RecursiveCTETask(Pipeline &pipeline_p, shared_ptr<Event> event_p, PipelineExecutor &executor_p,
	                 RecursiveSchedulingContext &context_p, RecursiveCTEPipelineMetricType metric_type_p)
	    : ExecutorTask(pipeline_p.executor, std::move(event_p)), RecursiveCTETaskMetrics<COLLECT_METRICS>(
	                                                                 context_p, metric_type_p),
	      pipeline(pipeline_p), pipeline_executor(executor_p), completion_signal(context_p.completion_signal) {
	}

	Pipeline &pipeline;
	PipelineExecutor &pipeline_executor;
	//! Wakes the coordinating thread's event wait when this task finishes
	shared_ptr<RecursiveInterruptSignalState> completion_signal;

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		pipeline_executor.SetTaskForInterrupts(shared_from_this());
		const auto execute_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		const auto result = mode == TaskExecutionMode::PROCESS_PARTIAL ? pipeline_executor.Execute(PARTIAL_CHUNK_COUNT)
		                                                               : pipeline_executor.Execute();
		if constexpr (COLLECT_METRICS) {
			const auto execute_end = std::chrono::steady_clock::now();
			this->epoch_metrics.RecordPipelineExecution(
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
		if (completion_signal) {
			completion_signal->Signal();
		}
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
	RecursiveCTEPipelineEvent(shared_ptr<Pipeline> pipeline_p, const RecursiveSchedulingContext &context_p,
	                          idx_t worker_limit_p, RecursiveCTEPipelineMetricType metric_type_p)
	    : BasePipelineEvent(std::move(pipeline_p)), context(context_p), worker_limit(worker_limit_p),
	      metric_type(metric_type_p) {
	}

	RecursiveSchedulingContext context;
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
		const auto collect_metrics = context.metrics && context.metrics->Enabled();
		if (collect_metrics) {
			context.metrics->RecordTasks(max_threads);
			context.epoch_metrics->RecordPipelineWorkers(max_threads);
		}
		AdjustRecursiveSinkWorkers(*pipeline, max_threads);
		context.scheduler.PrepareExecutors(*pipeline, max_threads);
		auto &executors = context.scheduler.GetExecutors(*pipeline);
		D_ASSERT(executors.size() >= max_threads);

		// Create tasks using cached executors
		vector<shared_ptr<Task>> tasks;
		for (idx_t i = 0; i < max_threads; i++) {
			executors[i]->PrepareForExecution();
			if (collect_metrics) {
				tasks.push_back(make_uniq<RecursiveCTETask<true>>(*pipeline, shared_from_this(), *executors[i], context,
				                                                  metric_type));
			} else {
				tasks.push_back(make_uniq<RecursiveCTETask<false>>(*pipeline, shared_from_this(), *executors[i],
				                                                   context, metric_type));
			}
		}
		SetTasks(std::move(tasks));
	}
};

//! Finish-stage events complete through dependency chains on whichever worker finished the
//! last dependency task, without creating tasks of their own. The recursive event waiter
//! sleeps on the completion signal, so these wrappers forward their completion to it; the
//! finalize semantics are the wrapped events' own.
template <class EVENT>
class RecursiveSignallingEvent : public EVENT {
public:
	RecursiveSignallingEvent(shared_ptr<Pipeline> pipeline_p, shared_ptr<RecursiveInterruptSignalState> signal_p)
	    : EVENT(std::move(pipeline_p)), completion_signal(std::move(signal_p)) {
	}

	void FinishEvent() override {
		EVENT::FinishEvent();
		if (completion_signal) {
			completion_signal->Signal();
		}
	}

private:
	shared_ptr<RecursiveInterruptSignalState> completion_signal;
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
		// Blocking here is safe because the caller runs it synchronously and explicitly waits for
		// completion, and the wait keeps the executor making progress.
		D_ASSERT(total_tasks == 0);
		total_tasks = 1;

		auto signal_state = make_shared_ptr<RecursiveInterruptSignalState>();
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
					signal_state->AwaitWithExecutorProgress(executor);
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
				signal_state->AwaitWithExecutorProgress(executor);
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

unique_ptr<RecursiveCTEPipelineSchedulePlan>
BuildRecursivePipelineSchedulePlan(const vector<shared_ptr<MetaPipeline>> &meta_pipelines,
                                   const reference_set_t<const MetaPipeline> &invariant_meta_pipelines,
                                   const PhysicalOperator &recursive_sink) {
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
		const auto is_invariant = invariant_meta_pipelines.find(*meta_pipeline) != invariant_meta_pipelines.end();
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
		bool has_parallel_source = false;
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
				    plan->source_tasks_write_recursive_output || pipeline.GetSink().get() == &recursive_sink;
			}
			for (auto &op_ref : pipeline.GetIntermediateOperators()) {
				if (invalidate_dedup.insert(op_ref.get()).second) {
					plan->invalidate_operators.push_back(op_ref);
				}
			}
			// capability alone (e.g. a dormant hash-join cleanup source) must not count as
			// independent source work, so only concrete scans qualify
			has_parallel_source =
			    source && source->ParallelSource() && source->type == PhysicalOperatorType::TABLE_SCAN;
			plan->has_parallel_source = plan->has_parallel_source || has_parallel_source;
			if ((has_parallel_source || has_source_tasks) &&
			    invariant_build_pipelines.find(pipeline) == invariant_build_pipelines.end()) {
				plan->max_independent_source_rows =
				    MaxValue(plan->max_independent_source_rows, source->estimated_cardinality);
			}
		}
		recursive_stage_indices[stage_idx] = plan->stages.size();
		auto metric_type = pipeline_metric_types.find(pipeline);
		D_ASSERT(metric_type != pipeline_metric_types.end());
		plan->stages.emplace_back(stage.type, pipeline, has_source_tasks, has_parallel_source, metric_type->second,
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
	    op.type == PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN ||
	    op.type == PhysicalOperatorType::TRAMPOLINE_FRONTIER_SCAN) {
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

void CountDirectRecursiveReferences(const PhysicalOperator &op, TableIndex cte_index,
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
	    op.type != PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN &&
	    op.type != PhysicalOperatorType::TRAMPOLINE_FRONTIER_SCAN) {
		return;
	}
	auto &scan = op.Cast<PhysicalColumnDataScan>();
	if (scan.cte_index != cte_index) {
		return;
	}
	if (op.type == PhysicalOperatorType::RECURSIVE_CTE_SCAN) {
		references.frontier_scans++;
	} else {
		// the trampoline's round snapshot changes between rounds like the recurring table
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

reference_set_t<const MetaPipeline>
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

void ConfigureInvariantRecursiveBuildReuse(const reference_set_t<const MetaPipeline> &invariant_meta_pipelines,
                                           bool preserve_build) {
	for (auto &meta_pipeline_ref : invariant_meta_pipelines) {
		auto sink = meta_pipeline_ref.get().GetSink();
		if (!sink || sink->type != PhysicalOperatorType::HASH_JOIN) {
			continue;
		}
		sink->Cast<PhysicalHashJoin>().SetPreserveBuildForRecursiveReuse(preserve_build);
	}
}

bool InvariantRecursiveBuildsRemainReusable(const reference_set_t<const MetaPipeline> &invariant_meta_pipelines) {
	// Recursive invariant caching may omit these meta-pipelines entirely after the first materialization.
	// That is only safe if every invariant HASH_JOIN actually kept a reusable in-memory build side.
	// External/spilled hash joins run through mutable partition/probe-spill rounds and must therefore
	// stay in the active recursive schedule instead of being treated as "materialized once".
	for (auto &meta_pipeline_ref : invariant_meta_pipelines) {
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

//! Retention accounting lives beside the state transitions it describes, shared by the
//! recursive CTE and the trampoline so the counters cannot drift between callers.
void RecordInvariantRetentionTransition(RecursiveCTEMetrics &metrics,
                                        const reference_set_t<const MetaPipeline> &invariant_meta_pipelines) {
	for (auto &meta_pipeline : invariant_meta_pipelines) {
		auto sink = meta_pipeline.get().GetSink();
		if (sink && sink->type == PhysicalOperatorType::CTE) {
			metrics.RecordRetainedCTEMaterialization();
		} else {
			metrics.RecordRetainedBuild();
		}
	}
}

void RecordInvariantRetentionReuse(RecursiveCTEMetrics &metrics,
                                   const reference_set_t<const MetaPipeline> &invariant_meta_pipelines) {
	for (auto &meta_pipeline : invariant_meta_pipelines) {
		auto sink = meta_pipeline.get().GetSink();
		if (sink && sink->type == PhysicalOperatorType::CTE) {
			metrics.RecordRetainedCTEReuse();
		}
	}
}

//! Runs pending executor tasks; returns whether any ran. An executor error takes precedence
//! over any other outcome, and an idle iteration enforces interrupts and the query deadline,
//! which no task-side InterruptCheck call site can reach while nothing executes.
static bool PumpRecursiveExecutor(Executor &executor) {
	// Only drain the recursive executor here. Re-entering the outer query executor while waiting for
	// recursive work can run unrelated tasks and used to break recursive completion tracking.
	const auto did_work = executor.WorkOnTasks();
	if (executor.HasError()) {
		executor.ThrowException();
	}
	if (!did_work) {
		executor.context.BlockingInterruptCheck();
	}
	return did_work;
}

void WaitForRecursiveEvents(Executor &executor, vector<shared_ptr<Event>> &events,
                            optional_ptr<RecursiveInterruptSignalState> completion_signal) {
	while (true) {
		const auto did_work = PumpRecursiveExecutor(executor);
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
		if (did_work) {
			continue;
		}
		if (completion_signal) {
			// woken promptly by any recursive task completion; the bounded timeout covers
			// tasks that finish without signalling (finish and prepare-finish events)
			completion_signal->AwaitFor(RECURSIVE_BOUNDED_WAIT);
		} else {
			executor.WaitForTask();
		}
		if (executor.HasError()) {
			executor.ThrowException();
		}
	}
}

void WaitForRecursiveEvent(Executor &executor, Event &event,
                           optional_ptr<RecursiveInterruptSignalState> completion_signal) {
	while (!event.IsFinished()) {
		const auto did_work = PumpRecursiveExecutor(executor);
		if (did_work || event.IsFinished()) {
			continue;
		}
		if (completion_signal) {
			completion_signal->AwaitFor(RECURSIVE_BOUNDED_WAIT);
		} else {
			executor.WaitForTask();
		}
		if (executor.HasError()) {
			executor.ThrowException();
		}
	}
}

void ExecuteRecursivePipelineEventInline(Pipeline &pipeline, const RecursiveSchedulingContext &context,
                                         idx_t worker_limit, idx_t max_threads,
                                         RecursiveCTEPipelineMetricType metric_type) {
	auto event_context = context;
	if (!event_context.completion_signal) {
		event_context.completion_signal = make_shared_ptr<RecursiveInterruptSignalState>();
	}
	auto event = make_shared_ptr<RecursiveCTEPipelineEvent>(pipeline.shared_from_this(), event_context, worker_limit,
	                                                        metric_type);
	event->SchedulePrepared(max_threads);
	WaitForRecursiveEvent(event_context.executor, *event, event_context.completion_signal.get());
}

void ScheduleRecursivePlan(const RecursiveCTEPipelineSchedulePlan &plan, const RecursiveSchedulingContext &context,
                           vector<shared_ptr<Event>> &events, const RecursiveCTEParallelism &parallelism) {
	for (auto &pipeline : plan.initialize_on_schedule_pipelines) {
		pipeline.get().ResetSource(true);
	}

	const auto configured_threads = TaskScheduler::GetScheduler(context.executor.context).NumberOfThreads();
	events.reserve(plan.stages.size());
	for (auto &stage : plan.stages) {
		auto pipeline = stage.pipeline.get().shared_from_this();
		switch (stage.type) {
		case PipelineScheduleStageType::EXECUTE: {
			const auto source_sized =
			    stage.has_source_tasks || (context.scan_sources_use_source_workers && stage.has_parallel_source);
			events.push_back(make_shared_ptr<RecursiveCTEPipelineEvent>(
			    std::move(pipeline), context,
			    stage.is_invariant_build ? configured_threads : parallelism.WorkerCount(source_sized),
			    stage.metric_type));
			break;
		}
		case PipelineScheduleStageType::PREPARE_FINISH:
			events.push_back(make_shared_ptr<RecursiveSignallingEvent<PipelinePrepareFinishEvent>>(
			    std::move(pipeline), context.completion_signal));
			break;
		case PipelineScheduleStageType::FINISH:
			events.push_back(make_shared_ptr<RecursiveSignallingEvent<PipelineFinishEvent>>(std::move(pipeline),
			                                                                                context.completion_signal));
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

void ExecuteRecursivePipelineFinishInline(Pipeline &pipeline, Executor &executor) {
	auto finish_event = make_shared_ptr<RecursiveCTEFinishEvent>(pipeline.shared_from_this());
	auto complete_event = make_shared_ptr<PipelineCompleteEvent>(executor, false);
	complete_event->AddDependency(*finish_event);
	finish_event->Schedule();
	if (!complete_event->IsFinished()) {
		WaitForRecursiveEvent(executor, *complete_event);
	}
}

template <bool COLLECT_METRICS>
static void ExecuteRecursiveInlinePlanInternal(RecursiveSchedulingContext context,
                                               const RecursiveCTEPipelineSchedulePlan &plan) {
	for (auto &pipeline : plan.initialize_on_schedule_pipelines) {
		pipeline.get().ResetSource(true);
	}

	context.scheduler.InitializeInlinePlan(plan);

	for (idx_t ready_idx = 0; ready_idx < context.scheduler.ReadyStageCount(); ready_idx++) {
		auto stage_idx = context.scheduler.ReadyStage(ready_idx);
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
			const auto worker_limit = stage.is_invariant_build
			                              ? TaskScheduler::GetScheduler(context.executor.context).NumberOfThreads()
			                              : idx_t(1);
			const auto max_threads = GetRecursivePipelineMaxThreads(pipeline, worker_limit);
			if (max_threads > 1) {
				ExecuteRecursivePipelineEventInline(pipeline, context, worker_limit, max_threads, stage.metric_type);
				break;
			}
			D_ASSERT(max_threads == 1);
			AdjustRecursiveSinkWorkers(pipeline, max_threads);
			context.scheduler.PrepareExecutors(pipeline, max_threads);
			auto &executors = context.scheduler.GetExecutors(pipeline);
			D_ASSERT(executors.size() >= max_threads);
			executors[0]->PrepareForExecution();
			if constexpr (COLLECT_METRICS) {
				context.metrics->RecordTasks(1);
			}
			const auto execute_start =
			    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			ExecuteRecursivePipelineInline(*executors[0], context.executor);
			if constexpr (COLLECT_METRICS) {
				const auto execute_end = std::chrono::steady_clock::now();
				context.epoch_metrics->RecordPipelineExecution(
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
			if (stage.trivial_finish) {
				// the sink has no finalize work and no operator on this pipeline requires
				// finalization, so the finish event machinery is skipped
				pipeline.GetSink()->sink_state->state = SinkFinalizeType::READY;
				break;
			}
			ExecuteRecursivePipelineFinishInline(pipeline, context.executor);
			break;
		default:
			throw InternalException("Unsupported recursive inline stage");
		}

		context.scheduler.CompleteInlineStage(plan, stage_idx);
	}

	if (context.scheduler.ReadyStageCount() != plan.stages.size()) {
		throw InternalException("Recursive inline plan did not schedule every stage");
	}
}

void ExecuteRecursiveInlinePlan(const RecursiveSchedulingContext &context,
                                const RecursiveCTEPipelineSchedulePlan &plan) {
	if (context.metrics) {
		ExecuteRecursiveInlinePlanInternal<true>(context, plan);
	} else {
		ExecuteRecursiveInlinePlanInternal<false>(context, plan);
	}
}

void GatherColumnDataScans(const PhysicalOperator &op, vector<const_reference<PhysicalOperator>> &delim_scans) {
	if (op.type == PhysicalOperatorType::DELIM_SCAN || op.type == PhysicalOperatorType::CTE_SCAN) {
		delim_scans.push_back(op);
	}
	for (auto child : op.GetChildren()) {
		GatherColumnDataScans(child.get(), delim_scans);
	}
}

//! One scan-discovery traversal for every CTE scan kind: walks the owned children plus
//! the delim join's hidden join and distinct operators, deduplicated by a visited set
static void GatherCTEScansInternal(PhysicalOperator &op, PhysicalOperatorType scan_type, TableIndex cte_index,
                                   vector<reference<PhysicalColumnDataScan>> &scans,
                                   reference_set_t<const PhysicalOperator> &visited) {
	if (!visited.insert(op).second) {
		return;
	}
	if (op.type == scan_type) {
		auto &scan = op.Cast<PhysicalColumnDataScan>();
		if (scan.cte_index == cte_index) {
			scans.push_back(scan);
		}
	}
	for (auto &child : op.children) {
		GatherCTEScansInternal(child.get(), scan_type, cte_index, scans, visited);
	}
	if (op.type == PhysicalOperatorType::LEFT_DELIM_JOIN || op.type == PhysicalOperatorType::RIGHT_DELIM_JOIN) {
		auto &delim_join = op.Cast<PhysicalDelimJoin>();
		GatherCTEScansInternal(delim_join.join, scan_type, cte_index, scans, visited);
		GatherCTEScansInternal(delim_join.distinct, scan_type, cte_index, scans, visited);
	}
}

void GatherRecursiveScans(PhysicalOperator &op, TableIndex cte_index,
                          vector<reference<PhysicalColumnDataScan>> &recursive_scans) {
	reference_set_t<const PhysicalOperator> visited;
	GatherCTEScansInternal(op, PhysicalOperatorType::RECURSIVE_CTE_SCAN, cte_index, recursive_scans, visited);
}

void GatherFrontierScans(PhysicalOperator &op, TableIndex cte_index,
                         vector<reference<PhysicalColumnDataScan>> &frontier_scans) {
	reference_set_t<const PhysicalOperator> visited;
	GatherCTEScansInternal(op, PhysicalOperatorType::TRAMPOLINE_FRONTIER_SCAN, cte_index, frontier_scans, visited);
}

} // namespace duckdb
