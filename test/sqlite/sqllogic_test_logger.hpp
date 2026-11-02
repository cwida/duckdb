//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqllogic_test_logger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "sqllogic_command.hpp"

namespace duckdb {

class Command;
class LoopCommand;

class SQLLogicTestLogger {
public:
	SQLLogicTestLogger(ExecuteContext &context, const Command &command);
	~SQLLogicTestLogger();

	static void Log(const string &annotation, const string &str);
	static void PrintSkip(const string &file_name, const string &reason);
	//! Emit the parseable skip marker and record the skip with the reporter
	static void ReportSkip(const string &file_name, const string &reason);
	void PrintExpectedResult(const vector<string> &values, idx_t columns, bool row_wise);
	static void PrintLineSep();
	static void PrintHeader(string header);
	void PrintFileHeader();
	void PrintSQL();
	void PrintSQLFormatted();
	void PrintErrorHeader(const string &description);
	static void PrintErrorHeader(const string &file_name, idx_t query_line, const string &description);
	void PrintResultError(const vector<string> &result_values, const vector<string> &values,
	                      idx_t expected_column_count, bool row_wise);
	static void PrintSummaryHeader(const std::string &file_name, idx_t query_line);
	void PrintResultError(QueryResult &result, const vector<string> &values, idx_t expected_column_count,
	                      bool row_wise);
	void PrintResultString(QueryResult &result);
	void UnexpectedFailure(QueryResult &result);
	void OutputResult(QueryResult &result, const vector<string> &result_values_string);
	void OutputHash(const string &hash_value);
	void ColumnCountMismatch(QueryResult &result, const vector<string> &result_values_string,
	                         idx_t expected_column_count, bool row_wise);
	void NotCleanlyDivisible(idx_t expected_column_count, idx_t actual_column_count);
	void WrongRowCount(idx_t expected_rows, QueryResult &result, const vector<string> &comparison_values,
	                   idx_t expected_column_count, bool row_wise);
	void ColumnCountMismatchCorrectResult(idx_t original_expected_columns, idx_t expected_column_count,
	                                      QueryResult &result);
	void SplitMismatch(idx_t row_number, idx_t expected_column_count, idx_t split_count);
	void WrongResultHash(const string &expected_result, QueryResult &result, const string &expected_hash,
	                     const string &actual_hash);
	void UnexpectedStatement(bool expect_ok, QueryResult &result);
	void ExpectedErrorMismatch(const string &expected_error, QueryResult &result);
	void InternalException(QueryResult &result);
	static void LoadDatabaseFail(const string &file_name, const string &dbpath, const string &message);
	//! Write a machine-readable event line: "[TEST_EVENT] <json>" (--emit-test-events). Caller gates.
	static void EmitTestEvent(const string &json_payload);

	static void AppendFailure(const string &log_message);
	static void LogFailure(const string &log_message);
	static void LogFailureAnnotation(const string &log_message);
	string ResultToString(QueryResult &result);

private:
	static mutex log_mutex;

private:
	Connection &connection;
	lock_guard<mutex> log_lock;
	string file_name;
	int query_line;
	string sql_query;
};
} // namespace duckdb
