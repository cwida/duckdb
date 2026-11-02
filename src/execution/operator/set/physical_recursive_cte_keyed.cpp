#include "duckdb/execution/operator/set/physical_recursive_cte_keyed_state.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_delta.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

RecursiveCTEKeyedProperties RecursiveCTEKeyedProperties::Derive(const PhysicalRecursiveCTE &op) {
	D_ASSERT(op.UsingKey());
	RecursiveCTEKeyedProperties result;
	for (auto &comparison : op.payload_comparisons) {
		if (comparison) {
			result.has_payload_comparisons = true;
			break;
		}
	}
	if (op.UnionAll()) {
		return result;
	}
	result.can_preaggregate = true;
	for (idx_t payload_idx = 0; payload_idx < op.payload_types.size(); payload_idx++) {
		auto &aggregate = op.payload_aggregates[payload_idx]->Cast<BoundAggregateExpression>();
		if (!aggregate.Function().HasStateCombineCallback() ||
		    aggregate.Function().GetOrderDependent() == AggregateOrderDependent::ORDER_DEPENDENT) {
			result.can_preaggregate = false;
			break;
		}
	}
	result.can_reuse_new_group_candidates = op.internal_types == op.GetTypes();
	for (idx_t payload_idx = 0; result.can_reuse_new_group_candidates && payload_idx < op.payload_types.size();
	     payload_idx++) {
		auto &aggregate = op.payload_aggregates[payload_idx]->Cast<BoundAggregateExpression>();
		auto &children = aggregate.GetChildren();
		result.can_reuse_new_group_candidates =
		    aggregate.Function().HasSingleValueIdentity() && !children.empty() &&
		    children[0]->GetExpressionClass() == ExpressionClass::BOUND_REF &&
		    children[0]->Cast<BoundReferenceExpression>().Index() == op.payload_idx[payload_idx];
	}
	result.can_reuse_changed_group_candidates =
	    result.can_reuse_new_group_candidates && op.key_normalizers.empty() && !result.has_payload_comparisons;
	for (auto &key_type : op.hash_key_types) {
		switch (key_type.InternalType()) {
		case PhysicalType::FLOAT:
		case PhysicalType::DOUBLE:
		case PhysicalType::LIST:
		case PhysicalType::STRUCT:
		case PhysicalType::ARRAY:
		case PhysicalType::UNKNOWN:
			result.can_reuse_changed_group_candidates = false;
			break;
		default:
			break;
		}
	}
	for (auto &payload_type : op.payload_types) {
		switch (payload_type.InternalType()) {
		case PhysicalType::LIST:
		case PhysicalType::STRUCT:
		case PhysicalType::ARRAY:
		case PhysicalType::UNKNOWN:
			result.can_reuse_changed_group_candidates = false;
			break;
		default:
			break;
		}
	}
	return result;
}

RecursiveCTEKeyedCandidateExtractor::RecursiveCTEKeyedCandidateExtractor(ClientContext &context,
                                                                         const PhysicalRecursiveCTE &op)
    : op(op), payload_executor(context) {
	for (auto &payload_aggregate : op.payload_aggregates) {
		D_ASSERT(payload_aggregate->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
		auto &bound_aggregate = payload_aggregate->Cast<BoundAggregateExpression>();
		for (auto &child : bound_aggregate.GetChildren()) {
			payload_executor.AddExpression(*child);
			payload_types.push_back(child->GetReturnType());
		}
	}
	if (!op.key_normalizers.empty()) {
		key_executor = make_uniq<ExpressionExecutor>(context);
		for (auto &normalizer : op.key_normalizers) {
			key_executor->AddExpression(*normalizer);
		}
		raw_keys.Initialize(Allocator::Get(context), op.distinct_types);
	}
}

void RecursiveCTEKeyedCandidateExtractor::ExtractKeys(DataChunk &source, DataChunk &keys) {
	keys.Reset();
	if (!key_executor) {
		for (idx_t key_idx = 0; key_idx < op.distinct_idx.size(); key_idx++) {
			keys.data[key_idx].Reference(source.data[op.distinct_idx[key_idx]]);
		}
		keys.CheckCardinality(source.size());
		return;
	}
	raw_keys.Reset();
	for (idx_t key_idx = 0; key_idx < op.distinct_idx.size(); key_idx++) {
		raw_keys.data[key_idx].Reference(source.data[op.distinct_idx[key_idx]]);
	}
	raw_keys.CheckCardinality(source.size());
	key_executor->Execute(raw_keys, keys);
}

void RecursiveCTEKeyedCandidateExtractor::EvaluatePayload(DataChunk &source, DataChunk &payload) {
	if (payload_executor.expressions.empty()) {
		return;
	}
	payload.Reset();
	payload_executor.Execute(source, payload);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
RecursiveCTEKeyedState::RecursiveCTEKeyedState(ClientContext &context, const PhysicalRecursiveCTE &op)
    : RecursiveCTEState(context, op), extractor(context, op), new_group_addresses(LogicalType::POINTER),
      new_groups(STANDARD_VECTOR_SIZE), intermediate_table(context, op.internal_types),
      preaggregation_hashes(LogicalType::HASH, nullptr, 0) {
	for (idx_t i = 0; i < op.payload_aggregates.size(); i++) {
		D_ASSERT(op.payload_aggregates[i]->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
		auto &bound_aggr_expr = op.payload_aggregates[i]->Cast<BoundAggregateExpression>();
		payload_aggregate_objects.emplace_back(bound_aggr_expr);
	}

	payload_rows.Initialize(Allocator::Get(context), extractor.PayloadTypes());
	for (auto &comparison : op.payload_comparisons) {
		if (comparison) {
			payload_comparison_executors.push_back(make_uniq<ExpressionExecutor>(context, *comparison));
		} else {
			payload_comparison_executors.push_back(nullptr);
		}
	}

	ht = CreateUsingKeyHashTable();
	for (auto &spec : op.partial_key_index_specs) {
		partial_key_indexes.push_back(
		    make_uniq<RecursiveCTEPartialKeyIndex>(Allocator::Get(context), op.hash_key_types, spec.Indices()));
	}
	if (op.keyed.can_preaggregate) {
		preaggregation_hashes.Initialize();
	}
	if (!op.UnionAll()) {
		key_delta = make_uniq<RecursiveCTEKeyDeltaState>(context, op);
	}
	distinct_rows.Initialize(Allocator::DefaultAllocator(), op.hash_key_types);
	update_rows.Initialize(Allocator::DefaultAllocator(), op.internal_types);
	source_distinct_rows.Initialize(Allocator::DefaultAllocator(), op.hash_key_types);
	source_aggregate_rows.Initialize(Allocator::DefaultAllocator(), op.aggregate_types);
	InitializeIntermediateAppend();
	op.working_table->InitializeAppend(working_append_state);
}

RecursiveCTEKeyedState::~RecursiveCTEKeyedState() {
	LogMetrics(partial_key_indexes);
}

unique_ptr<GroupedAggregateHashTable> RecursiveCTEKeyedState::CreateUsingKeyHashTable() const {
	return make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), op.hash_key_types,
	                                            op.aggregate_types, payload_aggregate_objects);
}

const RecursiveCTEPartialKeyIndex &RecursiveCTEKeyedState::GetPartialKeyIndex(const vector<idx_t> &key_indices) const {
	for (auto &index : partial_key_indexes) {
		if (index->key_indices == key_indices) {
			return *index;
		}
	}
	throw InternalException("USING KEY partial-key index is missing");
}

void RecursiveCTEKeyedState::AppendOutput(DataChunk &chunk) {
	const auto collect_metrics = metrics.Enabled();
	const auto before_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	lock_guard<mutex> guard(intermediate_table_lock);
	const auto after_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	// USING KEY collects updates without mutating the hash state read by recurring.T in this iteration.
	intermediate_table.Append(intermediate_append_state, chunk);
	if (collect_metrics) {
		const auto after_work = std::chrono::steady_clock::now();
		RecordSinkMetrics(
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_lock - before_lock).count()),
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()),
		    chunk.size());
	}
}

