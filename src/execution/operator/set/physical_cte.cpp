#include "duckdb/execution/operator/set/physical_cte.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/types/batched_data_collection.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"

namespace duckdb {

enum class CTESinkExecutionState : uint8_t { ACTIVE, BLOCKED, STOPPED };
enum class CTECombineState : uint8_t { PENDING, COMBINED };

class CTEConsumerGlobalSourceState : public GlobalSourceState {
public:
	CTEConsumerGlobalSourceState(shared_ptr<PipelineBroadcastExchange> exchange_p, idx_t consumer_idx_p)
	    : exchange(std::move(exchange_p)), consumer_idx(consumer_idx_p) {
		auto scan_mode = exchange->GetConsumerScanMode(consumer_idx);
		max_threads = exchange->PreservesOrder() && scan_mode != PipelineBroadcastExchangeScanMode::BATCH
		                  ? 1
		                  : exchange->MaxThreads();
	}

	~CTEConsumerGlobalSourceState() override {
		Unregister();
	}

	idx_t MaxThreads() override {
		return max_threads;
	}

	void Unregister() {
		if (unregistered.exchange(true)) {
			return;
		}
		exchange->UnregisterConsumer(consumer_idx);
	}

	shared_ptr<PipelineBroadcastExchange> exchange;
	idx_t consumer_idx;
	idx_t max_threads;
	atomic<bool> unregistered {false};
	//! The exchange total grows while the producer outruns its estimate - keeps the scan progress monotonic
	MonotonicProgress scan_progress;
};

class CTEConsumerLocalSourceState : public LocalSourceState {
public:
	explicit CTEConsumerLocalSourceState(shared_ptr<PipelineBroadcastExchangeScanState> scan_state_p)
	    : scan_state(std::move(scan_state_p)) {
	}

	shared_ptr<PipelineBroadcastExchangeScanState> scan_state;
	optional_idx exchange_batch_index;
};

PhysicalCTEConsumerSource::PhysicalCTEConsumerSource(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                     idx_t estimated_cardinality, TableIndex cte_index,
                                                     shared_ptr<PipelineBroadcastExchange> exchange, idx_t consumer_idx)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::CTE_SCAN, std::move(types), estimated_cardinality),
      cte_index(cte_index), exchange(std::move(exchange)), consumer_idx(consumer_idx) {
}

unique_ptr<GlobalSourceState> PhysicalCTEConsumerSource::GetGlobalSourceState(ClientContext &context) const {
	return GetGlobalSourceState(context, OperatorPartitionInfo::NoPartitionInfo());
}

unique_ptr<GlobalSourceState>
PhysicalCTEConsumerSource::GetGlobalSourceState(ClientContext &context,
                                                const OperatorPartitionInfo &partition_info) const {
	return make_uniq<CTEConsumerGlobalSourceState>(exchange, consumer_idx);
}

unique_ptr<LocalSourceState> PhysicalCTEConsumerSource::GetLocalSourceState(ExecutionContext &context,
                                                                            GlobalSourceState &gstate) const {
	return make_uniq<CTEConsumerLocalSourceState>(exchange->GetScanState());
}

SourceResultType PhysicalCTEConsumerSource::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                            OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<CTEConsumerGlobalSourceState>();
	auto &lstate = input.local_state.Cast<CTEConsumerLocalSourceState>();
	return gstate.exchange->Scan(gstate.consumer_idx, chunk, *lstate.scan_state, lstate.exchange_batch_index,
	                             input.batch_index_state, input.interrupt_state);
}

OperatorPartitionData PhysicalCTEConsumerSource::GetPartitionData(ExecutionContext &context, DataChunk &chunk,
                                                                  GlobalSourceState &gstate, LocalSourceState &lstate_p,
                                                                  const OperatorPartitionInfo &partition_info) const {
	D_ASSERT(SupportsPartitioning(partition_info));
	auto &lstate = lstate_p.Cast<CTEConsumerLocalSourceState>();
	D_ASSERT(lstate.exchange_batch_index.IsValid());
	return OperatorPartitionData(lstate.exchange_batch_index.GetIndex());
}

