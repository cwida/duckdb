//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/sort_strategy.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/sorting/sort.hpp"

namespace duckdb {

class ParallelHyperLogLogLocalState;
class TupleDataCollection;

class SortStrategy {
public:
	using Types = vector<LogicalType>;
	using HashGroupPtr = unique_ptr<ColumnDataCollection>;
	using SortedRunPtr = unique_ptr<SortedRun>;

	static unique_ptr<SortStrategy> Factory(ClientContext &context, const vector<unique_ptr<Expression>> &partition_bys,
	                                        const vector<BoundOrderByNode> &order_bys, const Types &payload_types,
	                                        const vector<unique_ptr<BaseStatistics>> &partitions_stats,
	                                        const OperatorPartitionInfo &partition_info, idx_t estimated_cardinality,
	                                        bool require_payload = false);

	explicit SortStrategy(const Types &input_types);
	virtual ~SortStrategy() = default;

public:
	//===--------------------------------------------------------------------===//
	// Sink Interface
	//===--------------------------------------------------------------------===//
	virtual unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const = 0;
	virtual unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &client) const = 0;
	virtual SinkNextBatchType NextBatch(ExecutionContext &, OperatorSinkNextBatchInput &) const;
	virtual SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const = 0;
	virtual SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const = 0;
	virtual SinkFinalizeType Finalize(ClientContext &client, OperatorSinkFinalizeInput &finalize) const = 0;
	virtual ProgressData GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
	                                     const ProgressData source_progress) const = 0;
	virtual void Synchronize(ClientContext &client, const GlobalSinkState &source, GlobalSinkState &target) const;

public:
	//===--------------------------------------------------------------------===//
	// Source Interface
	//===--------------------------------------------------------------------===//
	virtual unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                         GlobalSourceState &gstate) const;
	virtual unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context, GlobalSinkState &sink) const = 0;

public:
	//===--------------------------------------------------------------------===//
	// Non-Standard Interface
	//===--------------------------------------------------------------------===//
	virtual void SortColumnData(ExecutionContext &context, hash_t hash_bin, OperatorSinkFinalizeInput &finalize) const;

	virtual SourceResultType MaterializeColumnData(ExecutionContext &context, idx_t hash_bin,
	                                               OperatorSourceInput &source) const = 0;
	virtual HashGroupPtr GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const = 0;

	virtual SourceResultType MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
	                                              OperatorSourceInput &source) const = 0;
	virtual SortedRunPtr GetSortedRun(ClientContext &client, idx_t hash_bin, OperatorSourceInput &source) const = 0;

	// The chunk and row counts of the hash groups.
	struct ChunkRow {
		idx_t chunks = 0;
		idx_t count = 0;
	};
	using ChunkRows = vector<ChunkRow>;
	virtual const ChunkRows &GetHashGroups(GlobalSourceState &global_state) const = 0;

	//! Takes the buffered rows of a hash bin that is known to hold a single partition key out of the strategy, so that
	//! the caller can read them directly instead of sorting or materializing the bin (which must not happen
	//! afterwards). Call after Finalize and GetGlobalSourceState. The default knows no such bins.
	virtual unique_ptr<TupleDataCollection> TryTakeSinglePartition(idx_t hash_bin, GlobalSourceState &source) const;

	virtual void RegisterHyperLogLog(LocalSinkState &local_state, ParallelHyperLogLogLocalState &hll_state) const;

public:
	//! The inserted data schema
	Types payload_types;
	//! Input columns in the sorted output
	vector<column_t> scan_ids;
	//! Key columns in the sorted output. Needed for prefix computations.
	vector<column_t> sort_ids;
};

} // namespace duckdb
