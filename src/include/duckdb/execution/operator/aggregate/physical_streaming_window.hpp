//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/physical_streaming_window.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

//! PhysicalStreamingWindow implements streaming window functions (i.e. with an empty OVER clause)
class PhysicalStreamingWindow : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::STREAMING_WINDOW;

	static bool IsStreamingFunction(ClientContext &context, BoundWindowExpression &wexpr);

public:
	PhysicalStreamingWindow(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                        vector<unique_ptr<Expression>> select_list, idx_t estimated_cardinality,
	                        optional_idx partition_limit = optional_idx());

	//! The projection list of the WINDOW statement
	vector<unique_ptr<Expression>> select_list;
	//! The partition size limit (if any)
	optional_idx partition_limit;

public:
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override;

	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;

	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk, GlobalOperatorState &gstate,
	                                        OperatorState &state) const final;

	bool RequiresFinalExecute() const final {
		return true;
	}

	OrderPreservationType OperatorOrder() const override {
		return OrderPreservationType::FIXED_ORDER;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

private:
	void ExecuteFunctions(ExecutionContext &context, DataChunk &chunk, DataChunk &delayed,
	                      GlobalOperatorState &gstate_p) const;
	void ExecuteInput(ExecutionContext &context, DataChunk &delayed, DataChunk &input, DataChunk &chunk,
	                  GlobalOperatorState &gstate) const;
	void ExecuteDelayed(ExecutionContext &context, DataChunk &delayed, DataChunk &input, DataChunk &chunk,
	                    GlobalOperatorState &gstate) const;
	void ExecuteShifted(ExecutionContext &context, DataChunk &delayed, DataChunk &input, DataChunk &chunk,
	                    GlobalOperatorState &gstate) const;
};

} // namespace duckdb