bool PhysicalCTEConsumerSource::SupportsPartitioning(const OperatorPartitionInfo &partition_info) const {
	return exchange->SupportsBatchIndex() && partition_info.RequiresBatchIndex() &&
	       !partition_info.RequiresPartitionColumns();
}

OrderPreservationType PhysicalCTEConsumerSource::SourceOrder() const {
	return exchange->SourceOrder();
}

bool PhysicalCTEConsumerSource::ParallelSource() const {
	return exchange->OrderMode() != PipelineBroadcastExchangeOrderMode::SEQUENTIAL;
}

ProgressData PhysicalCTEConsumerSource::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<CTEConsumerGlobalSourceState>();
	return state.scan_progress.Update(state.exchange->ScanProgress(state.consumer_idx, estimated_cardinality));
}

void PhysicalCTEConsumerSource::SourceFinished(ClientContext &context, GlobalSourceState &gstate) const {
	gstate.Cast<CTEConsumerGlobalSourceState>().Unregister();
}

InsertionOrderPreservingMap<string> PhysicalCTEConsumerSource::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Index"] = StringUtil::Format("%llu", cte_index.index);
	auto consumer_mode = exchange->GetConsumerMode(consumer_idx);
	result["CTE Mode"] = EnumUtil::ToString(consumer_mode);
	result["Consumer"] = StringUtil::Format("%llu", consumer_idx);
	if (consumer_mode == PipelineBroadcastExchangeConsumerMode::BUFFERED) {
		result["Scan Mode"] = EnumUtil::ToString(exchange->GetConsumerScanMode(consumer_idx));
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

PhysicalCTE::PhysicalCTE(PhysicalPlan &physical_plan, Identifier ctename, TableIndex table_index,
                         vector<LogicalType> types, PhysicalOperator &top, PhysicalOperator &bottom,
                         idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::CTE, std::move(types), estimated_cardinality),
      table_index(table_index), ctename(std::move(ctename)) {
	children.push_back(top);
	children.push_back(bottom);
}

PhysicalCTE::~PhysicalCTE() {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class CTEGlobalState : public GlobalSinkState {
public:
	explicit CTEGlobalState(ClientContext &context, const PhysicalCTE &op)
	    : op(op), working_table_ref(op.working_table.get()),
	      exchange(op.UseStreamingExchange() ? op.exchange : nullptr), execution_mode(op.GetExecutionMode()) {
		ResetState(context);
	}

	~CTEGlobalState() override {
		if (exchange) {
			exchange->Cancel();
		}
	}
	const PhysicalCTE &op;
	optional_ptr<ColumnDataCollection> working_table_ref;
	shared_ptr<PipelineBroadcastExchange> exchange;
	CTEExecutionMode execution_mode;
	unique_ptr<BatchedDataCollection> ordered_data;

	annotated_mutex lhs_lock;

private:
	void ResetState(ClientContext &context) {
		if (exchange) {
			exchange->Reset();
		}
		if (execution_mode != CTEExecutionMode::STREAMING_FANOUT) {
			D_ASSERT(op.working_table);
			op.working_table->Reset();
			working_table_ref = op.working_table.get();
			if (op.use_batch_index) {
				ordered_data = make_uniq<BatchedDataCollection>(context, op.working_table->Types(),
				                                                op.working_table->GetAllocatorType());
			} else {
				ordered_data.reset();
			}
		} else {
			working_table_ref = nullptr;
			ordered_data.reset();
		}
		GlobalSinkState::Reset(context);
	}

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &context) override {
		ResetState(context);
	}

	void MergeIT(ColumnDataCollection &input) {
		annotated_lock_guard<annotated_mutex> guard(lhs_lock);
		working_table_ref->Combine(input);
	}

	void MergeBatches(BatchedDataCollection &input) {
		annotated_lock_guard<annotated_mutex> guard(lhs_lock);
		D_ASSERT(ordered_data);
		ordered_data->Merge(input);
	}

	void FinalizeBatches() {
		annotated_lock_guard<annotated_mutex> guard(lhs_lock);
		if (!ordered_data) {
			return;
		}
		D_ASSERT(working_table_ref);
		auto collection = ordered_data->FetchCollection();
		working_table_ref->Combine(*collection);
		ordered_data.reset();
	}
};

class CTELocalState : public LocalSinkState {
public:
	explicit CTELocalState(ExecutionContext &context, const PhysicalCTE &op)
	    : execution_mode(op.GetExecutionMode()), use_batch_index(op.use_batch_index) {
		if (execution_mode != CTEExecutionMode::STREAMING_FANOUT) {
			D_ASSERT(op.working_table);
			if (use_batch_index) {
				lhs_batches = make_uniq<BatchedDataCollection>(context.client, op.working_table->Types(),
				                                               op.working_table->GetAllocatorType());
			} else {
				lhs_data = make_uniq<ColumnDataCollection>(context.client, op.working_table->Types());
				lhs_data->InitializeAppend(append_state);
			}
		}
		if (execution_mode != CTEExecutionMode::MATERIALIZED) {
			D_ASSERT(op.exchange);
			D_ASSERT(context.pipeline);
			exchange_state = op.exchange->GetLocalState(context.client, context.pipeline->GetBaseBatchIndex());
		}
	}

	unique_ptr<LocalSinkState> distinct_state;
	unique_ptr<ColumnDataCollection> lhs_data;
	unique_ptr<BatchedDataCollection> lhs_batches;
	ColumnDataAppendState append_state;
	CTEExecutionMode execution_mode;
	bool use_batch_index;
	CTESinkExecutionState sink_execution_state = CTESinkExecutionState::ACTIVE;
	// Combine can be retried after a blocked exchange finish.
	CTECombineState combine_state = CTECombineState::PENDING;
	unique_ptr<PipelineBroadcastExchangeLocalState> exchange_state;

	void Append(DataChunk &input) {
		D_ASSERT(execution_mode != CTEExecutionMode::STREAMING_FANOUT);
		if (use_batch_index) {
			D_ASSERT(lhs_batches);
			D_ASSERT(partition_info.batch_index.IsValid());
			lhs_batches->Append(input, partition_info.batch_index.GetIndex());
		} else {
			D_ASSERT(lhs_data);
			lhs_data->Append(append_state, input);
		}
	}
};

unique_ptr<GlobalSinkState> PhysicalCTE::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<CTEGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalCTE::GetLocalSinkState(ExecutionContext &context) const {
	auto state = make_uniq<CTELocalState>(context, *this);
	return std::move(state);
}

SinkResultType PhysicalCTE::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CTEGlobalState>();
	auto &lstate = input.local_state.Cast<CTELocalState>();
	SinkResultType result = SinkResultType::NEED_MORE_INPUT;
	if (lstate.execution_mode != CTEExecutionMode::MATERIALIZED) {
		D_ASSERT(gstate.exchange);
		D_ASSERT(lstate.exchange_state);
		result = gstate.exchange->Push(chunk, *lstate.exchange_state, lstate.partition_info, input.interrupt_state);
		if (result == SinkResultType::BLOCKED) {
			if (lstate.sink_execution_state != CTESinkExecutionState::BLOCKED) {
				DUCKDB_LOG(context.client, PhysicalOperatorLogType, *this, "PhysicalCTE", "ExchangeBlocked",
				           {{"mode", EnumUtil::ToString(lstate.execution_mode)}});
				lstate.sink_execution_state = CTESinkExecutionState::BLOCKED;
			}
			return result;
		}
		if (result == SinkResultType::FINISHED && lstate.sink_execution_state != CTESinkExecutionState::STOPPED) {
			DUCKDB_LOG(context.client, PhysicalOperatorLogType, *this, "PhysicalCTE", "ProducerStoppedEarly",
			           {{"mode", EnumUtil::ToString(lstate.execution_mode)}});
			lstate.sink_execution_state = CTESinkExecutionState::STOPPED;
		} else if (result != SinkResultType::FINISHED) {
			lstate.sink_execution_state = CTESinkExecutionState::ACTIVE;
		}
	}
	if (lstate.execution_mode != CTEExecutionMode::STREAMING_FANOUT) {
		// In hybrid mode, append only after a successful exchange push; blocked pushes retry the same chunk.
		lstate.Append(chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}

	return result;
}

SinkNextBatchType PhysicalCTE::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<CTEGlobalState>();
	auto &lstate = input.local_state.Cast<CTELocalState>();
	if (lstate.execution_mode == CTEExecutionMode::MATERIALIZED) {
		return SinkNextBatchType::READY;
	}
	D_ASSERT(gstate.exchange);
	D_ASSERT(lstate.exchange_state);
	return gstate.exchange->NextBatch(*lstate.exchange_state, lstate.partition_info, input.interrupt_state);
}

