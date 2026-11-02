//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/partitioned_sort.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/sorting/sort_strategy.hpp"

namespace duckdb {

class PartitionedSort : public SortStrategy {
public:
	PartitionedSort(ClientContext &client, const vector<BoundOrderByNode> &order_bys, const Types &payload_types,
	                const OperatorPartitionInfo &partition_info, bool require_payload = false);

public:
	//===--------------------------------------------------------------------===//
	// Sink Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &client) const override;
	SinkNextBatchType NextBatch(ExecutionContext &, OperatorSinkNextBatchInput &) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(ClientContext &client, OperatorSinkFinalizeInput &finalize) const override;
	ProgressData GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
	                             const ProgressData source_progress) const override;
	void Synchronize(ClientContext &client, const GlobalSinkState &source, GlobalSinkState &target) const override;

public:
	//===--------------------------------------------------------------------===//
	// Source Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context, GlobalSinkState &sink) const override;

public:
	//===--------------------------------------------------------------------===//
	// Non-Standard Interface
	//===--------------------------------------------------------------------===//
	void SortColumnData(ExecutionContext &context, hash_t hash_bin, OperatorSinkFinalizeInput &finalize) const override;
	const ChunkRows &GetHashGroups(GlobalSourceState &global_state) const override;
	SourceResultType MaterializeColumnData(ExecutionContext &context, idx_t hash_bin,
	                                       OperatorSourceInput &source) const override;
	HashGroupPtr GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const override;
	SourceResultType MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
	                                      OperatorSourceInput &source) const override;
	SortedRunPtr GetSortedRun(ClientContext &client, idx_t hash_bin, OperatorSourceInput &source) const override;

private:
	//! The partitions over which this is grouped (if any)
	const OperatorPartitionInfo &partition_info;
	//! The inner sort strategy
	unique_ptr<SortStrategy> child_strategy;
};

} // namespace duckdb
