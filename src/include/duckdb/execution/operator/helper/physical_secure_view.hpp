//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_secure_view.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

//! PhysicalSecureView emits exactly the data it ingests - it only marks the boundary of a secure view so that
//! EXPLAIN and EXPLAIN ANALYZE can hide the operators of the view and report their metrics as a single node.
class PhysicalSecureView : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::SECURE_VIEW;

public:
	PhysicalSecureView(PhysicalPlan &physical_plan, PhysicalOperator &child, string view_name,
	                   vector<string> pushed_filters, idx_t estimated_cardinality);

	//! The name of the view - used for printing the plan
	string view_name;
	//! The filters that were pushed into the view - reported here because the operators inside are never shown
	vector<string> pushed_filters;

public:
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	bool ParallelOperator() const override {
		return true;
	}
	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}
};

} // namespace duckdb
