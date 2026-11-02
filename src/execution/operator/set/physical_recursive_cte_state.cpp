#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"

namespace duckdb {

RecursiveCTEPartialKeyIndex::RecursiveCTEPartialKeyIndex(Allocator &allocator,
                                                         const vector<LogicalType> &full_key_types,
                                                         vector<idx_t> key_indices_p)
    : key_indices(std::move(key_indices_p)), hashes(LogicalType::HASH) {
	vector<LogicalType> partial_key_types;
	for (auto key_idx : key_indices) {
		D_ASSERT(key_idx < full_key_types.size());
		partial_key_types.push_back(full_key_types[key_idx]);
	}
	partial_keys.Initialize(allocator, partial_key_types);
	selected_keys.Initialize(allocator, partial_key_types);
	Resize(1024);
}

void RecursiveCTEPartialKeyIndex::Resize(idx_t capacity) {
	D_ASSERT(capacity > 0 && (capacity & (capacity - 1)) == 0);
	heads.assign(capacity, DConstants::INVALID_INDEX);
	for (idx_t entry_idx = 0; entry_idx < entries.size(); entry_idx++) {
		auto &entry = entries[entry_idx];
		const auto bucket = entry.hash & (capacity - 1);
		entry.next = heads[bucket];
		heads[bucket] = entry_idx;
	}
}

void RecursiveCTEPartialKeyIndex::AddGroups(DataChunk &full_keys, const SelectionVector &key_selection,
                                            Vector &group_addresses, const SelectionVector &address_selection,
                                            idx_t group_count) {
	if (group_count == 0) {
		return;
	}
	while (entries.size() + group_count > heads.size()) {
		Resize(heads.size() * 2);
	}
	partial_keys.Reset();
	for (idx_t partial_idx = 0; partial_idx < key_indices.size(); partial_idx++) {
		partial_keys.data[partial_idx].Reference(full_keys.data[key_indices[partial_idx]]);
	}
	partial_keys.CheckCardinality(full_keys.size());
	selected_keys.Reset();
	selected_keys.Slice(partial_keys, key_selection, group_count);
	selected_keys.Hash(hashes);

	const auto hash_values = hashes.Values<hash_t>();
	const auto addresses = FlatVector::GetData<data_ptr_t>(group_addresses);
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		const auto hash = hash_values[group_idx].GetValue();
		const auto bucket = hash & (heads.size() - 1);
		entries.push_back({hash, addresses[address_selection.get_index(group_idx)], heads[bucket]});
		heads[bucket] = entries.size() - 1;
	}
}

idx_t RecursiveCTEPartialKeyIndex::GetHead(hash_t hash) const {
	return heads[hash & (heads.size() - 1)];
}

const RecursiveCTEPartialKeyIndex::Entry &RecursiveCTEPartialKeyIndex::GetEntry(idx_t entry_idx) const {
	return entries[entry_idx];
}

idx_t RecursiveCTEPartialKeyIndex::Count() const {
	return entries.size();
}

idx_t RecursiveCTEPartialKeyIndex::SizeInBytes() const {
	return heads.capacity() * sizeof(idx_t) + entries.capacity() * sizeof(Entry);
}

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
    : logger(context.logger),
      enabled(logger && logger->ShouldLog(PhysicalOperatorLogType::NAME, PhysicalOperatorLogType::LEVEL)) {
	if (enabled) {
		identity = make_uniq<RecursiveCTELogIdentity>(op_p.type, op_p.NextMetricsInvocation());
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
	           "PhysicalRecursiveCTE", "DistinctPromoted",
	           {{"invocation_id", to_string(identity->invocation_id)},
	            {"partitions", to_string(partitions)},
	            {"migrated_rows", to_string(migrated_rows)},
	            {"elapsed_us", to_string(elapsed_us_p)}});
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
	           "PhysicalRecursiveCTE", "RuntimeMetrics",
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
	    logger, PhysicalOperatorLogType, identity->operator_type, identity->operator_parameters, "PhysicalRecursiveCTE",
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

} // namespace duckdb
