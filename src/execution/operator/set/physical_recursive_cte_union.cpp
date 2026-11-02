#include "duckdb/execution/operator/set/physical_recursive_cte_union_state.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct RecursiveCTEDistinctPartition {
	RecursiveCTEDistinctPartition(ClientContext &context, const vector<LogicalType> &types)
	    : ht(context, BufferAllocator::Get(context), types) {
	}

	mutex lock;
	GroupedAggregateHashTable ht;
};

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
RecursiveCTEUnionState::RecursiveCTEUnionState(ClientContext &context, const PhysicalRecursiveCTE &op)
    : RecursiveCTEState(context, op), intermediate_table(context, op.GetTypes()) {
	if (!op.UnionAll()) {
		ht = make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), op.distinct_types);
	}
	if (op.recurring_table) {
		// the recurring collection is owned by the operator and must start empty for every
		// invocation: a nested recursion re-entered per outer iteration, and a re-executed
		// prepared plan, would otherwise read the previous invocation's rows
		op.recurring_table->Reset();
	}
}

RecursiveCTEUnionState::~RecursiveCTEUnionState() {
	LogMetrics({});
}

class RecursiveCTEUnionLocalState : public LocalSinkState {
public:
	RecursiveCTEUnionLocalState(ClientContext &context, const PhysicalRecursiveCTE &op)
	    : op(op), hashes(LogicalType::HASH), partition_hashes(LogicalType::HASH), dummy_addresses(LogicalType::POINTER),
	      new_groups(STANDARD_VECTOR_SIZE) {
		output = make_uniq<ColumnDataCollection>(context, op.GetTypes());
		output->InitializeAppend(append_state);
		if (!op.UnionAll()) {
			partition_chunk.Initialize(Allocator::Get(context), op.GetTypes());
		}
	}

	const PhysicalRecursiveCTE &op;
	unique_ptr<ColumnDataCollection> output;
	ColumnDataAppendState append_state;
	Vector hashes;
	Vector partition_hashes;
	Vector dummy_addresses;
	SelectionVector new_groups;
	DataChunk partition_chunk;
	vector<SelectionVector> partition_selections;
	vector<idx_t> partition_counts;

	void InitializePartitions(idx_t partition_count) {
		if (partition_selections.size() == partition_count) {
			return;
		}
		partition_selections.clear();
		partition_selections.reserve(partition_count);
		for (idx_t partition_idx = 0; partition_idx < partition_count; partition_idx++) {
			partition_selections.emplace_back(STANDARD_VECTOR_SIZE);
		}
		partition_counts.resize(partition_count);
	}

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ExecutionContext &context, GlobalSinkState &gstate) override {
		auto &recursive_state = gstate.Cast<RecursiveCTEUnionState>();
		if (op.UnionAll() && !recursive_state.UsesLocalUnionAllOutput()) {
			return;
		}
		output->ResetForReuse();
		output->InitializeAppend(append_state);
	}
};

unique_ptr<LocalSinkState> RecursiveCTEUnionState::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<RecursiveCTEUnionLocalState>(context.client, op);
}

void RecursiveCTEUnionState::InitializeSharedOutputAppend() {
	CurrentOutputTable().InitializeAppend(CurrentOutputAppendState());
}

void RecursiveCTEUnionState::AppendOutput(DataChunk &chunk) {
	const auto collect_metrics = metrics.Enabled();
	const auto before_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	lock_guard<mutex> guard(intermediate_table_lock);
	const auto after_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	CurrentOutputTable().Append(CurrentOutputAppendState(), chunk);
	if (collect_metrics) {
		const auto after_work = std::chrono::steady_clock::now();
		RecordSinkMetrics(
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_lock - before_lock).count()),
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()),
		    chunk.size());
	}
}