void RecursiveCTEKeyedState::CombineOutput(ColumnDataCollection &output) {
	const auto collect_metrics = metrics.Enabled();
	const auto before_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	lock_guard<mutex> guard(intermediate_table_lock);
	const auto after_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	const auto row_count = output.Count();
	intermediate_table.Combine(output);
	// Keyed workers can resume shared appends after publishing local output.
	InitializeIntermediateAppend();
	if (collect_metrics) {
		const auto after_work = std::chrono::steady_clock::now();
		RecordSinkMetrics(
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_lock - before_lock).count()),
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()),
		    row_count);
	}
}

void RecursiveCTEKeyedState::RegisterLocalPreaggregation(unique_ptr<GroupedAggregateHashTable> local_ht,
                                                         idx_t candidate_rows, idx_t classification_work_ns,
                                                         idx_t preaggregation_work_ns) {
	D_ASSERT(local_ht && candidate_rows > 0 && local_ht->Count() <= candidate_rows);
	const auto group_count = local_ht->Count();
	{
		lock_guard<mutex> guard(intermediate_table_lock);
		local_preaggregate_candidate_count += candidate_rows;
		local_preaggregates.push_back(std::move(local_ht));
	}
	if (metrics.Enabled()) {
		metrics.RecordHashRows(candidate_rows);
		GetEpochMetrics().RecordLocalKeyPreaggregationClassification(classification_work_ns);
		GetEpochMetrics().RecordLocalKeyPreaggregation(candidate_rows, group_count, preaggregation_work_ns);
	}
}

void RecursiveCTEKeyedState::AssembleStateRows(DataChunk &keys, DataChunk &aggregates, DataChunk &result) const {
	result.Reset();
	for (idx_t key_idx = 0; key_idx < op.distinct_idx.size(); key_idx++) {
		const auto representative_idx = op.key_representative_indices[key_idx];
		auto &source =
		    representative_idx == DConstants::INVALID_INDEX ? keys.data[key_idx] : aggregates.data[representative_idx];
		result.data[op.distinct_idx[key_idx]].Reference(source);
	}
	for (idx_t payload_idx = 0; payload_idx < op.payload_idx.size(); payload_idx++) {
		result.data[op.payload_idx[payload_idx]].Reference(aggregates.data[payload_idx]);
	}
	result.CheckCardinality(keys.size());
}

void RecursiveCTEKeyedState::FinalizeStateRows(RowOperationsState &row_state, Vector &addresses, DataChunk &keys,
                                               DataChunk &aggregates, DataChunk &result) {
	FinalizeAggregateRows(row_state, addresses, aggregates, keys.size());
	AssembleStateRows(keys, aggregates, result);
}

void RecursiveCTEKeyedState::FinalizeAggregateRows(RowOperationsState &row_state, Vector &addresses,
                                                   DataChunk &aggregates, idx_t count) {
	aggregates.Reset();
	aggregates.SetChildCardinality(count);
	{
		lock_guard<mutex> guard(ht_finalize_lock);
		RowOperations::FinalizeStates(row_state, *GetHashTable().GetLayoutPtr(), addresses, aggregates, 0);
	}
}

void RecursiveCTEKeyedState::InitializeIntermediateAppend() {
	intermediate_table.InitializeAppend(intermediate_append_state);
}

enum class RecursiveCTELocalPreaggregationDecision : uint8_t { DEFER, PREAGGREGATE, DIRECT };

class RecursiveCTELocalPreaggregationState {
public:
	RecursiveCTELocalPreaggregationState(ClientContext &context_p, const PhysicalRecursiveCTE &op_p)
	    : context(context_p), op(op_p), extractor(context, op), hashes(LogicalType::HASH) {
		for (auto &payload_aggregate : op.payload_aggregates) {
			auto &bound_aggregate = payload_aggregate->Cast<BoundAggregateExpression>();
			aggregates.emplace_back(bound_aggregate);
		}
		keys.Initialize(Allocator::Get(context), op.hash_key_types);
		payload.Initialize(Allocator::Get(context), extractor.PayloadTypes());
		input.Initialize(Allocator::Get(context), op.internal_types);
	}

