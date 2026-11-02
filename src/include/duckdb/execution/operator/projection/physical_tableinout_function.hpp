//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/projection/physical_tableinout_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/table_filter_set.hpp"

namespace duckdb {

class PhysicalTableInOutFunction : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::INOUT_FUNCTION;

public:
	PhysicalTableInOutFunction(PhysicalPlan &physical_plan, vector<LogicalType> types, BoundTableFunction function_p,
	                           unique_ptr<FunctionData> bind_data_p, vector<ColumnIndex> column_ids_p,
	                           vector<idx_t> projection_ids_p, unique_ptr<TableFilterSet> table_filters_p,
	                           idx_t estimated_cardinality, vector<column_t> projected_input);

public:
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override;
	static void SetOrdinality(DataChunk &chunk, const optional_idx &ordinality_column_idx, const idx_t &ordinality_idx,
	                          const idx_t &ordinality);
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;
	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk, GlobalOperatorState &gstate,
	                                        OperatorState &state) const override;

	bool ParallelOperator() const override {
		return true;
	}

	bool RequiresFinalExecute() const override {
		return function.in_out_function_final;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	//! Information for WITH ORDINALITY
	optional_idx ordinality_idx;

	OrderPreservationType OperatorOrder() const override {
		return function.order_preservation_type;
	}

private:
	//! The table function
	BoundTableFunction function;
	//! Bind data of the function
	unique_ptr<FunctionData> bind_data;
	//! The set of column ids to fetch
	vector<ColumnIndex> column_ids;
	//! The projected-out column ids
	vector<idx_t> projection_ids;
	//! Filters pushed into the table in-out function
	unique_ptr<TableFilterSet> table_filters;
	//! The set of input columns to project out
	vector<column_t> projected_input;
};

} // namespace duckdb
