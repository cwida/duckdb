//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_execute.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalExecute : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_EXECUTE;

public:
	explicit LogicalExecute(shared_ptr<PreparedStatementData> prepared_p)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_EXECUTE), prepared(std::move(prepared_p)) {
		D_ASSERT(prepared);
		types = prepared->types;
	}

	shared_ptr<PreparedStatementData> prepared;

public:
	//! Skips the serialization check in VerifyPlan
	bool SupportSerialization() const override {
		return false;
	}
	//! Without a rebound child plan, the prepared plan is executed as-is and was already optimized
	bool RequireOptimizer() const override {
		return !children.empty();
	}

protected:
	void ResolveTypes() override {
		types = prepared->types;
	}
	vector<ColumnBinding> GetColumnBindings() override {
		return GenerateColumnBindings(TableIndex(0), types.size());
	}
};
} // namespace duckdb