	RecursiveCTELocalPreaggregationDecision Classify(ColumnDataCollection &candidates) {
		const auto candidate_count = candidates.Count();
		D_ASSERT(candidate_count >= STANDARD_VECTOR_SIZE);
		sampled_candidate_count += candidate_count;
		ColumnDataScanState scan_state;
		candidates.InitializeScan(scan_state);
		while (candidates.Scan(scan_state, input)) {
			extractor.ExtractKeys(input, keys);
			keys.Hash(hashes);
			cardinality.Update(hashes);
		}
		const auto distinct_upper_bound =
		    LossyNumericCast<idx_t>((1 + HyperLogLog::GetErrorRate()) * static_cast<double>(cardinality.Count()));
		if (distinct_upper_bound < sampled_candidate_count / 4) {
			return RecursiveCTELocalPreaggregationDecision::PREAGGREGATE;
		}
		// Keep sampling across vector boundaries, but bound the extra hashing for unique streams.
		static constexpr idx_t MAX_SAMPLE_VECTORS = 8;
		const auto sample_size = MaxValue<idx_t>(STANDARD_VECTOR_SIZE, 16);
		if (sampled_candidate_count >= sample_size * MAX_SAMPLE_VECTORS) {
			return RecursiveCTELocalPreaggregationDecision::DIRECT;
		}
		return RecursiveCTELocalPreaggregationDecision::DEFER;
	}

	void ResetClassification() {
		cardinality = HyperLogLog();
		sampled_candidate_count = 0;
	}

	unique_ptr<GroupedAggregateHashTable> Preaggregate(ColumnDataCollection &candidates) {
		auto result = make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), op.hash_key_types,
		                                                   op.aggregate_types, aggregates);
		ColumnDataScanState scan_state;
		candidates.InitializeScan(scan_state);
		while (candidates.Scan(scan_state, input)) {
			extractor.ExtractKeys(input, keys);
			extractor.EvaluatePayload(input, payload);
			result->AddChunk(keys, payload, AggregateType::NON_DISTINCT);
		}
		return result;
	}

private:
	ClientContext &context;
	const PhysicalRecursiveCTE &op;
	RecursiveCTEKeyedCandidateExtractor extractor;
	vector<AggregateObject> aggregates;
	DataChunk keys;
	DataChunk payload;
	DataChunk input;
	Vector hashes;
	HyperLogLog cardinality;
	idx_t sampled_candidate_count = 0;
};

class RecursiveCTEKeyedLocalState : public LocalSinkState {
public:
	RecursiveCTEKeyedLocalState(ClientContext &context, const PhysicalRecursiveCTE &op) : context(context), op(op) {
	}

	ClientContext &context;
	const PhysicalRecursiveCTE &op;
	unique_ptr<ColumnDataCollection> output;
	ColumnDataAppendState append_state;
	idx_t using_key_candidate_count = 0;
	idx_t using_key_classification_work_ns = 0;
	bool buffer_using_key_output = false;
	bool direct_using_key_output = false;
	unique_ptr<RecursiveCTELocalPreaggregationState> using_key_preaggregation;

	void SinkUsingKeyOutput(DataChunk &chunk, RecursiveCTEKeyedState &gstate) {
		D_ASSERT(!op.UnionAll());
		using_key_candidate_count += chunk.size();
		if (buffer_using_key_output) {
			BufferUsingKeyOutput(chunk);
			return;
		}
		const auto sample_size = MaxValue<idx_t>(STANDARD_VECTOR_SIZE, 16);
		const auto coalesce_small_chunks = chunk.size() < sample_size && (using_key_candidate_count >= sample_size ||
		                                                                  gstate.CurrentInputCount() >= sample_size);
		if (direct_using_key_output || !gstate.CanPreaggregateUsingKey()) {
			if (coalesce_small_chunks) {
				BufferUsingKeyOutput(chunk);
			} else {
				gstate.AppendOutput(chunk);
			}
			return;
		}
		if (using_key_candidate_count <= gstate.CurrentInputCount() && !coalesce_small_chunks) {
			gstate.AppendOutput(chunk);
			return;
		}
		BufferUsingKeyOutput(chunk);
		if (output->Count() < sample_size) {
			return;
		}
		ClassifyBufferedUsingKeyOutput();
		if (!buffer_using_key_output) {
			gstate.CombineOutput(*output);
			output->ResetForReuse();
			output->InitializeAppend(append_state);
		}
	}

	void ClassifyBufferedUsingKeyOutput() {
		D_ASSERT(output && output->Count() >= STANDARD_VECTOR_SIZE && !buffer_using_key_output &&
		         !direct_using_key_output);
		if (!using_key_preaggregation) {
			using_key_preaggregation = make_uniq<RecursiveCTELocalPreaggregationState>(context, op);
		}
		const auto classification_start = std::chrono::steady_clock::now();
		const auto decision = using_key_preaggregation->Classify(*output);
		const auto classification_end = std::chrono::steady_clock::now();
		using_key_classification_work_ns += NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(classification_end - classification_start).count());
		buffer_using_key_output = decision == RecursiveCTELocalPreaggregationDecision::PREAGGREGATE;
		direct_using_key_output = decision == RecursiveCTELocalPreaggregationDecision::DIRECT;
	}

	void BufferUsingKeyOutput(DataChunk &chunk) {
		D_ASSERT(!op.UnionAll());
		if (!output) {
			output = make_uniq<ColumnDataCollection>(context, op.internal_types);
			output->InitializeAppend(append_state);
		}
		output->Append(append_state, chunk);
	}

	unique_ptr<GroupedAggregateHashTable> Preaggregate(idx_t &preaggregation_work_ns) {
		D_ASSERT(output && !op.UnionAll());
		D_ASSERT(buffer_using_key_output && using_key_preaggregation);
		const auto preaggregation_start = std::chrono::steady_clock::now();
		auto result = using_key_preaggregation->Preaggregate(*output);
		const auto preaggregation_end = std::chrono::steady_clock::now();
		preaggregation_work_ns = NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(preaggregation_end - preaggregation_start).count());
		return result;
	}

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ExecutionContext &context, GlobalSinkState &gstate) override {
		using_key_candidate_count = 0;
		using_key_classification_work_ns = 0;
		buffer_using_key_output = false;
		direct_using_key_output = false;
		if (using_key_preaggregation) {
			using_key_preaggregation->ResetClassification();
		}
		if (!output) {
			return;
		}
		output->ResetForReuse();
		output->InitializeAppend(append_state);
	}
};

