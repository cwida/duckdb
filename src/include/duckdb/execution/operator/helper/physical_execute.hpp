//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_execute.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/prepared_statement_data.hpp"

namespace duckdb {

class PhysicalExecute : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXECUTE;

public:
	PhysicalExecute(PhysicalPlan &physical_plan, PhysicalOperator &plan, shared_ptr<PreparedStatementData> prepared);

	PhysicalOperator &plan;
	//! The prepared statement we are executing - kept alive here because it can own the plan we refer to
	shared_ptr<PreparedStatementData> prepared;

public:
	vector<const_reference<PhysicalOperator>> GetChildren() const override;
	//! Report the executed plan's sources, so an order-preserving result can be collected in parallel.
	vector<const_reference<PhysicalOperator>> GetSources() const override;

public:
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;
};

} // namespace duckdb
