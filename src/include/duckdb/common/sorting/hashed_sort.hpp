//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/hashed_sort.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/sorting/sort_strategy.hpp"

namespace duckdb {

class HashedSort : public SortStrategy {
public:
	using Orders = vector<BoundOrderByNode>;
	using Types = vector<LogicalType>;
	using HashGroupPtr = unique_ptr<ColumnDataCollection>;
	using SortedRunPtr = unique_ptr<SortedRun>;

	static void GenerateOrderings(Orders &partitions, Orders &orders,
	                              const vector<unique_ptr<Expression>> &partition_bys, const Orders &order_bys,
	                              const vector<unique_ptr<BaseStatistics>> &partitions_stats);

	HashedSort(ClientContext &context, const vector<unique_ptr<Expression>> &partition_bys,
	           const vector<BoundOrderByNode> &order_bys, const Types &payload_types,
	           const vector<unique_ptr<BaseStatistics>> &partitions_stats, idx_t estimated_cardinality,
	           bool require_payload = false);

public:
	//===--------------------------------------------------------------------===//
	// Sink Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &client) const override;
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

	SourceResultType MaterializeColumnData(ExecutionContext &context, idx_t hash_bin,
	                                       OperatorSourceInput &source) const override;
	HashGroupPtr GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const override;

	SourceResultType MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
	                                      OperatorSourceInput &source) const override;
	SortedRunPtr GetSortedRun(ClientContext &client, idx_t hash_bin, OperatorSourceInput &source) const override;

	const ChunkRows &GetHashGroups(GlobalSourceState &global_state) const override;
	unique_ptr<TupleDataCollection> TryTakeSinglePartition(idx_t hash_bin, GlobalSourceState &source) const override;

	void RegisterHyperLogLog(LocalSinkState &local_state, ParallelHyperLogLogLocalState &hll_state) const override;

public:
	//! The host's estimated row count
	const idx_t estimated_cardinality;

	//! The PARTITION BY sorting
	Orders partitions;
	//! The ORDER BY sorting
	Orders orders;
	//! The partition columns
	vector<column_t> partition_ids;
	//! The payload columns corresponding to the PARTITION BY keys
	vector<column_t> partition_key_ids;
	//! The PARTITION BY key types
	vector<LogicalType> partition_key_types;
	//! The number of PARTITION BY keys
	idx_t partition_key_count = 0;
	//! Whether single-key hash groups can skip sorting
	bool can_bypass_single_key_sort = false;
	//! Whether hash groups are keyed on the value of a single integer partition column (every group single-key)
	bool integer_partitioning = false;
	//! The smallest partition key value and the number of key values when integer partitioning
	Value integer_partition_min;
	idx_t integer_partition_count = 0;
	//! Whether single-key hash groups are detected while sinking
	bool track_single_keys = false;
	//! Are we creating a dummy payload column?
	bool force_payload = false;
	// Key columns that must be computed
	vector<unique_ptr<Expression>> sort_exprs;
	//! Common sort description
	unique_ptr<Sort> sort;
};

} // namespace duckdb
