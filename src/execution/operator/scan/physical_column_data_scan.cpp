#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"

#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

static idx_t ColumnDataScanBatchCount(idx_t count, idx_t batch_size) {
	return MaxValue<idx_t>(count / batch_size + (count % batch_size != 0), 1);
}

static idx_t GetColumnDataScanBatchSize(ClientContext &context, idx_t collection_count,
                                        const OperatorPartitionInfo &partition_info) {
	if (ClientConfig::GetConfig(context).verify_parallelism) {
		return STANDARD_VECTOR_SIZE;
	}
	if (!partition_info.RequiresBatchIndex()) {
		return STANDARD_VECTOR_SIZE;
	}
	idx_t preferred_batch_size = DEFAULT_ROW_GROUP_SIZE;
	if (partition_info.preferred_batch_size.IsValid() && partition_info.preferred_batch_size.GetIndex() > 0) {
		preferred_batch_size = partition_info.preferred_batch_size.GetIndex();
	}
	auto thread_count = MaxValue<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads(), 1);
	auto rows_per_thread = collection_count / thread_count + (collection_count % thread_count != 0);
	auto parallelism_cap = MaxValue<idx_t>(STANDARD_VECTOR_SIZE, rows_per_thread);
	return MinValue(preferred_batch_size, parallelism_cap);
}

static vector<column_t> GenerateColumnDataColumnIds(const vector<LogicalType> &types) {
	vector<column_t> column_ids;
	column_ids.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		column_ids.push_back(i);
	}
	return column_ids;
}

PhysicalColumnDataScan::PhysicalColumnDataScan(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                               PhysicalOperatorType op_type, idx_t estimated_cardinality,
                                               optionally_owned_ptr<ColumnDataCollection> collection_p)
    : PhysicalOperator(physical_plan, op_type, std::move(types), estimated_cardinality),
      collection(std::move(collection_p)), column_ids(GenerateColumnDataColumnIds(this->types)) {
}

PhysicalColumnDataScan::PhysicalColumnDataScan(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                               PhysicalOperatorType op_type, idx_t estimated_cardinality,
                                               optionally_owned_ptr<ColumnDataCollection> collection_p,
                                               vector<column_t> column_ids_p)
    : PhysicalOperator(physical_plan, op_type, std::move(types), estimated_cardinality),
      collection(std::move(collection_p)), column_ids(std::move(column_ids_p)) {
	D_ASSERT(this->types.size() == column_ids.size());
}

PhysicalColumnDataScan::PhysicalColumnDataScan(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                               PhysicalOperatorType op_type, idx_t estimated_cardinality,
                                               TableIndex cte_index)
    : PhysicalOperator(physical_plan, op_type, std::move(types), estimated_cardinality), collection(nullptr),
      column_ids(GenerateColumnDataColumnIds(this->types)), cte_index(cte_index) {
}

struct ColumnDataScanEntry {
	ColumnDataScanEntry(idx_t chunk_index_p, idx_t segment_index_p, idx_t row_index_p)
	    : chunk_index(chunk_index_p), segment_index(segment_index_p), row_index(row_index_p) {
	}

	idx_t chunk_index;
	idx_t segment_index;
	idx_t row_index;
};

class PhysicalColumnDataGlobalScanState : public GlobalSourceState {
public:
	PhysicalColumnDataGlobalScanState(ClientContext &context, const ColumnDataCollection &collection,
	                                  const vector<column_t> &column_ids, const OperatorPartitionInfo &partition_info) {
		collection.InitializeScan(global_scan_state, column_ids);
		auto batch_size = GetColumnDataScanBatchSize(context, collection.Count(), partition_info);
		auto chunk_count = collection.ChunkCount();
		scan_entries.reserve(chunk_count);
		batch_offsets.reserve(MinValue(chunk_count, ColumnDataScanBatchCount(collection.Count(), batch_size)) + 1);
		batch_offsets.push_back(0);

		idx_t batch_rows = 0;
		idx_t chunk_index;
		idx_t segment_index;
		idx_t row_index;
		while (collection.NextScanIndex(global_scan_state.scan_state, chunk_index, segment_index, row_index)) {
			scan_entries.emplace_back(chunk_index, segment_index, row_index);
			batch_rows += collection.GetSegments()[segment_index]->chunk_data[chunk_index].count;
			if (batch_rows >= batch_size) {
				batch_offsets.push_back(scan_entries.size());
				batch_rows = 0;
			}
		}
		if (batch_offsets.back() != scan_entries.size()) {
			batch_offsets.push_back(scan_entries.size());
		}
	}

	idx_t MaxThreads() override {
		return MaxValue<idx_t>(batch_offsets.size() - 1, 1);
	}

public:
	ColumnDataParallelScanState global_scan_state;
	vector<ColumnDataScanEntry> scan_entries;
	//! Batch boundaries in scan_entries, including the end sentinel.
	vector<idx_t> batch_offsets;
	atomic<idx_t> next_batch_index {0};
};

