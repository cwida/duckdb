#include "result_helper.hpp"

#include "duckdb/common/crypto/md5.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "re2/re2.h"
#include "sqllogic_test_logger.hpp"
#include "sqllogic_test_runner.hpp"
#include "termcolor.hpp"
#include "test_helpers.hpp"
#include "test_config.hpp"
#include "test_reporter.hpp"

#include <thread>

namespace duckdb {

void TestResultHelper::SortQueryResult(SortStyle sort_style, vector<string> &result, idx_t ncols) {
	if (sort_style == SortStyle::NO_SORT) {
		return;
	}
	if (sort_style == SortStyle::VALUE_SORT) {
		// sort values independently
		std::sort(result.begin(), result.end());
		return;
	}
	if (result.size() % ncols != 0) {
		// row-sort failed: result is not row-wise aligned, bail
		TEST_FAIL(
		    StringUtil::Format("Failed to sort query result - result is not aligned. Found %d rows with %d columns",
		                       result.size(), ncols));
		return;
	}
	// row-oriented sorting
	idx_t nrows = result.size() / ncols;
	vector<vector<string>> rows;
	rows.reserve(nrows);
	for (idx_t row_idx = 0; row_idx < nrows; row_idx++) {
		vector<string> row;
		row.reserve(ncols);
		for (idx_t col_idx = 0; col_idx < ncols; col_idx++) {
			row.push_back(std::move(result[row_idx * ncols + col_idx]));
		}
		rows.push_back(std::move(row));
	}
	// sort the individual rows
	std::sort(rows.begin(), rows.end(), [](const vector<string> &a, const vector<string> &b) {
		for (idx_t col_idx = 0; col_idx < a.size(); col_idx++) {
			if (a[col_idx] != b[col_idx]) {
				return a[col_idx] < b[col_idx];
			}
		}
		return false;
	});

	// now reconstruct the values from the rows
	for (idx_t row_idx = 0; row_idx < nrows; row_idx++) {
		for (idx_t col_idx = 0; col_idx < ncols; col_idx++) {
			result[row_idx * ncols + col_idx] = std::move(rows[row_idx][col_idx]);
		}
	}
}

bool TestResultHelper::CheckQueryResult(const Query &query, ExecuteContext &context,
                                        duckdb::unique_ptr<QueryResult> owned_result) {
	auto &result = *owned_result;
	auto &runner = query.runner;
	auto expected_column_count = query.expected_column_count;
	auto &values = query.values;
	auto sort_style = query.sort_style;
	auto query_has_label = query.query_has_label;
	auto &query_label = query.query_label;

	SQLLogicTestLogger logger(context, query);
	if (result.HasError()) {
		if (SkipErrorMessage(result.GetError())) {
			runner.finished_processing_file = true;
			return true;
		}
		runner.last_error_message = result.GetError();
		if (!FailureSummary::SkipLoggingSameError(context.error_file)) {
			logger.UnexpectedFailure(result);
		}
		return false;
	}
	idx_t row_count = result.RowCount();
	idx_t column_count = result.ColumnCount();
	idx_t total_value_count = row_count * column_count;
	bool compare_hash =
	    query_has_label || (runner.hash_threshold > 0 && total_value_count > idx_t(runner.hash_threshold));
	bool result_is_hash = false;
	// check if the current line (the first line of the result) is a hash value
	if (values.size() == 1 && ResultIsHash(values[0])) {
		compare_hash = true;
		result_is_hash = true;
	}

	vector<string> result_values_string;
	try {
		DuckDBConvertResult(result, runner.original_sqlite_test, result_values_string);
		if (runner.output_result_mode) {
			logger.OutputResult(result, result_values_string);
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		auto &original_error = error.Message();
		logger.LogFailure(original_error);
		return false;
	}

	SortQueryResult(sort_style, result_values_string, column_count);

	vector<string> comparison_values;
	if (values.size() == 1 && ResultIsFile(values[0])) {
		auto fname = StringUtil::Replace(values[0], "<FILE>:", "");
		fname = runner.ReplaceKeywords(fname);
		fname = runner.LoopReplacement(fname, context.running_loops);
		string csv_error;
		comparison_values =
		    LoadResultFromFile(fname, IdentifiersToStrings(result.GetNames()), expected_column_count, csv_error);
		if (!csv_error.empty()) {
			string log_message;
			logger.PrintErrorHeader(csv_error);

			return false;
		}
	} else {
		comparison_values = values;
	}

	// compute the hash of the results if there is a hash label or we are past the hash threshold
	string hash_value;
	if (runner.output_hash_mode || compare_hash) {
		MD5Context context;
		for (idx_t i = 0; i < total_value_count; i++) {
			context.Add(result_values_string[i]);
			context.Add("\n");
		}
		string digest = context.FinishHex();
		hash_value = to_string(total_value_count) + " values hashing to " + digest;
		if (runner.output_hash_mode) {
			logger.OutputHash(hash_value);
			return true;
		}
	}

	if (!compare_hash) {
		// check if the row/column count matches
		idx_t original_expected_columns = expected_column_count;
		bool column_count_mismatch = false;
		if (expected_column_count != result.ColumnCount()) {
			// expected column count is different from the count found in the result
			// we try to keep going with the number of columns in the result
			expected_column_count = result.ColumnCount();
			column_count_mismatch = true;
		}
		if (expected_column_count == 0) {
			return false;
		}
		idx_t expected_rows = comparison_values.size() / expected_column_count;
		// we first check the counts: if the values are equal to the amount of rows we expect the results to be row-wise
		bool row_wise = expected_column_count > 1 && comparison_values.size() == result.RowCount();
		if (!row_wise) {
			// the counts do not match up for it to be row-wise
			// however, this can also be because the query returned an incorrect # of rows
			// we make a guess: if everything contains tabs, we still treat the input as row wise
			bool all_tabs = true;
			for (auto &val : comparison_values) {
				if (val.find('\t') == string::npos) {
					all_tabs = false;
					break;
				}
			}
			row_wise = all_tabs;
		}
		if (row_wise) {
			// values are displayed row-wise, format row wise with a tab
			expected_rows = comparison_values.size();
			row_wise = true;
		} else if (comparison_values.size() % expected_column_count != 0) {
			if (column_count_mismatch) {
				logger.ColumnCountMismatch(result, query.values, original_expected_columns, row_wise);
			} else {
				logger.NotCleanlyDivisible(expected_column_count, comparison_values.size());
			}
			return false;
		}
		if (expected_rows != result.RowCount()) {
			if (column_count_mismatch) {
				logger.ColumnCountMismatch(result, query.values, original_expected_columns, row_wise);
			} else {
				logger.WrongRowCount(expected_rows, result, comparison_values, expected_column_count, row_wise);
			}
			return false;
		}

		if (row_wise) {
			// if the result is row-wise, turn it into a set of values by splitting it
			vector<string> expected_values;
			for (idx_t i = 0; i < total_value_count && i < comparison_values.size(); i++) {
				// split based on tab character
				auto splits = StringUtil::Split(comparison_values[i], "\t");
				if (splits.size() != expected_column_count) {
					if (column_count_mismatch) {
						logger.ColumnCountMismatch(result, query.values, original_expected_columns, row_wise);
					}
					logger.SplitMismatch(i + 1, expected_column_count, splits.size());
					return false;
				}
				for (auto &split : splits) {
					expected_values.push_back(std::move(split));
				}
			}
			comparison_values = std::move(expected_values);
			row_wise = false;
		}
		auto &test_config = TestConfiguration::Get();
		auto default_sort_style = test_config.GetDefaultSortStyle();
		idx_t check_it_count = column_count_mismatch || default_sort_style == SortStyle::NO_SORT ? 1 : 2;
		for (idx_t check_it = 0; check_it < check_it_count; check_it++) {
			bool final_iteration = check_it + 1 == check_it_count;
			idx_t current_row = 0, current_column = 0;
			bool success = true;
			for (idx_t i = 0; i < total_value_count && i < comparison_values.size(); i++) {
				success = CompareValues(logger, result,
				                        result_values_string[current_row * expected_column_count + current_column],
				                        comparison_values[i], current_row, current_column, comparison_values,
				                        expected_column_count, row_wise, result_values_string, final_iteration);
				if (!success) {
					break;
				}
				TEST_ASSERTION();

				current_column++;
				if (current_column == expected_column_count) {
					current_row++;
					current_column = 0;
				}
			}
			if (!success) {
				if (final_iteration) {
					return false;
				}
				SortQueryResult(default_sort_style, result_values_string, column_count);
				SortQueryResult(default_sort_style, comparison_values, query.expected_column_count);
			}
		}
		if (column_count_mismatch) {
			logger.ColumnCountMismatchCorrectResult(original_expected_columns, expected_column_count, result);
			return false;
		}
	} else {
		bool hash_compare_error = false;
		if (query_has_label) {
			runner.hash_label_map.WithLock([&](unordered_map<string, CachedLabelData> &map) {
				// the query has a label: check if the hash has already been computed
				auto entry = map.find(query_label);
				if (entry == map.end()) {
					// not computed yet: add it tot he map
					map.emplace(query_label, CachedLabelData(hash_value, logger.ResultToString(*owned_result)));
				} else {
					hash_compare_error = entry->second.hash != hash_value;
				}
			});
		}
		string expected_hash;
		if (result_is_hash) {
			expected_hash = values[0];
			D_ASSERT(values.size() == 1);
			hash_compare_error = expected_hash != hash_value;
		}
		if (hash_compare_error) {
			string expected_result;
			runner.hash_label_map.WithLock([&](unordered_map<string, CachedLabelData> &map) {
				auto it = map.find(query_label);
				if (it != map.end()) {
					expected_result = it->second.result_str;
				}
				logger.WrongResultHash(expected_result, result, expected_hash, hash_value);
			});
			return false;
		}
		TEST_REQUIRE(!hash_compare_error);
	}
	return true;
}

static bool IsRegexComparison(const string &expected) {
	return StringUtil::StartsWith(expected, "<REGEX>:") || StringUtil::StartsWith(expected, "<!REGEX>:");
}

bool TestResultHelper::ErrorMatchesExpected(SQLLogicTestLogger &logger, const string &expected,
                                            const string &actual) const {
	// We run both comparisons on purpose, we might move to only the second but might require some changes in tests
	// This is due to some errors containing absolute paths, some relatives
	if (StringUtil::Contains(actual, expected) || StringUtil::Contains(actual, runner.ReplaceKeywords(expected))) {
		return true;
	}

	if (!IsRegexComparison(expected)) {
		return false;
	}
	//! NOTE: mimicks 'QueryResult::ToString' behavior if 'error' is set
	auto error_message = actual + "\n";
	return MatchesRegex(logger, error_message, expected);
}

optional<string> TestResultHelper::EvaluateStatementResult(SQLLogicTestLogger &logger, const Statement &statement,
                                                           ExecuteContext &context,
                                                           const optional<string> &error) const {
	//! Check to see if we are expecting success or failure
	auto expected_result = statement.expected_result;
	switch (expected_result) {
	case ExpectedResult::RESULT_SUCCESS: {
		//! If there's an error, it's always unexpected.
		return error;
	}
	case ExpectedResult::RESULT_UNKNOWN:
	case ExpectedResult::RESULT_ERROR: {
		const bool success_is_not_an_error = expected_result == ExpectedResult::RESULT_UNKNOWN;
		if (!error) {
			if (success_is_not_an_error) {
				//! OK is not unexpected
				return std::nullopt;
			}
			//! Expected an error, didn't get an error - always unexpected
			//! NOTE: Signal unexpected result, without performing extra logging
			return "";
		}
		const auto &expected = statement.expected_error;
		const auto &actual = *error;
		if (expected.empty() || ErrorMatchesExpected(logger, expected, actual)) {
			//! Either no explicit expectation was set, or the error matches - not unexpected
			return std::nullopt;
		}
		return error;
	}
	default: {
		//! The result is never unexpected, we accept all results (apart from the special cases handled above)
		return std::nullopt;
	}
	}
}

bool TestResultHelper::CheckStatementResult(const Statement &statement, ExecuteContext &context,
                                            duckdb::unique_ptr<QueryResult> owned_result) {
	auto &result = *owned_result;
	SQLLogicTestLogger logger(context, statement);
	if (runner.output_result_mode || runner.debug_mode) {
		result.Print();
	}

	optional<string> error;
	if (result.HasError()) {
		error = result.GetError();
	}

	if (error && TestIsInternalError(runner.always_fail_error_messages, *error)) {
		//! Encountered an internal exception, regardless of what statement type, this is unexpected
		logger.InternalException(result);
		return false;
	}

	auto unexpected_result = EvaluateStatementResult(logger, statement, context, error);
	if (!unexpected_result) {
		TEST_ASSERTION();
		return true;
	}

	//! Statement ended in an unexpected result, deal with it
	auto expected_result = statement.expected_result;

	switch (expected_result) {
	case ExpectedResult::RESULT_SUCCESS: {
		if (SkipErrorMessage(*unexpected_result)) {
			//! File is skipped as a result of the encountered error message
			runner.finished_processing_file = true;
			return true;
		}
		runner.last_error_message = *unexpected_result;
		if (!FailureSummary::SkipLoggingSameError(statement.file_name)) {
			logger.UnexpectedStatement(true, result);
		}
		return false;
	}
	case ExpectedResult::RESULT_UNKNOWN:
	case ExpectedResult::RESULT_ERROR: {
		auto &error_message = *unexpected_result;
		//! NOTE: We use the empty string to indicate the statement ended in success unexpectedly
		const bool no_error_result = error_message.empty();
		if (expected_result == ExpectedResult::RESULT_UNKNOWN && no_error_result) {
			throw InternalException("OK (no error) is never unexpected for 'statement maybe'");
		}
		if (!no_error_result && SkipErrorMessage(error_message)) {
			//! File is skipped as a result of the encountered error message
			runner.finished_processing_file = true;
			return true;
		}
		if (!FailureSummary::SkipLoggingSameError(statement.file_name)) {
			if (no_error_result) {
				//! Expected an error but the statement succeeded!
				logger.UnexpectedStatement(false, result);
			} else {
				//! Received an error but it didn't match
				const auto &expected = statement.expected_error;
				logger.ExpectedErrorMismatch(expected, result);
			}
		}
		return false;
	}
	default: {
		//! Other types will never result in an unexpected result, because they accept anything
		throw InternalException("Unexpected ExpectedResult type encountered: %d",
		                        static_cast<uint8_t>(expected_result));
	}
	};
}

vector<string> TestResultHelper::LoadResultFromFile(string fname, vector<string> names, idx_t &expected_column_count,
                                                    string &error) {
	DuckDB db(nullptr);
	Connection con(db);
	auto threads = MaxValue<idx_t>(std::thread::hardware_concurrency(), 1);
	con.Query("PRAGMA threads=" + to_string(threads));

	string struct_definition = "STRUCT_PACK(";
	for (idx_t i = 0; i < names.size(); i++) {
		if (i > 0) {
			struct_definition += ", ";
		}
		struct_definition += StringUtil::Format("%s := VARCHAR", SQLIdentifier(names[i]));
	}
	struct_definition += ")";

	auto csv_result = con.Query("SELECT * FROM read_csv('" + fname +
	                            "', header=1, sep='|', columns=" + struct_definition + ", auto_detect=false)");
	if (csv_result->HasError()) {
		error = StringUtil::Format("Could not read CSV File \"%s\": %s", fname, csv_result->GetError());
		return vector<string>();
	}
	expected_column_count = csv_result->ColumnCount();

	vector<string> values;
	while (true) {
		auto chunk = csv_result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t r = 0; r < chunk->size(); r++) {
			for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
				values.push_back(chunk->GetValue(c, r).CastAs(*runner.con->context, LogicalType::VARCHAR).ToString());
			}
		}
	}
	return values;
}