SinkNextBatchType PhysicalCTE::UpdateMinBatchIndex(ExecutionContext &, OperatorSinkNextBatchInput &input) const {
	auto &gstate = input.global_state.Cast<CTEGlobalState>();
	auto &lstate = input.local_state.Cast<CTELocalState>();
	if (lstate.execution_mode == CTEExecutionMode::MATERIALIZED) {
		return SinkNextBatchType::READY;
	}
	D_ASSERT(gstate.exchange);
	D_ASSERT(lstate.exchange_state);
	return gstate.exchange->UpdateMinBatchIndex(*lstate.exchange_state, lstate.partition_info, input.interrupt_state);
}

SinkCombineResultType PhysicalCTE::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &lstate = input.local_state.Cast<CTELocalState>();
	if (lstate.execution_mode != CTEExecutionMode::MATERIALIZED) {
		D_ASSERT(lstate.exchange_state);
		auto result = exchange->FinishLocal(*lstate.exchange_state, lstate.partition_info, input.interrupt_state);
		if (result == SinkCombineResultType::BLOCKED) {
			return result;
		}
	}
	if (lstate.execution_mode != CTEExecutionMode::STREAMING_FANOUT &&
	    lstate.combine_state == CTECombineState::PENDING) {
		auto &gstate = input.global_state.Cast<CTEGlobalState>();
		if (lstate.use_batch_index) {
			D_ASSERT(lstate.lhs_batches);
			gstate.MergeBatches(*lstate.lhs_batches);
		} else {
			D_ASSERT(lstate.lhs_data);
			gstate.MergeIT(*lstate.lhs_data);
		}
		lstate.combine_state = CTECombineState::COMBINED;
	}

	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalCTE::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                       OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<CTEGlobalState>();
	gstate.FinalizeBatches();
	if (exchange && UseStreamingExchange()) {
		gstate.exchange->Finish();
		gstate.exchange->FinishDirectConsumers();
		DUCKDB_LOG(context, PhysicalOperatorLogType, *this, "PhysicalCTE", "ProducerFinished",
		           {{"mode", EnumUtil::ToString(gstate.execution_mode)}});
	}
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalCTE::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	D_ASSERT(children.size() == 2);
	pipeline_selection_state = CTEPipelineSelectionState::UNRESOLVED;
	preferred_batch_size = optional_idx();
	conflicting_batch_sizes = false;
	op_state.reset();
	sink_state.reset();
	if (exchange) {
		exchange->SetLogOperator(*this);
		// Prepared plans rebuild pipelines while keeping the physical CTE and consumer indexes.
		exchange->ResetConsumerRegistrations();
	}

	if (meta_pipeline.HasRecursiveCTE() && use_batch_index) {
		use_batch_index = false;
		parallel = false;
	}

	auto &state = meta_pipeline.GetState();

	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(
	    current, *this, MetaPipelineType::REGULAR,
	    exchange ? MetaPipelineDependencyMode::NO_DEPENDENCY : MetaPipelineDependencyMode::ADD_DEPENDENCY);
	child_meta_pipeline.Build(children[0]);
	if (exchange) {
		vector<shared_ptr<Pipeline>> producer_pipelines;
		child_meta_pipeline.GetPipelines(producer_pipelines, false);
		exchange->SetProducerPipelines(producer_pipelines);
	}

	for (auto &cte_scan : cte_scans) {
		state.cte_dependencies.insert(make_pair(cte_scan, reference<Pipeline>(*child_meta_pipeline.GetBasePipeline())));
	}

	// If the CTE body has side effects, all MetaPipelines created while building
	// children[1] (the query side) must run after the CTE completes.
	// We follow the same pattern as PhysicalJoin::BuildJoinPipelines: capture the side-effecting pipelines
	// and the current last child before building children[1], then call AddRecursiveDependencies
	// with RecursiveDependencyMode::FORCE so ordering is always enforced (not just when pipelines exceed the
	// thread count, as is the case for join build dependencies).
	vector<shared_ptr<Pipeline>> side_effect_pipelines;
	optional_ptr<MetaPipeline> last_child_ptr;
	if (cte_body_has_side_effects) {
		child_meta_pipeline.GetPipelines(side_effect_pipelines, false);
		last_child_ptr = meta_pipeline.GetLastChild();
	}

	children[1].get().BuildPipelines(current, meta_pipeline);
	auto has_unresolved_consumers = exchange && exchange->GetConsumerSummary().unresolved > 0;
	pipeline_selection_state =
	    has_unresolved_consumers ? CTEPipelineSelectionState::UNRESOLVED : CTEPipelineSelectionState::RESOLVED;

	bool dependency_added = false;
	if (exchange && !UseStreamingExchange()) {
		// Keep the materialized fallback until graph-dependent consumer selection has completed.
		auto cte_pipeline = child_meta_pipeline.GetBasePipeline();
		current.AddDependency(cte_pipeline);
		dependency_added = true;
	}
	if (exchange && last_child_ptr && !current.HasDataflowDependencies()) {
		for (auto &side_effect_pipeline : side_effect_pipelines) {
			current.AddDependency(side_effect_pipeline);
		}
	}
	if (last_child_ptr) {
		meta_pipeline.AddRecursiveDependencies(side_effect_pipelines, *last_child_ptr, RecursiveDependencyMode::FORCE,
		                                       exchange ? DataflowDependencyMode::SKIP_CONFLICTING
		                                                : DataflowDependencyMode::INCLUDE);
	}
	if (has_unresolved_consumers) {
		state.AddCTEPipelineSelection(*this, current, child_meta_pipeline.GetBasePipeline(), dependency_added);
	}
}

