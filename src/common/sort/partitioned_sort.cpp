#include "duckdb/common/sorting/partitioned_sort.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/value_map.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// PartitionedSortGlobalSinkState
//===--------------------------------------------------------------------===//
class PartitionedSortGlobalSinkState : public GlobalSinkState {
public:
	using GlobalStatePtr = unique_ptr<GlobalSinkState>;

	explicit PartitionedSortGlobalSinkState(SortStrategy &child_strategy) : child_strategy(child_strategy) {
	}

	optional_ptr<GlobalSinkState> GetOrCreatePartition(ClientContext &client, const Value &partition) {
		lock_guard<mutex> l(lock);
		// find the state that corresponds to this partition and combine
		auto entry = strategy_sinks.find(partition);
		if (entry != strategy_sinks.end()) {
			return entry->second.get();
		}
		// no state yet for this partition - allocate a new one
		auto new_global_state = child_strategy.GetGlobalSinkState(client);
		auto result = new_global_state.get();
		strategy_sinks.insert(make_pair(partition, std::move(new_global_state)));
		return result;
	}

	void SyncPartitioning(ClientContext &client, const PartitionedSortGlobalSinkState &other) {
		for (auto &strategy_sink : other.strategy_sinks) {
			GetOrCreatePartition(client, strategy_sink.first);
		}
	};

	//! The inner sort strategy
	SortStrategy &child_strategy;
	//! The partitioned sunk data. With partitioning there may be more than one
	mutex lock;
	value_map_t<GlobalStatePtr> strategy_sinks;
	vector<optional_ptr<GlobalSinkState>> bin_sinks;
};

//===--------------------------------------------------------------------===//
// PartitionedSortLocalSinkState
//===--------------------------------------------------------------------===//
class PartitionedSortLocalSinkState : public LocalSinkState {
public:
	using GlobalStatePtr = optional_ptr<GlobalSinkState>;
	using LocalStatePtr = unique_ptr<LocalSinkState>;

	explicit PartitionedSortLocalSinkState(ExecutionContext &context) {
	}

	SinkCombineResultType Combine(ExecutionContext &context, const PartitionedSortGlobalSinkState &gstate,
	                              InterruptState &interrupt_state) {
		if (!partition_group) {
			return SinkCombineResultType::FINISHED;
		}

		// flush the local state
		OperatorSinkCombineInput hcombine {*partition_group, *local_group, interrupt_state};
		auto result = gstate.child_strategy.Combine(context, hcombine);

		//	Start a new state pair
		partition_group = nullptr;
		local_group.reset();

		return result;
	}

	//	Partitioning state
	Value current_partition;
	GlobalStatePtr partition_group;
	LocalStatePtr local_group;
};

//===--------------------------------------------------------------------===//
// PartitionedSort
//===--------------------------------------------------------------------===//
PartitionedSort::PartitionedSort(ClientContext &client, const vector<BoundOrderByNode> &order_bys,
                                 const Types &payload_types, const OperatorPartitionInfo &partition_info,
                                 bool require_payload)
    : SortStrategy(payload_types), partition_info(partition_info) {
	//	Pipeline does the partitioning for us, so leave them out
	vector<unique_ptr<Expression>> unpartitioned;
	vector<unique_ptr<BaseStatistics>> unpartitioned_stats;
	OperatorPartitionInfo unpartitioned_info;
	child_strategy = SortStrategy::Factory(client, unpartitioned, order_bys, payload_types, unpartitioned_stats,
	                                       unpartitioned_info, 0, require_payload);

	this->payload_types = child_strategy->payload_types;
	scan_ids = child_strategy->scan_ids;
	sort_ids = child_strategy->sort_ids;
}

//===--------------------------------------------------------------------===//
// GetLocalSinkState
//===--------------------------------------------------------------------===//
unique_ptr<LocalSinkState> PartitionedSort::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<PartitionedSortLocalSinkState>(context);
}

//===--------------------------------------------------------------------===//
// PartitionedSortGlobalSinkState
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSinkState> PartitionedSort::GetGlobalSinkState(ClientContext &client) const {
	return make_uniq<PartitionedSortGlobalSinkState>(*child_strategy);
}

