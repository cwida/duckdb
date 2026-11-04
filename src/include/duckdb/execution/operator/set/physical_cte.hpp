//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/set/physical_cte.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class ColumnDataCollection;
class Pipeline;
class PipelineBroadcastExchange;
class RecursiveCTEState;

enum class CTEExecutionMode : uint8_t { MATERIALIZED, STREAMING_FANOUT, HYBRID_FANOUT };
enum class CTEPipelineSelectionState : uint8_t { UNRESOLVED, RESOLVED };

class PhysicalCTEConsumerSource : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::INVALID;

public:
	PhysicalCTEConsumerSource(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                          TableIndex cte_index, shared_ptr<PipelineBroadcastExchange> exchange, idx_t consumer_idx);

	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context,
	                                                   const OperatorPartitionInfo &partition_info) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	OperatorPartitionData GetPartitionData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
	                                       LocalSourceState &lstate,
	                                       const OperatorPartitionInfo &partition_info) const override;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;
	void SourceFinished(ClientContext &context, GlobalSourceState &gstate) const override;
	bool SupportsPartitioning(const OperatorPartitionInfo &partition_info) const override;
	OrderPreservationType SourceOrder() const override;

	bool IsSource() const override {
		return true;
	}

	bool ParallelSource() const override;

	InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
	TableIndex cte_index;
	shared_ptr<PipelineBroadcastExchange> exchange;
	idx_t consumer_idx;
};

class PhysicalCTE : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::CTE;

public:
	PhysicalCTE(PhysicalPlan &physical_plan, Identifier ctename, TableIndex table_index, vector<LogicalType> types,
	            PhysicalOperator &top, PhysicalOperator &bottom, idx_t estimated_cardinality);
	~PhysicalCTE() override;

	vector<const_reference<PhysicalOperator>> cte_scans;

	shared_ptr<ColumnDataCollection> working_table;
	shared_ptr<PipelineBroadcastExchange> exchange;

	TableIndex table_index;
	Identifier ctename;
	bool cte_body_has_side_effects = false;
	CTEPipelineSelectionState pipeline_selection_state = CTEPipelineSelectionState::UNRESOLVED;
	bool preserve_order = false;
	bool use_batch_index = false;
	bool parallel = true;
	optional_idx preferred_batch_size;
	bool conflicting_batch_sizes = false;

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkNextBatchType NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const override;
	SinkNextBatchType UpdateMinBatchIndex(ExecutionContext &context, OperatorSinkNextBatchInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return parallel;
	}

	OperatorPartitionInfo RequiredPartitionInfo() const override {
		return use_batch_index ? OperatorPartitionInfo::BatchIndex(preferred_batch_size)
		                       : OperatorPartitionInfo::NoPartitionInfo();
	}

	bool SinkOrderDependent() const override {
		return preserve_order;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	ProgressData GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
	                             const ProgressData source_progress) const override;

public:
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;
	bool CanRegisterDirectConsumer(Pipeline &pipeline) const;
	void RegisterDirectConsumer(Pipeline &pipeline, idx_t consumer_idx);
	bool TryRegisterDirectConsumer(Pipeline &pipeline, idx_t consumer_idx);
	vector<reference<Pipeline>> GetProducerPipelines() const;
	void SetPipelineSelectionResolved();
	bool ShouldUseBufferedConsumer(Pipeline &pipeline) const;
	void RegisterBufferedConsumer(Pipeline &pipeline, idx_t consumer_idx);
	void RegisterMaterializedConsumer(idx_t consumer_idx);
	//! Converts an already registered buffered consumer to a materialized one, before execution starts. Used when
	//! the schedule gates the consumer's execute behind its own producer's completion, so it could never drain.
	void DemoteBufferedConsumer(idx_t consumer_idx);
	CTEExecutionMode GetExecutionMode() const;
	bool UseStreamingExchange() const;

	vector<const_reference<PhysicalOperator>> GetSources() const override;

private:
	void RegisterBatchPreference(Pipeline &pipeline);
};

} // namespace duckdb