bool PhysicalCTE::TryRegisterDirectConsumer(Pipeline &pipeline, idx_t consumer_idx) {
	if (!CanRegisterDirectConsumer(pipeline)) {
		return false;
	}
	pipeline.SetExternalInput(GetProducerPipelines());
	RegisterDirectConsumer(pipeline, consumer_idx);
	return true;
}

bool PhysicalCTE::CanRegisterDirectConsumer(Pipeline &pipeline) const {
	return exchange && exchange->CanRegisterDirectConsumer(pipeline);
}

void PhysicalCTE::RegisterDirectConsumer(Pipeline &pipeline, idx_t consumer_idx) {
	D_ASSERT(exchange);
	exchange->SelectDirectConsumer(pipeline, consumer_idx);
	RegisterBatchPreference(pipeline);
}

vector<reference<Pipeline>> PhysicalCTE::GetProducerPipelines() const {
	D_ASSERT(exchange);
	return exchange->GetProducerPipelines();
}

void PhysicalCTE::SetPipelineSelectionResolved() {
	D_ASSERT(pipeline_selection_state == CTEPipelineSelectionState::UNRESOLVED);
	pipeline_selection_state = CTEPipelineSelectionState::RESOLVED;
}

bool PhysicalCTE::ShouldUseBufferedConsumer(Pipeline &pipeline) const {
	if (!exchange) {
		return false;
	}
	if (exchange->RegisteredConsumerCount() == 1) {
		return true;
	}
	// Multi-consumer all-data fanout is cheaper through the materialized table. Keep buffered exchange only
	// for consumers that can make streaming observable by stopping before source exhaustion.
	return pipeline.CanStopSourceEarly();
}

