#include "sqllogic_test_logger.hpp"
#include "duckdb/parser/parser.hpp"
#include "termcolor.hpp"
#include "result_helper.hpp"
#include "sqllogic_test_runner.hpp"
#include "test_helpers.hpp"
#include "test_reporter.hpp"
#include "duckdb/common/box_renderer.hpp"
#include "duckdb/common/box_renderer_context.hpp"

namespace duckdb {

//! Process-wide: a failure report is a sequence of writes to stderr, and test files running
//! concurrently would otherwise shred each other's output.
mutex SQLLogicTestLogger::log_mutex;

SQLLogicTestLogger::SQLLogicTestLogger(ExecuteContext &context, const Command &command)
    : connection(command.CommandConnection(context)), log_lock(log_mutex), file_name(command.file_name),
      query_line(command.query_line), sql_query(context.sql_query) {
}

SQLLogicTestLogger::~SQLLogicTestLogger() {
}

void SQLLogicTestLogger::Log(const string &annotation, const string &str) {
	std::cerr << annotation << str;
	AppendFailure(str);
}

void SQLLogicTestLogger::AppendFailure(const string &log_message) {
	FailureSummary::Log(log_message);
}

void SQLLogicTestLogger::EmitTestEvent(const string &json_payload) {
	// The "[TEST_EVENT] " flare marks a line as a machine-readable event; the payload is a JSON
	// object (begin / end). Callers gate on EmitTestEventsEnabled() and pass a serialized object.
	// Build the whole line first so it reaches cerr as a single insertion — no interleaving.
	std::cerr << "[TEST_EVENT] " + json_payload + "\n";
}

void SQLLogicTestLogger::LogFailure(const string &log_message) {
	Log("", log_message);
}

void SQLLogicTestLogger::LogFailureAnnotation(const string &log_message) {
	const char *ci = std::getenv("CI");
	// check the value is "true" otherwise you'll see the prefix in local run outputs
	auto prefix = (ci && string(ci) == "true") ? "\n::error::" : "";
	Log(prefix, log_message);
}

void SQLLogicTestLogger::PrintSkip(const string &file_name, const string &reason) {
	// Opt-in via --emit-on-skip (SetEmitOnSkip): off by default so normal runs stay quiet.
	if (!EmitOnSkipEnabled()) {
		return;
	}
	// Stable, uncolored marker consumable by e.g. pytest collector. Emitted to std::cerr
	// (which survives subprocess capture) per skipped test, so skips are attributable
	// even inside a batched invocation.
	std::cerr << "[SKIP_TEST] " << file_name << " :: " << reason << "\n";
}

void SQLLogicTestLogger::ReportSkip(const string &file_name, const string &reason) {
	PrintSkip(file_name, reason);
	TestReporter::Get().Skip(reason);
}

void SQLLogicTestLogger::PrintSummaryHeader(const std::string &file_name, idx_t query_line) {
	auto failures_count = to_string(FailureSummary::GetSummaryCounter());
	if (std::getenv("NO_DUPLICATING_HEADERS") == 0) {
		LogFailure("\n" + failures_count + ". " + file_name + ":" + to_string(query_line) + "\n");
		PrintLineSep();
	}
}

void SQLLogicTestLogger::PrintExpectedResult(const vector<string> &values, idx_t columns, bool row_wise) {
	if (row_wise) {
		for (idx_t r = 0; r < values.size(); r++) {
			LogFailure("\n" + values[r]);
		}
	} else {
		idx_t c = 0;
		for (idx_t r = 0; r < values.size(); r++) {
			if (c != 0) {
				LogFailure("\t");
			}
			LogFailure(values[r]);
			c++;
			if (c >= columns) {
				LogFailure("\n");
				c = 0;
			}
		}
	}
	LogFailure("\n");
}

void SQLLogicTestLogger::PrintLineSep() {
	string line_sep = string(80, '=');
	std::ostringstream oss;
	oss << termcolor::color<128, 128, 128> << line_sep << termcolor::reset << std::endl;
	LogFailure(oss.str());
}

void SQLLogicTestLogger::PrintHeader(string header) {
	std::ostringstream oss;
	oss << termcolor::bold << header << termcolor::reset << std::endl;
	LogFailure(oss.str());
}

void SQLLogicTestLogger::PrintFileHeader() {
	PrintHeader("File " + file_name + ":" + to_string(query_line) + ")");
}

void SQLLogicTestLogger::PrintSQL() {
	string query = sql_query;
	if (StringUtil::EndsWith(sql_query, "\n")) {
		// ends with a newline: don't add one
		if (!StringUtil::EndsWith(sql_query, ";\n")) {
			// no semicolon though
			query[query.size() - 1] = ';';
		}
	} else {
		if (!StringUtil::EndsWith(sql_query, ";")) {
			query += ";";
		}
	}
	Log("", query + "\n");
}

void SQLLogicTestLogger::PrintSQLFormatted() {
	std::cerr << termcolor::bold << "SQL Query" << termcolor::reset << std::endl;
	auto tokens = Parser::Tokenize(sql_query);
	for (idx_t i = 0; i < tokens.size(); i++) {
		auto &token = tokens[i];
		idx_t next = i + 1 < tokens.size() ? tokens[i + 1].start : sql_query.size();
		// adjust the highlighting based on the type
		switch (token.type) {
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER:
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR:
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS:
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_SUGGESTION:
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT:
			std::cerr << termcolor::yellow;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR:
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
			std::cerr << termcolor::green << termcolor::bold;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
			std::cerr << termcolor::grey;
			break;
		}
		// print the current token
		std::cerr << sql_query.substr(token.start, next - token.start);
		// reset and move to the next token
		std::cerr << termcolor::reset;
	}
	std::cerr << std::endl;
}

void SQLLogicTestLogger::PrintErrorHeader(const string &file_name, idx_t query_line, const string &description) {
	std::ostringstream oss;
	PrintSummaryHeader(file_name, query_line);
	oss << termcolor::red << termcolor::bold << description << " " << termcolor::reset;
	if (!file_name.empty()) {
		oss << termcolor::bold << "(" << file_name << ":" << query_line << ")!" << termcolor::reset;
	}
	LogFailureAnnotation(oss.str() + "\n");
}

void SQLLogicTestLogger::PrintErrorHeader(const string &description) {
	PrintErrorHeader(file_name, query_line, description);
}

void SQLLogicTestLogger::PrintResultError(const vector<string> &result_values, const vector<string> &values,
                                          idx_t expected_column_count, bool row_wise) {
	PrintHeader("Expected result:");
	PrintLineSep();
	PrintExpectedResult(values, expected_column_count, row_wise);
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintExpectedResult(result_values, expected_column_count, false);
}

string SQLLogicTestLogger::ResultToString(QueryResult &result) {
	if (result.RowCount() < 100) {
		return result.ToString();
	}
	BoxRendererConfig config;
	config.max_rows = 100;
	config.max_width = -1;
	ClientBoxRendererContext render_context(*connection.context);
	return result.ToBox(render_context, config);
}

void SQLLogicTestLogger::PrintResultString(QueryResult &result) {
	LogFailure(ResultToString(result));
}

void SQLLogicTestLogger::PrintResultError(QueryResult &result, const vector<string> &values,
                                          idx_t expected_column_count, bool row_wise) {
	PrintHeader("Expected result:");
	PrintLineSep();
	PrintExpectedResult(values, expected_column_count, row_wise);
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintResultString(result);
}

void SQLLogicTestLogger::UnexpectedFailure(QueryResult &result) {
	std::ostringstream oss;
	PrintErrorHeader("Query unexpectedly failed (" + file_name + ":" + to_string(query_line) + ")\n");
	LogFailure(oss.str());
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintResultString(result);
}
void SQLLogicTestLogger::OutputResult(QueryResult &result, const vector<string> &result_values_string) {
	// names
	for (idx_t c = 0; c < result.ColumnCount(); c++) {
		if (c != 0) {
			LogFailure("\t");
		}
		LogFailure(result.ColumnName(c).GetIdentifierName());
	}
	LogFailure("\n");
	// types
	for (idx_t c = 0; c < result.ColumnCount(); c++) {
		if (c != 0) {
			LogFailure("\t");
		}
		LogFailure(result.GetTypes()[c].ToString());
	}
	LogFailure("\n");
	PrintLineSep();
	for (idx_t r = 0; r < result.RowCount(); r++) {
		for (idx_t c = 0; c < result.ColumnCount(); c++) {
			if (c != 0) {
				LogFailure("\t");
			}
			LogFailure(result_values_string[r * result.ColumnCount() + c]);
		}
		LogFailure("\n");
	}
}

void SQLLogicTestLogger::OutputHash(const string &hash_value) {
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	LogFailure(hash_value + "\n");
	PrintLineSep();
}

void SQLLogicTestLogger::ColumnCountMismatch(QueryResult &result, const vector<string> &result_values_string,
                                             idx_t expected_column_count, bool row_wise) {
	std::ostringstream oss;
	PrintErrorHeader("Wrong column count in query!");
	oss << "Expected " << termcolor::bold << expected_column_count << termcolor::reset << " columns, but got "
	    << termcolor::bold << result.ColumnCount() << termcolor::reset << " columns" << std::endl;
	LogFailure(oss.str());
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	PrintResultError(result, result_values_string, expected_column_count, row_wise);
}

void SQLLogicTestLogger::NotCleanlyDivisible(idx_t expected_column_count, idx_t actual_column_count) {
	PrintLineSep();
	PrintErrorHeader("Error in test!");
	PrintLineSep();
	LogFailure("Expected " + to_string(expected_column_count) + " columns, but " + to_string(actual_column_count) +
	           " values were supplied\nThis is not cleanly divisible (i.e. the last row does not have enough values)");
}

void SQLLogicTestLogger::WrongRowCount(idx_t expected_rows, QueryResult &result,
                                       const vector<string> &comparison_values, idx_t expected_column_count,
                                       bool row_wise) {
	std::ostringstream oss;
	PrintErrorHeader("Wrong row count in query!");
	oss << "Expected " << termcolor::bold << expected_rows << termcolor::reset << " rows, but got " << termcolor::bold
	    << result.RowCount() << termcolor::reset << " rows" << std::endl;
	LogFailure(oss.str());
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	PrintResultError(result, comparison_values, expected_column_count, row_wise);
}

void SQLLogicTestLogger::ColumnCountMismatchCorrectResult(idx_t original_expected_columns, idx_t expected_column_count,
                                                          QueryResult &result) {
	std::ostringstream oss;
	PrintErrorHeader("Wrong column count in query!");
	oss << "Expected " << termcolor::bold << original_expected_columns << termcolor::reset << " columns, but got "
	    << termcolor::bold << expected_column_count << termcolor::reset << " columns" << std::endl;
	LogFailure(oss.str());
	oss.str("");
	oss.clear();
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	oss << "The expected result " << termcolor::bold << "matched" << termcolor::reset << " the query result."
	    << std::endl;
	LogFailure(oss.str());
	oss.str("");
	oss.clear();
	oss << termcolor::bold << "Suggested fix: modify header to \"" << termcolor::green << "query "
	    << string(result.ColumnCount(), 'I') << termcolor::reset << termcolor::bold << "\"" << termcolor::reset
	    << std::endl;
	LogFailure(oss.str());
	PrintLineSep();
}

void SQLLogicTestLogger::SplitMismatch(idx_t row_number, idx_t expected_column_count, idx_t split_count) {
	std::ostringstream oss;
	PrintLineSep();
	PrintErrorHeader("Error in test! Column count mismatch after splitting on tab on row " + to_string(row_number) +
	                 "!");
	oss << "Expected " << termcolor::bold << expected_column_count << termcolor::reset << " columns, but got "
	    << termcolor::bold << split_count << termcolor::reset << " columns" << std::endl;
	LogFailure(oss.str());
	LogFailure("Does the result contain tab values? In that case, place every value on a single row.\n");
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
}

void SQLLogicTestLogger::WrongResultHash(const string &expected_result, QueryResult &result,
                                         const string &expected_hash, const string &actual_hash) {
	PrintErrorHeader("Wrong result hash!");
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	PrintHeader("Expected result:");
	if (!expected_result.empty()) {
		LogFailure(expected_result);
	} else {
		LogFailure("???\n");
	}
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintResultString(result);
}

void SQLLogicTestLogger::UnexpectedStatement(bool expect_ok, QueryResult &result) {
	PrintErrorHeader(!expect_ok ? "Query unexpectedly succeeded!" : "Query unexpectedly failed!");
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	PrintResultString(result);
}

void SQLLogicTestLogger::ExpectedErrorMismatch(const string &expected_error, QueryResult &result) {
	PrintErrorHeader("Query failed, but error message did not match expected error message: " + expected_error);
	PrintLineSep();
	PrintSQL();
	PrintLineSep();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintResultString(result);
}

void SQLLogicTestLogger::InternalException(QueryResult &result) {
	PrintErrorHeader("Query failed with internal exception!");
	PrintLineSep();
	PrintSQL();
	PrintHeader("Actual result:");
	PrintLineSep();
	PrintResultString(result);
}

void SQLLogicTestLogger::LoadDatabaseFail(const string &file_name, const string &dbpath, const string &message) {
	PrintErrorHeader(file_name, 0, "Failed to load database " + dbpath);
	PrintLineSep();
	LogFailure("Error message: " + message + "\n");
	PrintLineSep();
}

} // namespace duckdb
