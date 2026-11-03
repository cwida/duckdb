#include "duckdb/execution/base_aggregate_hashtable.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

BaseAggregateHashTable::BaseAggregateHashTable(ClientContext &context, Allocator &allocator,
                                               const vector<AggregateObject> &aggregates,
                                               vector<LogicalType> payload_types_p)
    : allocator(allocator), buffer_manager(BufferManager::GetBufferManager(context)),
      layout_ptr(make_shared_ptr<TupleDataLayout>()), payload_types(std::move(payload_types_p)) {
	filter_set.Initialize(context, aggregates, payload_types);
}

bool BaseAggregateHashTable::AllAggregatesClustered(const vector<AggregateObject> &aggregates) {
	if (aggregates.empty()) {
		return false;
	}
	for (auto &aggregate : aggregates) {
		if (aggregate.filter || !aggregate.function.GetStateClusterUpdateCallback()) {
			return false;
		}
	}
	return true;
}

idx_t BaseAggregateHashTable::CountAggregatesClustered(const vector<AggregateObject> &aggregates) {
	idx_t count = 0;
	for (auto &aggregate : aggregates) {
		if (!aggregate.filter && aggregate.function.GetStateClusterUpdateCallback()) {
			count++;
		}
	}
	return count;
}

} // namespace duckdb