class PhysicalColumnDataLocalScanState : public LocalSourceState {
public:
	bool AssignTask(PhysicalColumnDataGlobalScanState &gstate) {
		// Metadata is immutable; the atomic ticket assigns exclusive ownership of an ordered batch.
		batch_index = gstate.next_batch_index.fetch_add(1, std::memory_order_relaxed);
		if (batch_index >= gstate.batch_offsets.size() - 1) {
			batch_index = DConstants::INVALID_INDEX;
			return false;
		}
		entry_index = gstate.batch_offsets[batch_index];
		entry_end = gstate.batch_offsets[batch_index + 1];
		return true;
	}

	ColumnDataLocalScanState local_scan_state;
	idx_t entry_index = 0;
	idx_t entry_end = 0;
	idx_t batch_index = DConstants::INVALID_INDEX;
};

unique_ptr<GlobalSourceState> PhysicalColumnDataScan::GetGlobalSourceState(ClientContext &context) const {
	if (!collection) {
		return make_uniq<GlobalSourceState>();
	}
	return GetGlobalSourceState(context, OperatorPartitionInfo::NoPartitionInfo());
}

unique_ptr<GlobalSourceState>
PhysicalColumnDataScan::GetGlobalSourceState(ClientContext &context,
                                             const OperatorPartitionInfo &partition_info) const {
	if (!collection) {
		return make_uniq<GlobalSourceState>();
	}
	return make_uniq<PhysicalColumnDataGlobalScanState>(context, *collection, column_ids, partition_info);
}

unique_ptr<LocalSourceState> PhysicalColumnDataScan::GetLocalSourceState(ExecutionContext &,
                                                                         GlobalSourceState &) const {
	return make_uniq<PhysicalColumnDataLocalScanState>();
}

SourceResultType PhysicalColumnDataScan::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<PhysicalColumnDataGlobalScanState>();
	auto &lstate = input.local_state.Cast<PhysicalColumnDataLocalScanState>();
	if (lstate.entry_index >= lstate.entry_end && !lstate.AssignTask(gstate)) {
		chunk.Reset();
		return SourceResultType::FINISHED;
	}

	auto &entry = gstate.scan_entries[lstate.entry_index++];
	if (column_ids.empty()) {
		chunk.Reset();
		auto &chunk_data = collection->GetSegments()[entry.segment_index]->chunk_data[entry.chunk_index];
		chunk.SetChildCardinality(chunk_data.count);
	} else {
		collection->ScanAtIndex(gstate.global_scan_state, lstate.local_scan_state, chunk, entry.chunk_index,
		                        entry.segment_index, entry.row_index);
	}
	return SourceResultType::HAVE_MORE_OUTPUT;
}

bool PhysicalColumnDataScan::SupportsPartitioning(const OperatorPartitionInfo &partition_info) const {
	if (partition_info.RequiresPartitionColumns()) {
		return false;
	}
	if (!partition_info.RequiresBatchIndex()) {
		return false;
	}
	if (type == PhysicalOperatorType::CTE_SCAN && cte_source) {
		return cte_source->SupportsPartitioning(partition_info);
	}
	return type == PhysicalOperatorType::COLUMN_DATA_SCAN || type == PhysicalOperatorType::CTE_SCAN;
}

OperatorPartitionData PhysicalColumnDataScan::GetPartitionData(ExecutionContext &context, DataChunk &chunk,
                                                               GlobalSourceState &gstate, LocalSourceState &lstate_p,
                                                               const OperatorPartitionInfo &partition_info) const {
	D_ASSERT(SupportsPartitioning(partition_info));
	auto &lstate = lstate_p.Cast<PhysicalColumnDataLocalScanState>();
	return OperatorPartitionData(lstate.batch_index);
}