unique_ptr<LocalSinkState> RecursiveCTEKeyedState::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<RecursiveCTEKeyedLocalState>(context.client, op);
}

bool RecursiveCTEKeyedState::ShouldPreaggregateUsingKeyUpdates(idx_t candidate_count) {
	D_ASSERT(op.keyed.can_preaggregate && candidate_count >= STANDARD_VECTOR_SIZE);
	if (op.working_table->Count() >= candidate_count) {
		return false;
	}
	HyperLogLog key_cardinality;
	const auto group_limit = candidate_count / 4;
	ColumnDataScanState sample_scan_state;
	intermediate_table.InitializeScan(sample_scan_state);
	while (intermediate_table.Scan(sample_scan_state, update_rows)) {
		extractor.ExtractKeys(update_rows, distinct_rows);
		distinct_rows.Hash(preaggregation_hashes);
		key_cardinality.Update(preaggregation_hashes);
		const auto distinct_upper_bound =
		    LossyNumericCast<idx_t>((1 + HyperLogLog::GetErrorRate()) * static_cast<double>(key_cardinality.Count()));
		if (distinct_upper_bound >= group_limit) {
			return false;
		}
	}
	// A second keyed hash table only pays off when the estimated fan-in is substantial.
	return true;
}

template <bool COLLECT_METRICS>
void RecursiveCTEKeyedState::CommitUsingKeyUpdatesInternal() {
	if (!local_preaggregates.empty()) {
		D_ASSERT(!op.UnionAll() && local_preaggregate_candidate_count > 0);
		const auto combine_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		auto epoch_ht = std::move(local_preaggregates[0]);
		for (idx_t local_idx = 1; local_idx < local_preaggregates.size(); local_idx++) {
			epoch_ht->Combine(*local_preaggregates[local_idx]);
		}
		local_preaggregates.clear();
		auto preaggregated_candidate_count = local_preaggregate_candidate_count;
		local_preaggregate_candidate_count = 0;
		if constexpr (COLLECT_METRICS) {
			const auto combine_end = std::chrono::steady_clock::now();
			GetEpochMetrics().RecordKeyPreaggregationCombine(NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(combine_end - combine_start).count()));
		}

		const auto raw_candidate_count = intermediate_table.Count();
		if constexpr (COLLECT_METRICS) {
			GetEpochMetrics().RecordLocalKeyPreaggregationResidual(raw_candidate_count);
		}
		bool preaggregate_raw_candidates = false;
		if (raw_candidate_count >= STANDARD_VECTOR_SIZE) {
			const auto classification_start =
			    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			preaggregate_raw_candidates = ShouldPreaggregateUsingKeyUpdates(raw_candidate_count);
			if constexpr (COLLECT_METRICS) {
				const auto classification_end = std::chrono::steady_clock::now();
				GetEpochMetrics().RecordKeyPreaggregationClassification(NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(classification_end - classification_start)
				        .count()));
			}
		}
		if (preaggregate_raw_candidates) {
			auto raw_ht = CreateUsingKeyHashTable();
			const auto preaggregation_work_ns = PreaggregateUsingKeyUpdates<COLLECT_METRICS>(*raw_ht);
			if constexpr (COLLECT_METRICS) {
				GetEpochMetrics().RecordKeyPreaggregation(raw_candidate_count, raw_ht->Count(), preaggregation_work_ns);
			}
			const auto raw_combine_start =
			    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			epoch_ht->Combine(*raw_ht);
			if constexpr (COLLECT_METRICS) {
				const auto raw_combine_end = std::chrono::steady_clock::now();
				GetEpochMetrics().RecordKeyPreaggregationCombine(NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(raw_combine_end - raw_combine_start).count()));
			}
			preaggregated_candidate_count += raw_candidate_count;
			intermediate_table.ResetForReuse();
			InitializeIntermediateAppend();
		}
		CommitMixedUsingKeyUpdatesInternal<COLLECT_METRICS>(std::move(epoch_ht), preaggregated_candidate_count);
		return;
	}
	const auto candidate_count = intermediate_table.Count();
	bool use_preaggregation = false;
	if (op.keyed.can_preaggregate && candidate_count >= STANDARD_VECTOR_SIZE) {
		const auto classification_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		use_preaggregation = ShouldPreaggregateUsingKeyUpdates(candidate_count);
		if constexpr (COLLECT_METRICS) {
			const auto classification_end = std::chrono::steady_clock::now();
			GetEpochMetrics().RecordKeyPreaggregationClassification(NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(classification_end - classification_start)
			        .count()));
		}
	}
	if (use_preaggregation) {
		CommitPreaggregatedUsingKeyUpdatesInternal<COLLECT_METRICS>();
		return;
	}
	const auto delta_candidate_count = op.UnionAll() ? idx_t(0) : candidate_count;
	idx_t delta_work_ns = 0;
	if (!op.UnionAll()) {
		D_ASSERT(key_delta);
		const auto delta_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		key_delta->Reset();
		if constexpr (COLLECT_METRICS) {
			const auto delta_end = std::chrono::steady_clock::now();
			delta_work_ns += NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(delta_end - delta_start).count());
		}
	}
	ColumnDataScanState update_scan_state;
	intermediate_table.InitializeScan(update_scan_state);
	while (intermediate_table.Scan(update_scan_state, update_rows)) {
		if constexpr (COLLECT_METRICS) {
			metrics.RecordHashRows(update_rows.size());
		}
		const auto hash_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		idx_t snapshot_work_ns = 0;
		extractor.ExtractKeys(update_rows, distinct_rows);
		extractor.EvaluatePayload(update_rows, payload_rows);
		if (!op.UnionAll()) {
			const auto new_group_count = ht->AddChunk(
			    distinct_rows, payload_rows, AggregateType::NON_DISTINCT,
			    [&](const Vector &group_addresses, const SelectionVector &new_groups, idx_t new_group_count) {
				    const auto delta_start =
				        COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
				    SnapshotUsingKeyDelta(group_addresses, new_groups, new_group_count, update_rows.size());
				    if constexpr (COLLECT_METRICS) {
					    const auto delta_end = std::chrono::steady_clock::now();
					    snapshot_work_ns = NumericCast<idx_t>(
					        std::chrono::duration_cast<std::chrono::nanoseconds>(delta_end - delta_start).count());
					    delta_work_ns += snapshot_work_ns;
				    }
			    });
			if (key_delta->deferred_previous_rows) {
				const auto delta_start =
				    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
				ValidateDeferredUsingKeyCandidateReuse(update_rows);
				if constexpr (COLLECT_METRICS) {
					const auto delta_end = std::chrono::steady_clock::now();
					const auto elapsed_ns = NumericCast<idx_t>(
					    std::chrono::duration_cast<std::chrono::nanoseconds>(delta_end - delta_start).count());
					snapshot_work_ns += elapsed_ns;
					delta_work_ns += elapsed_ns;
				}
			}
			if constexpr (COLLECT_METRICS) {
				const auto hash_end = std::chrono::steady_clock::now();
				const auto hash_work_ns = NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(hash_end - hash_start).count());
				D_ASSERT(snapshot_work_ns <= hash_work_ns);
				GetEpochMetrics().RecordKeyedHashCommit(hash_work_ns - snapshot_work_ns);
			}
			const auto index_start =
			    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			for (auto &index : partial_key_indexes) {
				index->AddGroups(distinct_rows, new_groups, new_group_addresses,
				                 *FlatVector::IncrementalSelectionVector(), new_group_count);
			}
			if constexpr (COLLECT_METRICS) {
				const auto index_end = std::chrono::steady_clock::now();
				const auto elapsed_ns = NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(index_end - index_start).count());
				GetEpochMetrics().RecordPartialIndexMaintenance(elapsed_ns);
				metrics.RecordPartialIndexBuild(NumericCast<idx_t>(elapsed_ns / 1000));
			}
			continue;
		}
		if (partial_key_indexes.empty()) {
			ht->AddChunk(distinct_rows, payload_rows, AggregateType::NON_DISTINCT);
			if constexpr (COLLECT_METRICS) {
				const auto hash_end = std::chrono::steady_clock::now();
				const auto hash_work_ns = NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(hash_end - hash_start).count());
				D_ASSERT(snapshot_work_ns <= hash_work_ns);
				GetEpochMetrics().RecordKeyedHashCommit(hash_work_ns - snapshot_work_ns);
			}
			continue;
		}
		const auto new_group_count = ht->AddChunkAndGetNewGroups(
		    distinct_rows, payload_rows, AggregateType::NON_DISTINCT, new_group_addresses, new_groups);
		if constexpr (COLLECT_METRICS) {
			const auto hash_end = std::chrono::steady_clock::now();
			const auto hash_work_ns =
			    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(hash_end - hash_start).count());
			D_ASSERT(snapshot_work_ns <= hash_work_ns);
			GetEpochMetrics().RecordKeyedHashCommit(hash_work_ns - snapshot_work_ns);
		}
		const auto index_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		for (auto &index : partial_key_indexes) {
			index->AddGroups(distinct_rows, new_groups, new_group_addresses, *FlatVector::IncrementalSelectionVector(),
			                 new_group_count);
		}
		if constexpr (COLLECT_METRICS) {
			const auto index_end = std::chrono::steady_clock::now();
			const auto elapsed_ns = NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(index_end - index_start).count());
			GetEpochMetrics().RecordPartialIndexMaintenance(elapsed_ns);
			metrics.RecordPartialIndexBuild(NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::microseconds>(index_end - index_start).count()));
		}
	}
	if (!op.UnionAll()) {
		const auto delta_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		if (op.keyed.can_reuse_new_group_candidates && key_delta->new_count == delta_candidate_count) {
			op.working_table->Reset();
			op.working_table->Combine(intermediate_table);
			InitializeIntermediateAppend();
		} else if (TryReuseChangedGroupCandidates(delta_candidate_count)) {
			op.working_table->Reset();
			op.working_table->Combine(intermediate_table);
			InitializeIntermediateAppend();
		} else {
			op.working_table->ResetForReuse();
			op.working_table->InitializeAppend(working_append_state);
			FinalizeUsingKeyDelta(false, COLLECT_METRICS);
			intermediate_table.ResetForReuse();
			InitializeIntermediateAppend();
		}
		if constexpr (COLLECT_METRICS) {
			const auto delta_end = std::chrono::steady_clock::now();
			delta_work_ns += NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(delta_end - delta_start).count());
			GetEpochMetrics().RecordKeyDelta(delta_candidate_count, key_delta->touched_count, key_delta->new_count,
			                                 key_delta->changed_count, delta_work_ns);
		}
	}
}