bool TestResultHelper::SkipErrorMessage(const string &message) {
	for (auto &error_message : runner.ignore_error_messages) {
		if (StringUtil::Contains(message, error_message)) {
			SQLLogicTestLogger::ReportSkip(runner.file_name, "skip on error_message matching '" + error_message + "'");
			return true;
		}
	}
	return false;
}

string TestResultHelper::SQLLogicTestConvertValue(Value value, LogicalType sql_type, bool original_sqlite_test) {
	if (value.IsNull()) {
		return "NULL";
	} else {
		if (original_sqlite_test) {
			// sqlite test hashes want us to convert floating point numbers to integers
			switch (sql_type.id()) {
			case LogicalTypeId::DECIMAL:
			case LogicalTypeId::FLOAT:
			case LogicalTypeId::DOUBLE:
				return value.CastAs(*runner.con->context, LogicalType::BIGINT).ToString();
			default:
				break;
			}
		}
		switch (sql_type.id()) {
		case LogicalTypeId::BOOLEAN:
			return BooleanValue::Get(value) ? "1" : "0";
		default: {
			string str = value.CastAs(*runner.con->context, LogicalType::VARCHAR).ToString();
			if (str.empty()) {
				return "(empty)";
			} else {
				return StringUtil::Replace(str, string("\0", 1), "\\0");
			}
		}
		}
	}
}