void PhysicalCTE::RegisterBufferedConsumer(Pipeline &pipeline, idx_t consumer_idx) {
	D_ASSERT(exchange);
	auto scan_mode = PipelineBroadcastExchangeScanMode::CHUNK;
	auto sink = pipeline.GetSink();
	if (sink) {
		auto required_partition_info = sink->RequiredPartitionInfo();
		if (exchange->SupportsBatchIndex() && required_partition_info.RequiresBatchIndex() &&
		    !required_partition_info.RequiresPartitionColumns()) {
			scan_mode = PipelineBroadcastExchangeScanMode::BATCH;
		}
	}
	exchange->SelectBufferedConsumer(consumer_idx, scan_mode);
	RegisterBatchPreference(pipeline);
}

void PhysicalCTE::RegisterMaterializedConsumer(idx_t consumer_idx) {
	D_ASSERT(exchange);
	// Materialized consumers must not keep the exchange producer active or force exchange buffering.
	exchange->SelectMaterializedConsumer(consumer_idx);
}

CTEExecutionMode PhysicalCTE::GetExecutionMode() const {
	if (!exchange) {
		return CTEExecutionMode::MATERIALIZED;
	}
	auto summary = exchange->GetConsumerSummary();
	if (summary.ExchangeConsumerCount() == 0) {
		return CTEExecutionMode::MATERIALIZED;
	}
	if (summary.materialized > 0) {
		return CTEExecutionMode::HYBRID_FANOUT;
	}
	return CTEExecutionMode::STREAMING_FANOUT;
}

