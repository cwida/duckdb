//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/physical_recursive_cte_keyed_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"

namespace duckdb {

class RecursiveCTEKeyedLocalState;
struct RecursiveCTEKeyDeltaState;
struct AuxiliaryDataSet;
class StringHeap;

//! Turns candidate rows into the normalized key columns and the aggregate inputs of the keyed state. Classification,
//! pre-aggregation and commits each own one, so every path applies the same key normalization.
class RecursiveCTEKeyedCandidateExtractor {
public:
	RecursiveCTEKeyedCandidateExtractor(ClientContext &context, const PhysicalRecursiveCTE &op);

	//! The key columns of source, referenced directly or normalized into keys
	void ExtractKeys(DataChunk &source, DataChunk &keys);
	//! The aggregate inputs of source, evaluated into payload; nothing happens when the aggregates take no inputs
	void EvaluatePayload(DataChunk &source, DataChunk &payload);
	const vector<LogicalType> &PayloadTypes() const {
		return payload_types;
	}

private:
	const PhysicalRecursiveCTE &op;
	ExpressionExecutor payload_executor;
	unique_ptr<ExpressionExecutor> key_executor;
	vector<LogicalType> payload_types;
	//! The raw key columns before normalization
	DataChunk raw_keys;
};

//! The finalized aggregate values of every group, stored in the trailing region of the hash table rows and
//! refreshed by the commit for every touched or new group. Reads gather these values instead of invoking
//! aggregate callbacks, so frozen-state reads stay plain loads. String values live in a shared-ownership heap
//! that emitted vectors reference, so gathers emit the stored strings without copying. Compaction at commit
//! swaps in a fresh heap and migrates every live slot; the old heap stays alive until the last referencing
//! vector releases it.
class RecursiveCTEFinalizedValues {
public:
	RecursiveCTEFinalizedValues(ClientContext &context, const PhysicalRecursiveCTE &op);

	//! The trailing region bytes the operator's hash tables must reserve for this plan
	static idx_t RegionWidthFor(const PhysicalRecursiveCTE &op);

	bool Enabled() const {
		return enabled;
	}
	idx_t RegionWidth() const {
		return region_width;
	}
	//! Records the trailing region offset of the persistent hash table's layout
	void BindLayout(const TupleDataLayout &layout);
	//! Writes finalized values (aggregate order) at addresses[sel[i]] from values row sel[i]
	void Scatter(const Vector &addresses, const SelectionVector &sel, idx_t count, DataChunk &values);
	//! Writes candidate rows standing in for finalized values of new groups, mapping each aggregate to its
	//! first input column; valid under the single-value-identity proof of the candidate-reuse paths
	void ScatterCandidates(const Vector &addresses, const SelectionVector &sel, idx_t count, DataChunk &payload);
	//! Reads finalized values (aggregate order) from addresses[sel[i]] into values row i
	void Gather(const Vector &addresses, const SelectionVector &sel, idx_t count, DataChunk &values);
	//! Reclaims replaced string bytes once they outweigh the live ones; the caller guarantees no readers
	void MaybeCompact(GroupedAggregateHashTable &target);

private:
	void ScatterColumns(const Vector &addresses, const SelectionVector &sel, idx_t count, DataChunk &values,
	                    const vector<idx_t> &source_columns);
	string_t CopyStringToArena(string_t value);

private:
	bool enabled = false;
	idx_t region_width = 0;
	idx_t region_offset = 0;
	//! Offset of each aggregate's value slot relative to the region start; validity bytes precede the slots
	vector<idx_t> value_offsets;
	vector<PhysicalType> value_types;
	//! The identity mapping, for scattering finalized chunks in aggregate order
	vector<idx_t> identity_source_columns;
	//! Per aggregate, the column of its first input in the evaluated payload chunk
	vector<idx_t> candidate_source_columns;
	//! Shared-ownership storage for non-inlined finalized strings; emitted vectors hold references to it
	buffer_ptr<AuxiliaryDataSet> string_arena;
	//! The heap inside string_arena's holder; only the single-threaded commit writes to it
	optional_ptr<StringHeap> string_heap;
	idx_t string_live_bytes = 0;
	idx_t string_garbage_bytes = 0;
};

//! USING KEY recursion. Every key owns one aggregate state in a shared hash table. Within an iteration the state is
//! read by recurring scans and direct probes, member rows are collected as update candidates, and the commit at the
//! end of the iteration applies them and assembles the next frontier from the keys whose finalized payload changed.
class RecursiveCTEKeyedState : public RecursiveCTEState {
public:
	RecursiveCTEKeyedState(ClientContext &context, const PhysicalRecursiveCTE &op);
	~RecursiveCTEKeyedState() override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) override;
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, RecursiveCTEGlobalSourceState &source,
	                         const InterruptState &interrupt_state) override;
	const ColumnDataCollection &CurrentInputTable() const override;
	idx_t RecurringStateRows() const override;
	idx_t RecurringStateChunks() const override;

	//! Applies the collected update candidates to the keyed state
	void CommitUsingKeyUpdates();
	const RecursiveCTEPartialKeyIndex &GetPartialKeyIndex(const vector<idx_t> &key_indices) const;
	GroupedAggregateHashTable &GetHashTable() {
		D_ASSERT(ht);
		return *ht;
	}
	const GroupedAggregateHashTable &GetHashTable() const {
		D_ASSERT(ht);
		return *ht;
	}
	bool CanPreaggregateUsingKey() const {
		return op.keyed.can_preaggregate;
	}
	void AppendOutput(DataChunk &chunk);
	void CombineOutput(ColumnDataCollection &output);
	void RegisterLocalPreaggregation(unique_ptr<GroupedAggregateHashTable> local_ht, idx_t candidate_rows,
	                                 idx_t classification_work_ns, idx_t preaggregation_work_ns);
	//! Result rows from frozen state addresses: gathered from the finalized values when they are materialized,
	//! finalized from the aggregate states under the lock otherwise
	void FinalizeStateRows(RowOperationsState &row_state, Vector &addresses, DataChunk &keys, DataChunk &aggregates,
	                       DataChunk &result);
	bool UsesFinalizedValues() const {
		return finalized.Enabled();
	}
	//! Gathers the finalized values at the addresses and assembles result rows; no aggregate callbacks run
	void ReadStateRows(const Vector &addresses, DataChunk &keys, DataChunk &aggregates, DataChunk &result);

