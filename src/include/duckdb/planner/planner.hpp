//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/planner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"

namespace duckdb {
class ClientContext;
class PreparedStatementData;
class ExecuteStatement;

//! The planner creates a logical query plan from the parsed SQL statements
//! using the Binder and LogicalPlanGenerator.
class Planner {
	friend class Binder;

public:
	explicit Planner(ClientContext &context);

public:
	unique_ptr<LogicalOperator> plan;
	vector<Identifier> names;
	vector<LogicalType> types;
	identifier_map_t<BoundParameterData> parameter_data;

	shared_ptr<Binder> binder;
	ClientContext &context;

	StatementProperties properties;
	bound_parameter_map_t value_map;

	//! Whether a top-level EXECUTE may re-use the physical plan of its prepared statement
	bool reuse_prepared_plans = false;
	//! The prepared statement whose physical plan is re-used - set instead of plan
	shared_ptr<PreparedStatementData> reused_statement;

public:
	void CreatePlan(unique_ptr<SQLStatement> statement);
	//! Apply the execution planner's optimizer policy, including mandatory rewrites.
	void Optimize();
	static void VerifyPlan(ClientContext &context, unique_ptr<LogicalOperator> &op,
	                       optional_ptr<bound_parameter_map_t> map = nullptr);

private:
	void CreatePlan(SQLStatement &statement);
	void CreateExecutePlan(ExecuteStatement &statement);
	shared_ptr<PreparedStatementData> PrepareSQLStatement(unique_ptr<SQLStatement> statement);
};
} // namespace duckdb
