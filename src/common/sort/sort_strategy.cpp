#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/sorting/full_sort.hpp"
#include "duckdb/common/sorting/hashed_sort.hpp"
#include "duckdb/common/sorting/natural_sort.hpp"
#include "duckdb/common/sorting/partitioned_sort.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// SortStrategy
//===--------------------------------------------------------------------===//
SortStrategy::SortStrategy(const Types &input_types) : payload_types(input_types) {
	// The payload prefix is the same as the input schema
	for (column_t i = 0; i < payload_types.size(); ++i) {
		scan_ids.emplace_back(i);
	}
}

void SortStrategy::Synchronize(ClientContext &client, const GlobalSinkState &source, GlobalSinkState &target) const {
}

void SortStrategy::SortColumnData(ExecutionContext &context, hash_t hash_bin,
                                  OperatorSinkFinalizeInput &finalize) const {
	//	Nothing to sort
	return;
}

unique_ptr<LocalSourceState> SortStrategy::GetLocalSourceState(ExecutionContext &context,
                                                               GlobalSourceState &gstate) const {
	return make_uniq<LocalSourceState>();
}

unique_ptr<SortStrategy> SortStrategy::Factory(ClientContext &client,
                                               const vector<unique_ptr<Expression>> &partition_bys,
                                               const vector<BoundOrderByNode> &order_bys, const Types &payload_types,
                                               const vector<unique_ptr<BaseStatistics>> &partitions_stats,
                                               const OperatorPartitionInfo &partition_info, idx_t estimated_cardinality,
                                               bool require_payload) {
	if (partition_info.RequiresPartitionColumns()) {
		return make_uniq<PartitionedSort>(client, order_bys, payload_types, partition_info, require_payload);
	} else if (!partition_bys.empty()) {
		return make_uniq<HashedSort>(client, partition_bys, order_bys, payload_types, partitions_stats,
		                             estimated_cardinality, require_payload);
	} else if (!order_bys.empty()) {
		return make_uniq<FullSort>(client, order_bys, payload_types, require_payload);
	} else {
		return make_uniq<NaturalSort>(payload_types);
	}
}

//===--------------------------------------------------------------------===//
// NextBatch
//===--------------------------------------------------------------------===//
SinkNextBatchType SortStrategy::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &batch) const {
	return SinkNextBatchType::READY;
}

unique_ptr<TupleDataCollection> SortStrategy::TryTakeSinglePartition(idx_t, GlobalSourceState &) const {
	return nullptr;
}

void SortStrategy::RegisterHyperLogLog(LocalSinkState &, ParallelHyperLogLogLocalState &) const {
	// NOP for all but HashedSort
}

} // namespace duckdb
