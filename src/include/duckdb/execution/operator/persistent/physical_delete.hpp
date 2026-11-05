//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/bound_constraint.hpp"

namespace duckdb {
class DataTable;
class DuckTableEntry;

//! Physically delete data from a table
class PhysicalDelete : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DELETE_OPERATOR;

public:
	PhysicalDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, DuckTableEntry &tableref, DataTable &table,
	               vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t row_id_index,
	               idx_t estimated_cardinality, bool return_chunk, vector<idx_t> return_columns);

	DuckTableEntry &tableref;
	DataTable &table;
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	idx_t row_id_index;
	bool return_chunk;
	vector<idx_t> return_columns;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return return_chunk;
	}

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}
};

} // namespace duckdb