void RecursiveCTEUnionState::CombineOutput(ColumnDataCollection &output) {
	const auto collect_metrics = metrics.Enabled();
	const auto before_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	lock_guard<mutex> guard(intermediate_table_lock);
	const auto after_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	const auto row_count = output.Count();
	CurrentOutputTable().Combine(output);
	if (collect_metrics) {
		const auto after_work = std::chrono::steady_clock::now();
		RecordSinkMetrics(
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_lock - before_lock).count()),
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()),
		    row_count);
	}
}

void RecursiveCTEUnionState::SinkSerialDistinct(DataChunk &chunk, RecursiveCTEUnionLocalState &lstate) {
	D_ASSERT(ht);
	const auto collect_metrics = metrics.Enabled();
	const auto candidate_count = chunk.size();
	const auto before_lock =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	idx_t new_group_count;
	{
		lock_guard<mutex> guard(intermediate_table_lock);
		const auto after_lock =
		    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		if (collect_metrics) {
			metrics.RecordHashRows(candidate_count);
		}
		new_group_count = ht->FindOrCreateGroups(chunk, lstate.dummy_addresses, lstate.new_groups);
		chunk.Slice(lstate.new_groups, new_group_count);
		if (collect_metrics) {
			const auto after_work = std::chrono::steady_clock::now();
			GetEpochMetrics().RecordDistinctGrouping(
			    candidate_count, new_group_count,
			    NumericCast<idx_t>(
			        std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()));
			RecordSinkMetrics(
			    NumericCast<idx_t>(
			        std::chrono::duration_cast<std::chrono::nanoseconds>(after_lock - before_lock).count()),
			    NumericCast<idx_t>(
			        std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()),
			    candidate_count);
		}
	}
	if (new_group_count > 0) {
		lstate.output->Append(lstate.append_state, chunk);
	}
}

void RecursiveCTEUnionState::SinkDistinct(DataChunk &chunk, RecursiveCTEUnionLocalState &lstate, bool emit_rows,
                                          bool record_sink_metrics) {
	auto &partitions = distinct_partitions;
	D_ASSERT(!partitions.empty());
	D_ASSERT((partitions.size() & (partitions.size() - 1)) == 0);
	lstate.InitializePartitions(partitions.size());
	std::fill(lstate.partition_counts.begin(), lstate.partition_counts.end(), 0);

	chunk.Hash(lstate.hashes);
	auto hash_data = FlatVector::GetData<hash_t>(lstate.hashes);
	const auto partition_mask = partitions.size() - 1;
	for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
		const auto partition_idx = hash_data[row_idx] & partition_mask;
		auto &partition_count = lstate.partition_counts[partition_idx];
		lstate.partition_selections[partition_idx].set_index(partition_count++, row_idx);
	}

	for (idx_t partition_idx = 0; partition_idx < partitions.size(); partition_idx++) {
		const auto partition_count = lstate.partition_counts[partition_idx];
		if (partition_count == 0) {
			continue;
		}
		lstate.partition_chunk.Reset();
		lstate.partition_chunk.Slice(chunk, lstate.partition_selections[partition_idx], partition_count);
		lstate.partition_hashes.Slice(lstate.hashes, lstate.partition_selections[partition_idx], partition_count);
		auto &partition = *partitions[partition_idx];
		const auto collect_sink_metrics = metrics.Enabled() && record_sink_metrics;
		const auto before_lock =
		    collect_sink_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		idx_t new_group_count;
		{
			lock_guard<mutex> guard(partition.lock);
			const auto after_lock =
			    collect_sink_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			if (metrics.Enabled()) {
				metrics.RecordHashRows(partition_count);
			}
			new_group_count = partition.ht.FindOrCreateGroups(lstate.partition_chunk, lstate.partition_hashes,
			                                                  lstate.dummy_addresses, lstate.new_groups);
			lstate.partition_chunk.Slice(lstate.new_groups, new_group_count);
			if (collect_sink_metrics) {
				const auto after_work = std::chrono::steady_clock::now();
				GetEpochMetrics().RecordDistinctGrouping(
				    partition_count, new_group_count,
				    NumericCast<idx_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()));
				RecordSinkMetrics(
				    NumericCast<idx_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(after_lock - before_lock).count()),
				    NumericCast<idx_t>(
				        std::chrono::duration_cast<std::chrono::nanoseconds>(after_work - after_lock).count()),
				    partition_count);
			}
		}
		if (emit_rows && new_group_count > 0) {
			lstate.output->Append(lstate.append_state, lstate.partition_chunk);
		}
	}
}