// standard result conversion: one line per value
void TestResultHelper::DuckDBConvertResult(QueryResult &result, bool original_sqlite_test, vector<string> &out_result) {
	size_t r, c;
	idx_t row_count = result.RowCount();
	idx_t column_count = result.ColumnCount();

	out_result.resize(row_count * column_count);
	auto rows = result.Collection().GetRows();
	for (r = 0; r < row_count; r++) {
		for (c = 0; c < column_count; c++) {
			auto value = rows.GetValue(c, r);
			auto converted_value = SQLLogicTestConvertValue(value, result.GetTypes()[c], original_sqlite_test);
			out_result[r * column_count + c] = converted_value;
		}
	}
}

bool TestResultHelper::ResultIsHash(const string &result) {
	idx_t pos = 0;
	// first parse the rows
	while (result[pos] >= '0' && result[pos] <= '9') {
		pos++;
	}
	if (pos == 0) {
		return false;
	}
	string constant_str = " values hashing to ";
	string example_hash = "acd848208cc35c7324ece9fcdd507823";
	if (pos + constant_str.size() + example_hash.size() != result.size()) {
		return false;
	}
	if (result.substr(pos, constant_str.size()) != constant_str) {
		return false;
	}
	pos += constant_str.size();
	// now parse the hash
	while ((result[pos] >= '0' && result[pos] <= '9') || (result[pos] >= 'a' && result[pos] <= 'z')) {
		pos++;
	}
	return pos == result.size();
}