template <bool COLLECT_METRICS>
void RecursiveCTEKeyedState::ApplyPreaggregatedUsingKeyUpdates(GroupedAggregateHashTable &epoch_ht,
                                                               idx_t &delta_work_ns) {
	AggregateHTScanState epoch_scan_state;
	epoch_ht.InitializeScan(epoch_scan_state);
	while (epoch_ht.ScanGroups(epoch_scan_state, distinct_rows)) {
		if (distinct_rows.size() == 0) {
			continue;
		}
		const auto snapshot_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		SnapshotPreaggregatedUsingKeyDeltaGroups(distinct_rows);
		if constexpr (COLLECT_METRICS) {
			const auto snapshot_end = std::chrono::steady_clock::now();
			delta_work_ns += NumericCast<idx_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(snapshot_end - snapshot_start).count());
		}
	}

	const auto combine_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	ht->Combine(epoch_ht);
	if constexpr (COLLECT_METRICS) {
		const auto combine_end = std::chrono::steady_clock::now();
		GetEpochMetrics().RecordKeyPreaggregationCombine(NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(combine_end - combine_start).count()));
	}
}

template <bool COLLECT_METRICS>
void RecursiveCTEKeyedState::CommitMixedUsingKeyUpdatesInternal(unique_ptr<GroupedAggregateHashTable> epoch_ht,
                                                                idx_t preaggregated_candidate_count) {
	D_ASSERT(!op.UnionAll() && key_delta && epoch_ht && preaggregated_candidate_count > 0);
	auto &delta = *key_delta;
	const auto raw_candidate_count = intermediate_table.Count();
	const auto delta_candidate_count = raw_candidate_count + preaggregated_candidate_count;
	idx_t delta_work_ns = 0;
	const auto reset_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	delta.Reset();
	if constexpr (COLLECT_METRICS) {
		const auto reset_end = std::chrono::steady_clock::now();
		delta_work_ns +=
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(reset_end - reset_start).count());
	}

	ColumnDataScanState update_scan_state;
	intermediate_table.InitializeScan(update_scan_state);
	while (intermediate_table.Scan(update_scan_state, update_rows)) {
		if constexpr (COLLECT_METRICS) {
			metrics.RecordHashRows(update_rows.size());
		}
		const auto hash_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		idx_t snapshot_work_ns = 0;
		extractor.ExtractKeys(update_rows, distinct_rows);
		extractor.EvaluatePayload(update_rows, payload_rows);
		ht->AddChunk(
		    distinct_rows, payload_rows, AggregateType::NON_DISTINCT,
		    [&](const Vector &group_addresses, const SelectionVector &new_groups, idx_t new_group_count) {
			    const auto snapshot_start =
			        COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			    SnapshotUsingKeyDelta(group_addresses, new_groups, new_group_count, update_rows.size(), false);
			    if constexpr (COLLECT_METRICS) {
				    const auto snapshot_end = std::chrono::steady_clock::now();
				    snapshot_work_ns = NumericCast<idx_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(snapshot_end - snapshot_start).count());
				    delta_work_ns += snapshot_work_ns;
			    }
		    });
		if constexpr (COLLECT_METRICS) {
			const auto hash_end = std::chrono::steady_clock::now();
			const auto hash_work_ns =
			    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(hash_end - hash_start).count());
			D_ASSERT(snapshot_work_ns <= hash_work_ns);
			GetEpochMetrics().RecordKeyedHashCommit(hash_work_ns - snapshot_work_ns);
		}
	}

	ApplyPreaggregatedUsingKeyUpdates<COLLECT_METRICS>(*epoch_ht, delta_work_ns);

	const auto finalize_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	op.working_table->ResetForReuse();
	op.working_table->InitializeAppend(working_append_state);
	const auto index_work_ns = FinalizeUsingKeyDelta(!partial_key_indexes.empty(), COLLECT_METRICS);
	intermediate_table.ResetForReuse();
	InitializeIntermediateAppend();
	if constexpr (COLLECT_METRICS) {
		const auto finalize_end = std::chrono::steady_clock::now();
		const auto finalize_work_ns = NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count());
		D_ASSERT(index_work_ns <= finalize_work_ns);
		delta_work_ns += finalize_work_ns - index_work_ns;
		GetEpochMetrics().RecordKeyDelta(delta_candidate_count, delta.touched_count, delta.new_count,
		                                 delta.changed_count, delta_work_ns);
	}
}