void PhysicalCTE::RegisterBatchPreference(Pipeline &pipeline) {
	auto sink = pipeline.GetSink();
	if (!sink) {
		return;
	}
	auto partition_info = sink->RequiredPartitionInfo();
	if (!partition_info.RequiresBatchIndex() || !partition_info.preferred_batch_size.IsValid() ||
	    conflicting_batch_sizes) {
		return;
	}
	auto batch_size = partition_info.preferred_batch_size.GetIndex();
	if (!preferred_batch_size.IsValid()) {
		preferred_batch_size = batch_size;
	} else if (preferred_batch_size.GetIndex() != batch_size) {
		preferred_batch_size = optional_idx();
		conflicting_batch_sizes = true;
	}
}

bool PhysicalCTE::UseStreamingExchange() const {
	return GetExecutionMode() != CTEExecutionMode::MATERIALIZED;
}

vector<const_reference<PhysicalOperator>> PhysicalCTE::GetSources() const {
	return children[1].get().GetSources();
}

InsertionOrderPreservingMap<string> PhysicalCTE::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	if (exchange && pipeline_selection_state == CTEPipelineSelectionState::UNRESOLVED) {
		result["Execution Mode"] = "PIPELINE_DEPENDENT";
		if (exchange->RunToCompletion()) {
			result["Run To Completion"] = "true";
		}
		SetEstimatedCardinality(result, estimated_cardinality);
		return result;
	}
	auto execution_mode = GetExecutionMode();
	result["Execution Mode"] = EnumUtil::ToString(execution_mode);
	if (UseStreamingExchange() && exchange->RunToCompletion()) {
		result["Run To Completion"] = "true";
	}
	if (UseStreamingExchange()) {
		auto summary = exchange->GetConsumerSummary();
		auto direct_count = summary.direct;
		auto consumer_count = summary.ExchangeConsumerCount();
		D_ASSERT(direct_count <= consumer_count);
		auto buffered_count = summary.buffered;
		if (direct_count == 0) {
			result["Fanout Mode"] = "BUFFERED";
		} else if (direct_count == consumer_count) {
			result["Fanout Mode"] = "DIRECT";
		} else {
			result["Fanout Mode"] = "MIXED";
		}
		result["Consumers"] = StringUtil::Format("%llu", consumer_count);
		result["Direct Consumers"] = StringUtil::Format("%llu", direct_count);
		result["Buffered Consumers"] = StringUtil::Format("%llu", buffered_count);
		result["Chunk Storage"] = buffered_count == 0 ? "NONE" : "POOLED/COLUMN_DATA";
	}
	if (exchange) {
		auto summary = exchange->GetConsumerSummary();
		result["Order Mode"] = EnumUtil::ToString(exchange->OrderMode());
		if (preferred_batch_size.IsValid()) {
			result["Preferred Batch Size"] = StringUtil::Format("%llu", preferred_batch_size.GetIndex());
		}
		if (summary.materialized > 0) {
			result["Materialized Consumers"] = StringUtil::Format("%llu", summary.materialized);
		}
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

ProgressData PhysicalCTE::GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
                                          const ProgressData source_progress) const {
	auto &state = gstate.Cast<CTEGlobalState>();
	if (state.exchange) {
		return state.exchange->SinkProgress(source_progress, estimated_cardinality);
	}
	annotated_lock_guard<annotated_mutex> guard(state.lhs_lock);
	if (!state.working_table_ref) {
		return ProgressData {0, 1, true};
	}
	// materializing the CTE is done once the source is exhausted
	return source_progress;
}

} // namespace duckdb