ProgressData PhysicalColumnDataScan::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	if (!collection) {
		ProgressData progress;
		progress.SetInvalid();
		return progress;
	}
	auto &state = gstate.Cast<PhysicalColumnDataGlobalScanState>();
	auto next_batch = state.next_batch_index.load(std::memory_order_relaxed);
	auto next_entry =
	    next_batch < state.batch_offsets.size() - 1 ? state.batch_offsets[next_batch] : state.scan_entries.size();
	ProgressData progress;
	progress.done =
	    double(next_entry < state.scan_entries.size() ? state.scan_entries[next_entry].row_index : collection->Count());
	progress.total = double(MaxValue<idx_t>(collection->Count(), 1));
	return progress;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalColumnDataScan::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	// check if there is any additional action we need to do depending on the type
	auto &state = meta_pipeline.GetState();
	switch (type) {
	case PhysicalOperatorType::DELIM_SCAN: {
		auto entry = state.delim_join_dependencies.find(*this);
		D_ASSERT(entry != state.delim_join_dependencies.end());
		// this chunk scan introduces a dependency to the current pipeline
		// namely a dependency on the duplicate elimination pipeline to finish
		auto delim_dependency = entry->second.get().shared_from_this();
		auto delim_sink = state.GetPipelineSink(*delim_dependency);
		D_ASSERT(delim_sink);
		D_ASSERT(delim_sink->type == PhysicalOperatorType::LEFT_DELIM_JOIN ||
		         delim_sink->type == PhysicalOperatorType::RIGHT_DELIM_JOIN);
		auto &delim_join = delim_sink->Cast<PhysicalDelimJoin>();
		current.AddDependency(delim_dependency);
		state.SetPipelineSource(current, delim_join.distinct.Cast<PhysicalOperator>());
		return;
	}
	case PhysicalOperatorType::CTE_SCAN: {
		if (cte_source) {
			auto entry = state.cte_dependencies.find(*this);
			D_ASSERT(entry != state.cte_dependencies.end());
			auto cte_dependency = entry->second.get().shared_from_this();
			auto cte_sink = state.GetPipelineSink(*cte_dependency);
			D_ASSERT(cte_sink);
			D_ASSERT(cte_sink->type == PhysicalOperatorType::CTE);
			auto &cte = cte_sink->Cast<PhysicalCTE>();
			auto &source = cte_source->Cast<PhysicalCTEConsumerSource>();
			// Prefer direct fanout. Buffered exchange is only used when it can avoid full materialization or
			// when the consumer can stop early; otherwise this scan reads the materialized working table.
			auto has_blocking_dependencies = !current.GetDependencies().empty();
			if (cte.CanRegisterDirectConsumer(current) &&
			    (!cte.cte_body_has_side_effects || has_blocking_dependencies)) {
				D_ASSERT(collection);
				current.AddDependency(cte_dependency);
				state.AddExternalInputCandidate(current, *this, *cte_source, cte_dependency, cte, source.consumer_idx);
				state.SetPipelineSource(current, *this);
				return;
			}
			if (cte.TryRegisterDirectConsumer(current, source.consumer_idx)) {
				DUCKDB_LOG(current.GetClientContext(), PhysicalOperatorLogType, cte, "PhysicalCTE", "SelectConsumer",
				           {{"consumer", to_string(source.consumer_idx)}, {"mode", "DIRECT"}});
				state.SetPipelineSource(current, *cte_source);
				return;
			}
			if (cte.ShouldUseBufferedConsumer(current)) {
				cte.RegisterBufferedConsumer(current, source.consumer_idx);
				current.AddDataflowDependency(cte_dependency);
				DUCKDB_LOG(current.GetClientContext(), PhysicalOperatorLogType, cte, "PhysicalCTE", "SelectConsumer",
				           {{"consumer", to_string(source.consumer_idx)}, {"mode", "BUFFERED"}});
				state.SetPipelineSource(current, *cte_source);
				return;
			}
			D_ASSERT(collection);
			cte.RegisterMaterializedConsumer(source.consumer_idx);
			current.AddDependency(cte_dependency);
			DUCKDB_LOG(current.GetClientContext(), PhysicalOperatorLogType, cte, "PhysicalCTE", "SelectConsumer",
			           {{"consumer", to_string(source.consumer_idx)}, {"mode", "MATERIALIZED"}});
			state.SetPipelineSource(current, *this);
			return;
		}
		auto entry = state.cte_dependencies.find(*this);
		D_ASSERT(entry != state.cte_dependencies.end());
		// this chunk scan introduces a dependency to the current pipeline
		// namely a dependency on the CTE pipeline to finish
		auto cte_dependency = entry->second.get().shared_from_this();
		auto cte_sink = state.GetPipelineSink(*cte_dependency);
		(void)cte_sink;
		D_ASSERT(cte_sink);
		D_ASSERT(cte_sink->type == PhysicalOperatorType::CTE);
		current.AddDependency(cte_dependency);
		state.SetPipelineSource(current, *this);
		return;
	}
	case PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN:
	case PhysicalOperatorType::RECURSIVE_CTE_SCAN:
		if (!meta_pipeline.HasRecursiveCTE()) {
			throw InternalException("Recursive CTE scan found without recursive CTE node");
		}
		break;
	default:
		break;
	}
	D_ASSERT(children.empty());
	state.SetPipelineSource(current, *this);
}

InsertionOrderPreservingMap<string> PhysicalColumnDataScan::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	switch (type) {
	case PhysicalOperatorType::DELIM_SCAN:
		if (delim_index.IsValid()) {
			result["Delim Index"] = StringUtil::Format("%llu", delim_index.GetIndex());
		}
		break;
	case PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN:
	case PhysicalOperatorType::CTE_SCAN:
	case PhysicalOperatorType::RECURSIVE_CTE_SCAN: {
		result["CTE Index"] = StringUtil::Format("%llu", cte_index.index);
		break;
	}
	default:
		break;
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
