//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/physical_recursive_cte_union_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"

namespace duckdb {

class RecursiveCTEUnionLocalState;
struct RecursiveCTEDistinctPartition;

//! UNION and UNION ALL recursion. The member output of an iteration becomes the next frontier, deduplicated against
//! a distinct hash table for UNION. The iteration output alternates between the operator's working table and an
//! intermediate table, so the frontier the member scans stays intact while the next one is produced.
class RecursiveCTEUnionState : public RecursiveCTEState {
public:
	RecursiveCTEUnionState(ClientContext &context, const PhysicalRecursiveCTE &op);
	~RecursiveCTEUnionState() override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) override;
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk,
	                         RecursiveCTEGlobalSourceState &source) override;
	const ColumnDataCollection &CurrentInputTable() const override;
	idx_t RecurringStateRows() const override;
	idx_t RecurringStateChunks() const override;

	//! Workers append to the shared iteration output when the scheduler decides against private output collections
	bool UsesLocalUnionAllOutput() const {
		return use_local_union_all_output;
	}
	void SetUseLocalUnionAllOutput(bool value) {
		use_local_union_all_output = value;
	}
	void InitializeSharedOutputAppend();
	void AppendOutput(DataChunk &chunk);
	void CombineOutput(ColumnDataCollection &output);
	//! UNION dedup: one serial table until the recursion is wide enough for partitions
	bool HasDistinctPartitions() const {
		return !distinct_partitions.empty();
	}
	void PromoteDistinctState(ClientContext &context, idx_t partition_count);
	void SinkSerialDistinct(DataChunk &chunk, RecursiveCTEUnionLocalState &local_state);
	void SinkDistinct(DataChunk &chunk, RecursiveCTEUnionLocalState &local_state, bool emit_rows = true,
	                  bool record_sink_metrics = true);

private:
	ColumnDataCollection &CurrentOutputTable();
	ColumnDataCollection &CurrentInputTable();
	ColumnDataAppendState &CurrentOutputAppendState();
	void AdvanceIterationBuffers();
	void ResetCurrentOutputTableForReuse();
	void RebindRecursiveScans();

private:
	mutex intermediate_table_lock;
	ColumnDataCollection intermediate_table;
	ColumnDataAppendState intermediate_append_state;
	ColumnDataAppendState working_append_state;
	ColumnDataScanState scan_state;
	//! Which of the two output buffers the current iteration writes
	bool output_is_working = false;
	bool use_local_union_all_output = true;
	//! Serial dedup table of UNION recursion before promotion to partitions
	unique_ptr<GroupedAggregateHashTable> ht;
	vector<unique_ptr<RecursiveCTEDistinctPartition>> distinct_partitions;
};

} // namespace duckdb