//===--------------------------------------------------------------------===//
// NextBatch
//===--------------------------------------------------------------------===//
SinkNextBatchType PartitionedSort::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &batch) const {
	auto &gstate = batch.global_state.Cast<PartitionedSortGlobalSinkState>();
	auto &lstate = batch.local_state.Cast<PartitionedSortLocalSinkState>();

	(void)lstate.Combine(context, gstate, batch.interrupt_state);

	return SinkNextBatchType::READY;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PartitionedSort::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<PartitionedSortGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<PartitionedSortLocalSinkState>();

	if (!lstate.partition_group) {
		// the local state is not yet initialized for this partition
		// initialize the partition
		child_list_t<Value> partition_values;
		const auto &partition_columns = partition_info.partition_columns;
		for (idx_t partition_idx = 0; partition_idx < partition_columns.size(); partition_idx++) {
			auto column_name = to_string(partition_idx);
			auto &partition = lstate.partition_info.partition_data[partition_idx];
			D_ASSERT(Value::NotDistinctFrom(partition.min_val, partition.max_val));
			partition_values.emplace_back(make_pair(std::move(column_name), partition.min_val));
		}
		lstate.current_partition = Value::STRUCT(std::move(partition_values));

		// initialize the state
		lstate.partition_group = gstate.GetOrCreatePartition(context.client, lstate.current_partition);
		lstate.local_group = child_strategy->GetLocalSinkState(context);
	}

	OperatorSinkInput hsink {*lstate.partition_group, *lstate.local_group, sink.interrupt_state};
	return child_strategy->Sink(context, chunk, hsink);
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
SinkCombineResultType PartitionedSort::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<PartitionedSortGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<PartitionedSortLocalSinkState>();

	return lstate.Combine(context, gstate, combine.interrupt_state);
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType PartitionedSort::Finalize(ClientContext &client, OperatorSinkFinalizeInput &finalize) const {
	auto &gsink = finalize.global_state.Cast<PartitionedSortGlobalSinkState>();
	SinkFinalizeType result = SinkFinalizeType::READY;
	lock_guard<mutex> sinks_guard(gsink.lock);
	for (auto &strategy_sink : gsink.strategy_sinks) {
		OperatorSinkFinalizeInput hfinalize {*strategy_sink.second, finalize.interrupt_state};
		result = child_strategy->Finalize(client, hfinalize);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// SortColumnData
//===--------------------------------------------------------------------===//
void PartitionedSort::SortColumnData(ExecutionContext &context, hash_t hash_bin,
                                     OperatorSinkFinalizeInput &finalize) const {
	auto &gsink = finalize.global_state.Cast<PartitionedSortGlobalSinkState>();
	auto &child_strategy = gsink.child_strategy;

	OperatorSinkFinalizeInput child_finalize {*gsink.bin_sinks[hash_bin], finalize.interrupt_state};
	return child_strategy.SortColumnData(context, 0, child_finalize);
}

//===--------------------------------------------------------------------===//
// Synchronize
//===--------------------------------------------------------------------===//
void PartitionedSort::Synchronize(ClientContext &client, const GlobalSinkState &source, GlobalSinkState &target) const {
	auto &src = source.Cast<PartitionedSortGlobalSinkState>();
	auto &tgt = target.Cast<PartitionedSortGlobalSinkState>();
	tgt.SyncPartitioning(client, src);
}

//===--------------------------------------------------------------------===//
// GetSinkProgress
//===--------------------------------------------------------------------===//
ProgressData PartitionedSort::GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
                                              const ProgressData source_progress) const {
	auto &gsink = gstate.Cast<PartitionedSortGlobalSinkState>();
	auto progress = source_progress;
	lock_guard<mutex> sinks_guard(gsink.lock);
	for (auto &strategy_sink : gsink.strategy_sinks) {
		progress.Add(child_strategy->GetSinkProgress(context, *strategy_sink.second, progress));
	}
	return progress;
}

//===--------------------------------------------------------------------===//
// PartitionedSortGlobalSourceState
//===--------------------------------------------------------------------===//
class PartitionedSortGlobalSourceState : public GlobalSourceState {
public:
	using GlobalStatePtr = unique_ptr<GlobalSourceState>;
	using HashGroupPtr = unique_ptr<ColumnDataCollection>;
	using ChunkRow = PartitionedSort::ChunkRow;
	using ChunkRows = PartitionedSort::ChunkRows;

	PartitionedSortGlobalSourceState(ClientContext &client, PartitionedSortGlobalSinkState &gsink);

	PartitionedSortGlobalSinkState &gsink;
	vector<GlobalStatePtr> child_sources;
	ChunkRows chunk_rows;
};

PartitionedSortGlobalSourceState::PartitionedSortGlobalSourceState(ClientContext &client,
                                                                   PartitionedSortGlobalSinkState &gsink)
    : gsink(gsink) {
	auto &child_strategy = gsink.child_strategy;

	//	Process the sinks in a deterministic order so join sides match up.
	set<Value> ordered_values;
	for (auto &strategy_sink : gsink.strategy_sinks) {
		ordered_values.insert(strategy_sink.first);
	}

	for (auto &value : ordered_values) {
		auto &strategy_sink = gsink.strategy_sinks[value];
		auto child_source = child_strategy.GetGlobalSourceState(client, *strategy_sink);

		//	Always include an empty chunk row so join sides match up.
		ChunkRow chunk_row;
		const auto &child_chunks = child_strategy.GetHashGroups(*child_source);
		if (!child_chunks.empty()) {
			D_ASSERT(child_chunks.size() == 1);
			chunk_row = child_chunks[0];
		}
		chunk_rows.emplace_back(chunk_row);
		child_sources.emplace_back(std::move(child_source));

		//	Map partition keys to bin numbers.
		gsink.bin_sinks.emplace_back(strategy_sink.get());
	}
}

//===--------------------------------------------------------------------===//
// GetGlobalSourceState
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSourceState> PartitionedSort::GetGlobalSourceState(ClientContext &client,
                                                                    GlobalSinkState &sink) const {
	return make_uniq<PartitionedSortGlobalSourceState>(client, sink.Cast<PartitionedSortGlobalSinkState>());
}

//===--------------------------------------------------------------------===//
// GetHashGroups
//===--------------------------------------------------------------------===//
const PartitionedSort::ChunkRows &PartitionedSort::GetHashGroups(GlobalSourceState &gstate) const {
	auto &gsource = gstate.Cast<PartitionedSortGlobalSourceState>();
	return gsource.chunk_rows;
}

//===--------------------------------------------------------------------===//
// MaterializeColumnData
//===--------------------------------------------------------------------===//
SourceResultType PartitionedSort::MaterializeColumnData(ExecutionContext &execution, idx_t hash_bin,
                                                        OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<PartitionedSortGlobalSourceState>();
	auto &child_strategy = gsource.gsink.child_strategy;

	OperatorSourceInput child_source {*gsource.child_sources[hash_bin], source.local_state, source.interrupt_state};
	return child_strategy.MaterializeColumnData(execution, 0, child_source);
}

//===--------------------------------------------------------------------===//
// GetColumnData
//===--------------------------------------------------------------------===//
PartitionedSort::HashGroupPtr PartitionedSort::GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<PartitionedSortGlobalSourceState>();
	auto &child_strategy = gsource.gsink.child_strategy;

	OperatorSourceInput child_source {*gsource.child_sources[hash_bin], source.local_state, source.interrupt_state};
	return child_strategy.GetColumnData(0, child_source);
}

//===--------------------------------------------------------------------===//
// MaterializeSortedRun
//===--------------------------------------------------------------------===//
SourceResultType PartitionedSort::MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
                                                       OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<PartitionedSortGlobalSourceState>();

	auto &child_strategy = gsource.gsink.child_strategy;

	OperatorSourceInput child_source {*gsource.child_sources[hash_bin], source.local_state, source.interrupt_state};
	return child_strategy.MaterializeSortedRun(context, 0, child_source);
}

//===--------------------------------------------------------------------===//
// GetSortedRun
//===--------------------------------------------------------------------===//
PartitionedSort::SortedRunPtr PartitionedSort::GetSortedRun(ClientContext &client, idx_t hash_bin,
                                                            OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<PartitionedSortGlobalSourceState>();
	auto &child_strategy = gsource.gsink.child_strategy;

	OperatorSourceInput child_source {*gsource.child_sources[hash_bin], source.local_state, source.interrupt_state};
	return child_strategy.GetSortedRun(client, 0, child_source);
}

}; // namespace duckdb
