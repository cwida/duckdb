//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_explain.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalExplain : public LogicalOperator {
	explicit LogicalExplain(ExplainType explain_type)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_EXPLAIN), explain_type(explain_type) {};

public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_EXPLAIN;

public:
	LogicalExplain(unique_ptr<LogicalOperator> plan, ExplainType explain_type, const ProfilerPrintFormat &format);

	ExplainType explain_type;
	ProfilerPrintFormat format;
	string physical_plan;
	string logical_plan_unopt;
	string logical_plan_opt;
	vector<Identifier> sql_output_names;
	bool allow_unsupported_sql = false;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	unique_ptr<LogicalOperator> CreateSQLResult(ClientContext &context, TableIndex table_index);

	idx_t EstimateCardinality(ClientContext &context) override;
	bool SupportSerialization() const override;

protected:
	void ResolveTypes() override;
	vector<ColumnBinding> GetColumnBindings() override;
};
} // namespace duckdb