template <bool COLLECT_METRICS>
idx_t RecursiveCTEKeyedState::PreaggregateUsingKeyUpdates(GroupedAggregateHashTable &epoch_ht) {
	idx_t preaggregation_work_ns = 0;
	ColumnDataScanState update_scan_state;
	intermediate_table.InitializeScan(update_scan_state);
	while (intermediate_table.Scan(update_scan_state, update_rows)) {
		if constexpr (COLLECT_METRICS) {
			metrics.RecordHashRows(update_rows.size());
		}
		const auto hash_start =
		    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		extractor.ExtractKeys(update_rows, distinct_rows);
		extractor.EvaluatePayload(update_rows, payload_rows);
		epoch_ht.AddChunk(distinct_rows, payload_rows, AggregateType::NON_DISTINCT);
		if constexpr (COLLECT_METRICS) {
			const auto hash_end = std::chrono::steady_clock::now();
			preaggregation_work_ns +=
			    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(hash_end - hash_start).count());
		}
	}
	return preaggregation_work_ns;
}

template <bool COLLECT_METRICS>
void RecursiveCTEKeyedState::CommitPreaggregatedUsingKeyUpdatesInternal() {
	D_ASSERT(!op.UnionAll() && key_delta);
	auto &delta = *key_delta;
	const auto delta_candidate_count = intermediate_table.Count();
	idx_t delta_work_ns = 0;
	const auto delta_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	delta.Reset();
	if constexpr (COLLECT_METRICS) {
		const auto delta_end = std::chrono::steady_clock::now();
		delta_work_ns +=
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(delta_end - delta_start).count());
	}

	auto epoch_ht = CreateUsingKeyHashTable();
	const auto preaggregation_work_ns = PreaggregateUsingKeyUpdates<COLLECT_METRICS>(*epoch_ht);
	if constexpr (COLLECT_METRICS) {
		GetEpochMetrics().RecordKeyPreaggregation(delta_candidate_count, epoch_ht->Count(), preaggregation_work_ns);
	}
	ApplyPreaggregatedUsingKeyUpdates<COLLECT_METRICS>(*epoch_ht, delta_work_ns);

	const auto finalize_start =
	    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	op.working_table->ResetForReuse();
	op.working_table->InitializeAppend(working_append_state);
	const auto index_work_ns = FinalizeUsingKeyDelta(!partial_key_indexes.empty(), COLLECT_METRICS);
	intermediate_table.ResetForReuse();
	InitializeIntermediateAppend();
	if constexpr (COLLECT_METRICS) {
		const auto finalize_end = std::chrono::steady_clock::now();
		const auto finalize_work_ns = NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(finalize_end - finalize_start).count());
		D_ASSERT(index_work_ns <= finalize_work_ns);
		delta_work_ns += finalize_work_ns - index_work_ns;
		GetEpochMetrics().RecordKeyDelta(delta_candidate_count, delta.touched_count, delta.new_count,
		                                 delta.changed_count, delta_work_ns);
	}
}