void RecursiveCTEUnionState::PromoteDistinctState(ClientContext &context, idx_t partition_count) {
	D_ASSERT(!op.UsingKey() && !op.UnionAll());
	if (!distinct_partitions.empty() || partition_count <= 1) {
		return;
	}
	D_ASSERT(ht);
	const auto migrated_rows = ht->Count();
	const auto promotion_start =
	    metrics.Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	distinct_partitions.reserve(partition_count);
	for (idx_t partition_idx = 0; partition_idx < partition_count; partition_idx++) {
		distinct_partitions.push_back(make_uniq<RecursiveCTEDistinctPartition>(context, op.distinct_types));
	}

	RecursiveCTEUnionLocalState migration_state(context, op);
	DataChunk groups;
	groups.Initialize(Allocator::Get(context), op.distinct_types);
	DataChunk payload;
	AggregateHTScanState scan_state;
	ht->InitializeScan(scan_state);
	while (ht->Scan(scan_state, groups, payload)) {
		context.InterruptCheck();
		if (groups.size() > 0) {
			SinkDistinct(groups, migration_state, false, false);
		}
	}
	ht.reset();
	if (metrics.Enabled()) {
		const auto promotion_end = std::chrono::steady_clock::now();
		const auto elapsed_us = NumericCast<idx_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(promotion_end - promotion_start).count());
		metrics.LogDistinctPromotion(partition_count, migrated_rows, elapsed_us);
	}
}

