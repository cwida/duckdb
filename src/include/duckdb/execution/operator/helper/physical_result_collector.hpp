//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_result_collector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/common/enums/statement_type.hpp"

namespace duckdb {

class PreparedStatementData;

//! PhysicalResultCollector is an abstract class that is used to generate the final result of a query
class PhysicalResultCollector : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::RESULT_COLLECTOR;

public:
	PhysicalResultCollector(PhysicalPlan &physical_plan, PreparedStatementData &data);

	StatementType statement_type;
	StatementProperties properties;
	PhysicalOperator &plan;
	vector<Identifier> names;

public:
	static unique_ptr<PhysicalOperator> GetResultCollector(ClientContext &context, PreparedStatementData &data);

public:
	//! The final method used to fetch the query result from this operator
	virtual unique_ptr<QueryResult> GetResult(GlobalSinkState &state) const = 0;

	bool IsSink() const override {
		return true;
	}

public:
	vector<const_reference<PhysicalOperator>> GetChildren() const override;
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;

	bool IsSource() const override {
		return true;
	}

public:
	//! Whether this collector produces a result that must stay open after the fetch.
	//! Custom collectors override this to keep the query alive for their stream
	virtual bool IsStreaming() const {
		return false;
	}
	//! Whether this collector builds its own result object, rather than serving it through a result buffer
	virtual bool BuildsOwnResult() const {
		return true;
	}
};

} // namespace duckdb