void RecursiveCTEKeyedState::CommitUsingKeyUpdates() {
	if (metrics.Enabled()) {
		CommitUsingKeyUpdatesInternal<true>();
	} else {
		CommitUsingKeyUpdatesInternal<false>();
	}
}

SinkResultType RecursiveCTEKeyedState::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) {
	if (op.UnionAll()) {
		AppendOutput(chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}
	auto &lstate = input.local_state.Cast<RecursiveCTEKeyedLocalState>();
	lstate.SinkUsingKeyOutput(chunk, *this);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType RecursiveCTEKeyedState::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) {
	if (op.UnionAll()) {
		return SinkCombineResultType::FINISHED;
	}
	auto &lstate = input.local_state.Cast<RecursiveCTEKeyedLocalState>();
	if (!lstate.buffer_using_key_output && !lstate.direct_using_key_output && lstate.output &&
	    lstate.output->Count() >= STANDARD_VECTOR_SIZE && lstate.using_key_candidate_count > CurrentInputCount() &&
	    CanPreaggregateUsingKey()) {
		lstate.ClassifyBufferedUsingKeyOutput();
	}
	if (!lstate.buffer_using_key_output) {
		if (GetMetrics().Enabled() && lstate.using_key_classification_work_ns > 0) {
			GetEpochMetrics().RecordLocalKeyPreaggregationClassification(lstate.using_key_classification_work_ns);
		}
		if (lstate.output && lstate.output->Count() > 0) {
			CombineOutput(*lstate.output);
		}
		return SinkCombineResultType::FINISHED;
	}
	D_ASSERT(lstate.output);
	const auto candidate_count = lstate.output->Count();
	D_ASSERT(candidate_count > 0 && lstate.buffer_using_key_output && CanPreaggregateUsingKey());
	idx_t preaggregation_work_ns = 0;
	auto local_ht = lstate.Preaggregate(preaggregation_work_ns);
	RegisterLocalPreaggregation(std::move(local_ht), candidate_count, lstate.using_key_classification_work_ns,
	                            preaggregation_work_ns);
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
const ColumnDataCollection &RecursiveCTEKeyedState::CurrentInputTable() const {
	D_ASSERT(op.working_table);
	return *op.working_table;
}

idx_t RecursiveCTEKeyedState::RecurringStateRows() const {
	return GetHashTable().Count();
}

idx_t RecursiveCTEKeyedState::RecurringStateChunks() const {
	return (GetHashTable().Count() + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
}

SourceResultType RecursiveCTEKeyedState::GetData(ExecutionContext &context, DataChunk &chunk,
                                                 RecursiveCTEGlobalSourceState &source) {
	if (source_phase == RecursiveCTESourcePhase::INITIAL) {
		source_phase = RecursiveCTESourcePhase::RECURSING_KEY;
	}
	if (metrics.Enabled()) {
		return GetUsingKeyDataInternal<true>(context, chunk, source);
	}
	return GetUsingKeyDataInternal<false>(context, chunk, source);
}

template <bool COLLECT_METRICS>
SourceResultType RecursiveCTEKeyedState::GetUsingKeyDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                                 RecursiveCTEGlobalSourceState &source) {
	while (true) {
		switch (source_phase) {
		case RecursiveCTESourcePhase::RECURSING_KEY: {
			idx_t expected_new;
			if (op.UnionAll()) {
				expected_new = intermediate_table.Count();
				op.working_table->Reset();
				op.working_table->Combine(intermediate_table);
				InitializeIntermediateAppend();
			} else {
				expected_new = op.working_table->Count();
				if (expected_new == 0) {
					ht->InitializeScan(ht_scan_state);
					source.StartFinalDrain(ht->Count());
					source_phase = RecursiveCTESourcePhase::DRAINING_FINAL_KEY_STATE;
					break;
				}
			}

			if (expected_new > 0) {
				const auto desired_capacity =
				    GroupedAggregateHashTable::GetCapacityForCount(ht->Count() + expected_new);
				if (desired_capacity > ht->Capacity()) {
					ht->Resize(desired_capacity);
				}
			}

			op.ExecuteRecursivePipelines(context);
			const auto next_count = op.UnionAll() ? intermediate_table.Count() : op.working_table->Count();
			if (next_count == 0) {
				ht->InitializeScan(ht_scan_state);
				source.StartFinalDrain(ht->Count());
				source_phase = RecursiveCTESourcePhase::DRAINING_FINAL_KEY_STATE;
			}
			break;
		}
		case RecursiveCTESourcePhase::DRAINING_FINAL_KEY_STATE: {
			const auto drain_start =
			    COLLECT_METRICS ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			while (ht->Scan(ht_scan_state, source_distinct_rows, source_aggregate_rows)) {
				if (source_distinct_rows.size() == 0) {
					continue;
				}
				AssembleStateRows(source_distinct_rows, source_aggregate_rows, chunk);
				source.AddEmittedRows(chunk.size());
				if constexpr (COLLECT_METRICS) {
					metrics.RecordFinalStateRows(chunk.size());
					const auto drain_end = std::chrono::steady_clock::now();
					GetEpochMetrics().RecordFinalStateDrain(NumericCast<idx_t>(
					    std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - drain_start).count()));
				}
				return SourceResultType::HAVE_MORE_OUTPUT;
			}
			if constexpr (COLLECT_METRICS) {
				const auto drain_end = std::chrono::steady_clock::now();
				GetEpochMetrics().RecordFinalStateDrain(NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(drain_end - drain_start).count()));
			}
			source_phase = RecursiveCTESourcePhase::FINISHED;
			source.Finish();
			break;
		}
		case RecursiveCTESourcePhase::FINISHED:
			return SourceResultType::FINISHED;
		default:
			throw InternalException("Unsupported recursive CTE key source phase");
		}
	}
}

