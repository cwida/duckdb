//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_limit.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

namespace duckdb {

//! PhyisicalLimit represents the LIMIT operator
class PhysicalLimit : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::LIMIT;

	static constexpr const idx_t MAX_LIMIT_VALUE = 1ULL << 62ULL;

public:
	PhysicalLimit(PhysicalPlan &physical_plan, vector<LogicalType> types, BoundLimitNode limit_val,
	              BoundLimitNode offset_val, idx_t estimated_cardinality);

	BoundLimitNode limit_val;
	BoundLimitNode offset_val;

public:
	bool SinkOrderDependent() const override {
		return true;
	}

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
	// Sink Interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}
	PipelineSourceConsumption GetSourceConsumption() const override {
		return PipelineSourceConsumption::MAY_STOP_EARLY;
	}

	OperatorPartitionInfo RequiredPartitionInfo() const override {
		return OperatorPartitionInfo::BatchIndex();
	}

public:
	static void SetInitialLimits(const BoundLimitNode &limit_val, const BoundLimitNode &offset_val, optional_idx &limit,
	                             optional_idx &offset);
	static bool ComputeOffset(ExecutionContext &context, DataChunk &input, optional_idx &limit, optional_idx &offset,
	                          idx_t current_offset, idx_t &max_element, const BoundLimitNode &limit_val,
	                          const BoundLimitNode &offset_val);
	static bool HandleOffset(DataChunk &input, idx_t &current_offset, idx_t offset, idx_t limit);
	static Value GetDelimiter(ExecutionContext &context, DataChunk &input, const Expression &expr);
	//! Reads the value of a non-constant LIMIT or OFFSET (null_value if it is NULL), which must be at most
	//! MAX_LIMIT_VALUE
	static idx_t GetLimitValue(const Value &val, idx_t null_value);

	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
