//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/physical_ungrouped_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/execution/operator/aggregate/grouped_aggregate_data.hpp"
#include "duckdb/execution/operator/aggregate/distinct_aggregate_data.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/execution/radix_partitioned_hashtable.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

//! PhysicalUngroupedAggregate is an aggregate operator that can only perform aggregates without any groups
class PhysicalUngroupedAggregate : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::UNGROUPED_AGGREGATE;

public:
	PhysicalUngroupedAggregate(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                           vector<unique_ptr<Expression>> expressions, idx_t estimated_cardinality,
	                           TupleDataValidityType distinct_validity);

	//! The aggregates that have to be computed
	vector<unique_ptr<Expression>> aggregates;
	unique_ptr<DistinctAggregateData> distinct_data;
	unique_ptr<DistinctAggregateCollectionInfo> distinct_collection_info;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}
	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}

	bool SinkOrderDependent() const override;

private:
	//! Finalize the distinct aggregates
	SinkFinalizeType FinalizeDistinct(Pipeline &pipeline, Event &event, ClientContext &context,
	                                  GlobalSinkState &gstate) const;
	//! Combine the distinct aggregates
	void CombineDistinct(ExecutionContext &context, OperatorSinkCombineInput &input) const;
	//! Sink the distinct aggregates
	void SinkDistinct(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const;
};

} // namespace duckdb