//===--------------------------------------------------------------------===//
// State Scan
//===--------------------------------------------------------------------===//
class RecursiveCTEStateScanGlobalState : public GlobalSourceState {
public:
	mutex lock;
	AggregateHTScanState scan_state;
	bool initialized = false;
	//! The number of groups in the hash table and the number of groups scanned (for progress)
	atomic<idx_t> total_groups {0};
	atomic<idx_t> scanned_groups {0};
};

class RecursiveCTEStateScanLocalState : public LocalSourceState {
public:
	RecursiveCTEStateScanLocalState(ClientContext &context, const PhysicalRecursiveCTE &op)
	    : found_groups(STANDARD_VECTOR_SIZE), arena(Allocator::Get(context)), row_state(arena) {
		distinct_rows.Initialize(Allocator::Get(context), op.hash_key_types);
		aggregate_rows.Initialize(Allocator::Get(context), op.aggregate_types);
	}

	DataChunk distinct_rows;
	DataChunk aggregate_rows;
	AggregateHTLookupState lookup_state;
	SelectionVector found_groups;
	ArenaAllocator arena;
	RowOperationsState row_state;
};

PhysicalRecursiveCTEStateScan::PhysicalRecursiveCTEStateScan(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                             idx_t estimated_cardinality, TableIndex cte_index)
    : PhysicalColumnDataScan(physical_plan, std::move(types), PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN,
                             estimated_cardinality, cte_index) {
}

unique_ptr<GlobalSourceState> PhysicalRecursiveCTEStateScan::GetGlobalSourceState(ClientContext &context) const {
	return GetGlobalSourceState(context, OperatorPartitionInfo::NoPartitionInfo());
}

unique_ptr<GlobalSourceState>
PhysicalRecursiveCTEStateScan::GetGlobalSourceState(ClientContext &context,
                                                    const OperatorPartitionInfo &partition_info) const {
	(void)partition_info;
	return make_uniq<RecursiveCTEStateScanGlobalState>();
}

unique_ptr<LocalSourceState> PhysicalRecursiveCTEStateScan::GetLocalSourceState(ExecutionContext &context,
                                                                                GlobalSourceState &gstate) const {
	if (!recursive_cte) {
		throw InternalException("USING KEY state scan is not linked to its recursive CTE");
	}
	return make_uniq<RecursiveCTEStateScanLocalState>(context.client, *recursive_cte);
}

SourceResultType PhysicalRecursiveCTEStateScan::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                                OperatorSourceInput &input) const {
	if (!recursive_cte || !recursive_cte->sink_state) {
		throw InternalException("USING KEY state scan has no recursive state");
	}
	auto &recursive_state = recursive_cte->sink_state->Cast<RecursiveCTEKeyedState>();
	if (!recursive_state.GetMetrics().Enabled()) {
		return GetDataFromState(chunk, input, recursive_state);
	}
	const auto scan_start = std::chrono::steady_clock::now();
	const auto result = GetDataFromState(chunk, input, recursive_state);
	const auto scan_end = std::chrono::steady_clock::now();
	recursive_state.GetEpochMetrics().RecordRecurringScan(
	    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(scan_end - scan_start).count()));
	return result;
}

SourceResultType PhysicalRecursiveCTEStateScan::GetDataFromState(DataChunk &chunk, OperatorSourceInput &input,
                                                                 RecursiveCTEKeyedState &recursive_state) const {
	auto &gstate = input.global_state.Cast<RecursiveCTEStateScanGlobalState>();
	auto &lstate = input.local_state.Cast<RecursiveCTEStateScanLocalState>();
	while (true) {
		{
			lock_guard<mutex> guard(gstate.lock);
			if (!gstate.initialized) {
				recursive_state.GetHashTable().InitializeScan(gstate.scan_state);
				gstate.total_groups = recursive_state.GetHashTable().Count();
				gstate.initialized = true;
			}
			if (!recursive_state.GetHashTable().ScanGroups(gstate.scan_state, lstate.distinct_rows)) {
				return SourceResultType::FINISHED;
			}
			gstate.scanned_groups.fetch_add(lstate.distinct_rows.size(), std::memory_order_relaxed);
		}
		if (lstate.distinct_rows.size() == 0) {
			continue;
		}
		const auto group_count = lstate.distinct_rows.size();
		const auto found_count =
		    recursive_state.GetHashTable().LookupGroups(lstate.distinct_rows, lstate.lookup_state, lstate.found_groups);
		if (found_count != group_count) {
			throw InternalException("USING KEY state scan could not find %d of %d frozen groups",
			                        group_count - found_count, group_count);
		}
		recursive_state.FinalizeStateRows(lstate.row_state, lstate.lookup_state.addresses, lstate.distinct_rows,
		                                  lstate.aggregate_rows, chunk);
		if (recursive_state.GetMetrics().Enabled()) {
			recursive_state.GetMetrics().RecordRecurringScanRows(chunk.size());
		}
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
}

ProgressData PhysicalRecursiveCTEStateScan::GetProgress(ClientContext &context, GlobalSourceState &gstate_p) const {
	auto &gstate = gstate_p.Cast<RecursiveCTEStateScanGlobalState>();
	idx_t total_groups = gstate.total_groups;
	ProgressData progress;
	progress.total = static_cast<double>(MaxValue<idx_t>(total_groups, 1));
	progress.done = static_cast<double>(MinValue<idx_t>(gstate.scanned_groups, total_groups));
	return progress;
}

InsertionOrderPreservingMap<string> PhysicalRecursiveCTEStateScan::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Index"] = StringUtil::Format("%llu", cte_index.index);
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
