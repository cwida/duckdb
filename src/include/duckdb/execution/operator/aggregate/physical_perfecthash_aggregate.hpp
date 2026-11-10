//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/physical_perfecthash_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/base_aggregate_hashtable.hpp"

namespace duckdb {
class ClientContext;
class PerfectAggregateHashTable;

//! PhysicalPerfectHashAggregate performs a group-by and aggregation using a perfect hash table
class PhysicalPerfectHashAggregate : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::PERFECT_HASH_GROUP_BY;

public:
	PhysicalPerfectHashAggregate(PhysicalPlan &physical_plan, ClientContext &context, vector<LogicalType> types,
	                             vector<unique_ptr<Expression>> aggregates, vector<unique_ptr<Expression>> groups,
	                             const vector<unique_ptr<BaseStatistics>> &group_stats, vector<idx_t> required_bits,
	                             idx_t estimated_cardinality);

	//! The groups
	vector<unique_ptr<Expression>> groups;
	//! The aggregates that have to be computed
	vector<unique_ptr<Expression>> aggregates;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;
	void SourceFinished(ClientContext &context, GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	//! Create a perfect aggregate hash table for this node
	unique_ptr<PerfectAggregateHashTable> CreateHT(Allocator &allocator, ClientContext &context) const;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}
	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}

	bool SinkOrderDependent() const override {
		return false;
	}

public:
	//! The group types
	vector<LogicalType> group_types;
	//! The payload types
	vector<LogicalType> payload_types;
	//! The aggregates to be computed
	vector<AggregateObject> aggregate_objects;
	//! The minimum value of each of the groups
	vector<Value> group_minima;
	//! The number of bits we need to completely cover each of the groups
	vector<idx_t> required_bits;

	//! The payload column of each distinct filter and the input column it is read from
	unordered_map<idx_t, idx_t> filter_columns;
};

} // namespace duckdb