bool TestResultHelper::ResultIsFile(string result) {
	return StringUtil::StartsWith(result, "<FILE>:");
}

bool TestResultHelper::CompareValues(SQLLogicTestLogger &logger, QueryResult &result, string lvalue_str,
                                     string rvalue_str, idx_t current_row, idx_t current_column, vector<string> &values,
                                     idx_t expected_column_count, bool row_wise, vector<string> &result_values,
                                     bool print_error) {
	Value lvalue, rvalue;
	bool error = false;
	// simple first test: compare string value directly
	// We run both comparions on purpose, we might move to only the second but might require some changes in tests
	// This is due to some results containing absolute paths, some relatives
	if (lvalue_str == rvalue_str || lvalue_str == runner.ReplaceKeywords(rvalue_str)) {
		return true;
	}
	if (StringUtil::StartsWith(rvalue_str, "<REGEX>:") || StringUtil::StartsWith(rvalue_str, "<!REGEX>:")) {
		if (MatchesRegex(logger, lvalue_str, rvalue_str)) {
			return true;
		}
	}
	// some times require more checking (specifically floating point numbers because of inaccuracies)
	// if not equivalent we need to cast to the SQL type to verify
	auto sql_type = result.GetTypes()[current_column];
	if (sql_type.IsNumeric()) {
		bool converted_lvalue = false;
		bool converted_rvalue = false;
		if (lvalue_str == "NULL") {
			lvalue = Value(sql_type);
			converted_lvalue = true;
		} else {
			auto cast_lvalue = Value(lvalue_str).TryCastAs(*runner.con->context, sql_type);
			if (cast_lvalue) {
				lvalue = std::move(*cast_lvalue);
				converted_lvalue = true;
			} else {
				lvalue = Value(lvalue_str);
			}
		}
		if (rvalue_str == "NULL") {
			rvalue = Value(sql_type);
			converted_rvalue = true;
		} else {
			auto cast_rvalue = Value(rvalue_str).TryCastAs(*runner.con->context, sql_type);
			if (cast_rvalue) {
				rvalue = std::move(*cast_rvalue);
				converted_rvalue = true;
			} else {
				rvalue = Value(rvalue_str);
			}
		}
		if (converted_lvalue && converted_rvalue) {
			error = !Value::ValuesAreEqual(*runner.con->context, lvalue, rvalue);
		} else {
			error = true;
		}
	} else if (sql_type == LogicalType::BOOLEAN) {
		auto low_r_val = StringUtil::Lower(rvalue_str);
		auto low_l_val = StringUtil::Lower(lvalue_str);

		string true_str = "true";
		string false_str = "false";
		if (low_l_val == true_str || lvalue_str == "1") {
			lvalue = Value(1);
		} else if (low_l_val == false_str || lvalue_str == "0") {
			lvalue = Value(0);
		}
		if (low_r_val == true_str || rvalue_str == "1") {
			rvalue = Value(1);
		} else if (low_r_val == false_str || rvalue_str == "0") {
			rvalue = Value(0);
		}
		error = !Value::ValuesAreEqual(*runner.con->context, lvalue, rvalue);

	} else {
		// for other types we just mark the result as incorrect
		error = true;
	}
	if (error) {
		if (print_error) {
			std::ostringstream oss;
			logger.PrintErrorHeader("Wrong result in query!");
			logger.PrintLineSep();
			logger.PrintSQL();
			logger.PrintLineSep();
			oss << termcolor::red << termcolor::bold << "Mismatch on row " << current_row + 1 << ", column "
			    << result.ColumnName(current_column) << "(index " << current_column + 1 << ")" << std::endl
			    << termcolor::reset;
			oss << lvalue_str << " <> " << rvalue_str << std::endl;
			logger.LogFailure(oss.str());
			logger.PrintLineSep();
			logger.PrintResultError(result_values, values, expected_column_count, row_wise);
		}
		return false;
	}
	return true;
}

bool TestResultHelper::MatchesRegex(SQLLogicTestLogger &logger, string lvalue_str, string rvalue_str) const {
	bool want_match = StringUtil::StartsWith(rvalue_str, "<REGEX>:");
	string regex_str = StringUtil::Replace(StringUtil::Replace(rvalue_str, "<REGEX>:", ""), "<!REGEX>:", "");
	RE2::Options options;
	options.set_dot_nl(true);
	RE2 re(regex_str, options);
	if (!re.ok()) {
		std::ostringstream oss;
		logger.PrintErrorHeader("Test error!");
		logger.PrintLineSep();
		oss << termcolor::red << termcolor::bold << "Failed to parse regex: " << re.error() << termcolor::reset
		    << std::endl;
		logger.LogFailure(oss.str());
		logger.PrintLineSep();
		return false;
	}
	bool regex_matches = RE2::FullMatch(lvalue_str, re);
	if ((want_match && regex_matches) || (!want_match && !regex_matches)) {
		return true;
	}
	return false;
}

} // namespace duckdb