private:
	//! Finalizes the aggregate states themselves; the commit uses this to produce the values it materializes
	void FinalizeStateRowsFromStates(RowOperationsState &row_state, Vector &addresses, DataChunk &keys,
	                                 DataChunk &aggregates, DataChunk &result);
	void FinalizeAggregateRows(RowOperationsState &row_state, Vector &addresses, DataChunk &aggregates, idx_t count);
	void AssembleStateRows(DataChunk &keys, DataChunk &aggregates, DataChunk &result) const;
	template <bool COLLECT_METRICS>
	SourceResultType GetUsingKeyDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                         RecursiveCTEGlobalSourceState &source,
	                                         const InterruptState &interrupt_state);
	template <bool COLLECT_METRICS>
	void CommitUsingKeyUpdatesInternal();
	template <bool COLLECT_METRICS>
	void CommitPreaggregatedUsingKeyUpdatesInternal();
	template <bool COLLECT_METRICS>
	void CommitMixedUsingKeyUpdatesInternal(unique_ptr<GroupedAggregateHashTable> epoch_ht,
	                                        idx_t preaggregated_candidate_count);
	template <bool COLLECT_METRICS>
	void ApplyPreaggregatedUsingKeyUpdates(GroupedAggregateHashTable &epoch_ht, idx_t &delta_work_ns);
	template <bool COLLECT_METRICS>
	idx_t PreaggregateUsingKeyUpdates(GroupedAggregateHashTable &epoch_ht);
	unique_ptr<GroupedAggregateHashTable> CreateUsingKeyHashTable() const;
	bool ShouldPreaggregateUsingKeyUpdates(idx_t candidate_count);
	void InitializeIntermediateAppend();
	//! Delta tracking of a commit (physical_recursive_cte_delta.cpp)
	void SnapshotUsingKeyDelta(const Vector &group_addresses, const SelectionVector &new_groups, idx_t new_group_count,
	                           idx_t row_count, bool allow_candidate_reuse = true);
	void SnapshotPreaggregatedUsingKeyDeltaGroups(DataChunk &keys);
	void SnapshotExistingUsingKeyDeltaAddresses(Vector &addresses, idx_t count, bool defer_append = false);
	void AppendPreviousUsingKeyDeltaRows(Vector &addresses, idx_t count);
	void ValidateDeferredUsingKeyCandidateReuse(DataChunk &candidates);
	bool TryReuseChangedGroupCandidates(idx_t candidate_count);
	idx_t FinalizeUsingKeyDelta(bool update_partial_indexes, bool collect_metrics);

private:
	unique_ptr<GroupedAggregateHashTable> ht;
	vector<unique_ptr<RecursiveCTEPartialKeyIndex>> partial_key_indexes;
	RecursiveCTEFinalizedValues finalized;
	RecursiveCTEKeyedCandidateExtractor extractor;
	DataChunk payload_rows;
	Vector new_group_addresses;
	SelectionVector new_groups;

	mutex intermediate_table_lock;
	mutex ht_finalize_lock;
	//! Update candidates collected during an iteration, committed when the iteration finishes
	ColumnDataCollection intermediate_table;
	ColumnDataAppendState intermediate_append_state;
	ColumnDataAppendState working_append_state;
	vector<unique_ptr<GroupedAggregateHashTable>> local_preaggregates;
	idx_t local_preaggregate_candidate_count = 0;
	//! Cached chunk for distinct key extraction in the keyed Sink path
	DataChunk distinct_rows;
	//! Cached chunks for source-side hash table scans
	DataChunk update_rows;
	DataChunk source_aggregate_rows;
	DataChunk source_distinct_rows;
	AggregateHTScanState ht_scan_state;

	unique_ptr<RecursiveCTEKeyDeltaState> key_delta;
	vector<AggregateObject> payload_aggregate_objects;
	Vector preaggregation_hashes;
	vector<unique_ptr<ExpressionExecutor>> payload_comparison_executors;
};

} // namespace duckdb
