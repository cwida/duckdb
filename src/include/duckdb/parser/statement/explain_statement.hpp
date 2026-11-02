//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/statement/explain_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/main/profiler/profiler_print_format.hpp"

namespace duckdb {

enum class ExplainType : uint8_t { EXPLAIN_STANDARD, EXPLAIN_ANALYZE, EXPLAIN_SQL };

class ExplainStatement : public SQLStatement {
public:
	static constexpr const StatementType TYPE = StatementType::EXPLAIN_STATEMENT;

public:
	explicit ExplainStatement(unique_ptr<SQLStatement> stmt, ExplainType explain_type = ExplainType::EXPLAIN_STANDARD,
	                          const ProfilerPrintFormat &format = ProfilerPrintFormat::Default());

	unique_ptr<SQLStatement> stmt;
	ExplainType explain_type;
	//! Internal verification may request an empty result for unsupported SQL export.
	bool allow_unsupported_sql = false;
	ProfilerPrintFormat format = ProfilerPrintFormat::Default();

protected:
	ExplainStatement(const ExplainStatement &other);

public:
	unique_ptr<SQLStatement> Copy() const override;
	string OptionsToString() const;
	string ToString() const override;
};

} // namespace duckdb