SinkResultType RecursiveCTEUnionState::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) {
	if (op.UnionAll()) {
		if (!UsesLocalUnionAllOutput()) {
			AppendOutput(chunk);
			return SinkResultType::NEED_MORE_INPUT;
		}
		auto &lstate = input.local_state.Cast<RecursiveCTEUnionLocalState>();
		D_ASSERT(lstate.output);
		lstate.output->Append(lstate.append_state, chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}
	auto &lstate = input.local_state.Cast<RecursiveCTEUnionLocalState>();
	D_ASSERT(lstate.output);
	if (!HasDistinctPartitions()) {
		SinkSerialDistinct(chunk, lstate);
	} else {
		SinkDistinct(chunk, lstate);
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType RecursiveCTEUnionState::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) {
	if (op.UnionAll() && !UsesLocalUnionAllOutput()) {
		return SinkCombineResultType::FINISHED;
	}
	auto &lstate = input.local_state.Cast<RecursiveCTEUnionLocalState>();
	D_ASSERT(lstate.output);
	CombineOutput(*lstate.output);
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
ColumnDataCollection &RecursiveCTEUnionState::CurrentOutputTable() {
	if (!output_is_working) {
		return intermediate_table;
	}
	D_ASSERT(op.working_table);
	return *op.working_table;
}

ColumnDataCollection &RecursiveCTEUnionState::CurrentInputTable() {
	if (output_is_working) {
		return intermediate_table;
	}
	D_ASSERT(op.working_table);
	return *op.working_table;
}

const ColumnDataCollection &RecursiveCTEUnionState::CurrentInputTable() const {
	if (output_is_working) {
		return intermediate_table;
	}
	D_ASSERT(op.working_table);
	return *op.working_table;
}

idx_t RecursiveCTEUnionState::RecurringStateRows() const {
	D_ASSERT(op.recurring_table);
	return op.recurring_table->Count();
}

idx_t RecursiveCTEUnionState::RecurringStateChunks() const {
	D_ASSERT(op.recurring_table);
	return op.recurring_table->ChunkCount();
}

ColumnDataAppendState &RecursiveCTEUnionState::CurrentOutputAppendState() {
	if (!output_is_working) {
		return intermediate_append_state;
	}
	return working_append_state;
}

void RecursiveCTEUnionState::AdvanceIterationBuffers() {
	output_is_working = !output_is_working;
}

void RecursiveCTEUnionState::ResetCurrentOutputTableForReuse() {
	CurrentOutputTable().ResetForReuse();
}

void RecursiveCTEUnionState::RebindRecursiveScans() {
	auto &input_table = CurrentInputTable();
	for (auto &scan_ref : op.recursive_scans) {
		auto &scan = scan_ref.get();
		scan.collection = input_table;
	}
}

SourceResultType RecursiveCTEUnionState::GetData(ExecutionContext &context, DataChunk &chunk,
                                                 RecursiveCTEGlobalSourceState &source) {
	if (source_phase == RecursiveCTESourcePhase::INITIAL) {
		CurrentOutputTable().InitializeScan(scan_state);
		source.StartIteration(CurrentOutputTable().Count());
		source_phase = RecursiveCTESourcePhase::SCANNING_UNION;
	}
	while (chunk.size() == 0) {
		if (source_phase == RecursiveCTESourcePhase::SCANNING_UNION) {
			// scan any chunks we have collected so far
			CurrentOutputTable().Scan(scan_state, chunk);
			if (chunk.size() != 0) {
				source.AddEmittedRows(chunk.size());
				break;
			}
		} else if (source_phase == RecursiveCTESourcePhase::FINISHED) {
			break;
		} else {
			throw InternalException("Unsupported recursive CTE union source phase");
		}

		if (chunk.size() == 0) {
			// we have run out of chunks
			// now we need to recurse
			// we set up the working table as the data we gathered in this iteration of the recursion
			auto &current_output = CurrentOutputTable();

			// After an iteration, we reset the recurring table
			// and fill it up with the new hash table rows for the next iteration.
			if (op.ref_recurring && current_output.Count() != 0) {
				// copy since Combine would destroy the intermediate table
				op.recurring_table->Append(current_output);
			}

			AdvanceIterationBuffers();
			ResetCurrentOutputTableForReuse();
			RebindRecursiveScans();

			// Pre-grow the dedup HT to avoid costly Resize + ReinsertTuples during the next Sink phase.
			// current_output.Count() is the count of rows output in the previous iteration — an upper bound
			// on the number of new unique rows the next iteration can add (since the recursion is converging).
			if (!op.UnionAll()) {
				const idx_t expected_new = current_output.Count();
				if (expected_new > 0) {
					if (distinct_partitions.empty()) {
						const idx_t desired_capacity =
						    GroupedAggregateHashTable::GetCapacityForCount(ht->Count() + expected_new);
						if (desired_capacity > ht->Capacity()) {
							ht->Resize(desired_capacity);
						}
					} else {
						const auto expected_per_partition =
						    (expected_new + distinct_partitions.size() - 1) / distinct_partitions.size();
						for (auto &partition : distinct_partitions) {
							const auto desired_capacity = GroupedAggregateHashTable::GetCapacityForCount(
							    partition->ht.Count() + expected_per_partition);
							if (desired_capacity > partition->ht.Capacity()) {
								partition->ht.Resize(desired_capacity);
							}
						}
					}
				}
			}

			// now we need to re-execute all of the pipelines that depend on the recursion
			op.ExecuteRecursivePipelines(context);

			// check if we obtained any results
			// if not, we are done
			if (CurrentOutputTable().Count() == 0) {
				source_phase = RecursiveCTESourcePhase::FINISHED;
				source.Finish();
				break;
			}
			// set up the scan again
			CurrentOutputTable().InitializeScan(scan_state);
			source.StartIteration(CurrentOutputTable().Count());
		}
	}

	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
