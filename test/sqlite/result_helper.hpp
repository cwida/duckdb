//===----------------------------------------------------------------------===//
//                         DuckDB
//
// result_comparison.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "sqllogic_command.hpp"

namespace duckdb {
class SQLLogicTestRunner;
class SQLLogicTestLogger;

class TestResultHelper {
public:
	TestResultHelper(SQLLogicTestRunner &runner) : runner(runner) {
	}

	SQLLogicTestRunner &runner;

public:
	bool CheckQueryResult(const Query &query, ExecuteContext &context, duckdb::unique_ptr<QueryResult> owned_result);
	optional<string> EvaluateStatementResult(SQLLogicTestLogger &logger, const Statement &statement,
	                                         ExecuteContext &context, const optional<string> &error) const;
	bool ErrorMatchesExpected(SQLLogicTestLogger &logger, const string &expected, const string &actual) const;
	bool CheckStatementResult(const Statement &statement, ExecuteContext &context,
	                          duckdb::unique_ptr<QueryResult> owned_result);
	string SQLLogicTestConvertValue(Value value, LogicalType sql_type, bool original_sqlite_test);
	void DuckDBConvertResult(QueryResult &result, bool original_sqlite_test, vector<string> &out_result);

	static bool ResultIsHash(const string &result);
	static bool ResultIsFile(string result);
	void SortQueryResult(SortStyle sort_style, vector<string> &result, idx_t ncols);

	bool MatchesRegex(SQLLogicTestLogger &logger, string lvalue_str, string rvalue_str) const;
	bool CompareValues(SQLLogicTestLogger &logger, QueryResult &result, string lvalue_str, string rvalue_str,
	                   idx_t current_row, idx_t current_column, vector<string> &values, idx_t expected_column_count,
	                   bool row_wise, vector<string> &result_values, bool print_error = true);
	bool SkipErrorMessage(const string &message);
	bool SkipLoggingSameError(const string &file_name);

	vector<string> LoadResultFromFile(string fname, vector<string> names, idx_t &expected_column_count, string &error);
};

} // namespace duckdb
