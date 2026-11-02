//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/physical_limited_distinct.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

//! PhysicalLimitedDistinct implements DISTINCT ... LIMIT N or GROUP BY ... LIMIT N
//! using a hash table with early termination once enough distinct groups are found.
class PhysicalLimitedDistinct : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::LIMITED_DISTINCT;

public:
	PhysicalLimitedDistinct(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                        vector<unique_ptr<Expression>> groups, vector<unique_ptr<Expression>> aggregates,
	                        idx_t limit, idx_t estimated_cardinality);

	//! The group-by columns
	vector<unique_ptr<Expression>> groups;
	//! The aggregate expressions (FIRST() for DISTINCT ON non-group cols; empty for plain DISTINCT)
	vector<unique_ptr<Expression>> aggregates;
	//! The types of the group columns
	vector<LogicalType> group_types;
	//! The maximum number of distinct groups to collect (includes offset, enforced by LIMIT on top)
	idx_t limit;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;
	void SourceFinished(ClientContext &context, GlobalSourceState &gstate) const override;

	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return false;
	}
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}
	bool SinkOrderDependent() const override {
		return false;
	}
	PipelineSourceConsumption GetSourceConsumption() const override {
		return PipelineSourceConsumption::MAY_STOP_EARLY;
	}

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
