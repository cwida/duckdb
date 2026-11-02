#include "sqllogic_parser.hpp"
#include "test_reporter.hpp"

#include <fstream>

namespace duckdb {

bool SQLLogicParser::OpenFile(const string &path) {
	auto infile = make_uniq<std::ifstream>(path);
	if (infile->bad() || infile->fail()) {
		return false;
	}
	owned_stream = std::move(infile);
	return OpenStream(*owned_stream, path);
}

bool SQLLogicParser::OpenStream(std::istream &input, const string &source_name) {
	file_name = source_name;
	stream = &input;
	stream_finished = false;
	current_line = 0;
	line_start = 0;
	lines.clear();
	seen_statement = false;
	current_include.reset();
	return !stream->bad() && !stream->fail();
}

void SQLLogicParser::IncludeFile(const string &file_name) {
	auto include_parser = make_uniq<SQLLogicParser>();
	auto success = include_parser->OpenFile(file_name);
	if (!success) {
		Fail("Failed to open include file %s", file_name);
	}
	current_include = std::move(include_parser);
}

bool SQLLogicParser::EmptyOrComment(const string &line) {
	return line.empty() || StringUtil::StartsWith(line, "#");
}

bool SQLLogicParser::HasLine(idx_t line_idx) {
	if (line_idx < line_start) {
		return false;
	}
	while (!stream_finished && line_idx >= line_start + lines.size()) {
		if (!stream || !stream->good()) {
			stream_finished = true;
			break;
		}
		string line;
		if (!std::getline(*stream, line)) {
			stream_finished = true;
			break;
		}
		lines.push_back(StringUtil::Replace(line, "\r", ""));
	}
	return line_idx >= line_start && line_idx < line_start + lines.size();
}

string &SQLLogicParser::GetLine(idx_t line_idx) {
	if (!HasLine(line_idx)) {
		Fail("Unexpected end of file");
	}
	return lines[line_idx - line_start];
}

void SQLLogicParser::PruneLines() {
	if (current_line <= line_start || lines.empty()) {
		return;
	}
	auto prune_count = MinValue<idx_t>(current_line - line_start, lines.size());
	lines.erase(lines.begin(), lines.begin() + prune_count);
	line_start += prune_count;
}

bool SQLLogicParser::NextLineEmptyOrComment() {
	if (current_include) {
		return current_include->NextLineEmptyOrComment();
	}
	if (!HasLine(current_line + 1)) {
		return true;
	} else {
		return EmptyOrComment(GetLine(current_line + 1));
	}
}

bool SQLLogicParser::NextStatement() {
	if (current_include) {
		auto result = current_include->NextStatement();
		if (result) {
			return true;
		}
		// finished processing this include - continue processing the main file
		current_include.reset();
	}
	if (seen_statement) {
		// skip the current statement
		// but only if we have already seen a statement in the file
		while (HasLine(current_line) && !EmptyOrComment(GetLine(current_line))) {
			current_line++;
		}
	}
	seen_statement = true;
	// now look for the first non-empty line
	while (HasLine(current_line) && EmptyOrComment(GetLine(current_line))) {
		current_line++;
	}
	PruneLines();
	// return whether or not we reached the end of the file
	return HasLine(current_line);
}

void SQLLogicParser::NextLine() {
	if (current_include) {
		current_include->NextLine();
		return;
	}
	current_line++;
}

string SQLLogicParser::ExtractStatement() {
	if (current_include) {
		return current_include->ExtractStatement();
	}
	string statement;

	bool first_line = true;
	while (HasLine(current_line) && !EmptyOrComment(GetLine(current_line))) {
		if (GetLine(current_line) == "----") {
			break;
		}
		if (!first_line) {
			statement += "\n";
		}
		statement += GetLine(current_line);
		first_line = false;

		current_line++;
	}

	return statement;
}

vector<string> SQLLogicParser::ExtractExpectedResult() {
	if (current_include) {
		return current_include->ExtractExpectedResult();
	}
	vector<string> result;
	// skip the result line (----) if we are still reading that
	if (HasLine(current_line) && GetLine(current_line) == "----") {
		current_line++;
	}
	// read the expected result until we encounter a new line
	while (HasLine(current_line) && !GetLine(current_line).empty()) {
		result.push_back(GetLine(current_line));
		current_line++;
	}
	return result;
}

string SQLLogicParser::ExtractExpectedError(ExpectedResult expected_result, bool original_sqlite_test) {
	if (current_include) {
		return current_include->ExtractExpectedError(expected_result, original_sqlite_test);
	}
	bool expect_error_message =
	    expected_result == ExpectedResult::RESULT_ERROR || expected_result == ExpectedResult::RESULT_UNKNOWN;

	// check if there is an expected error at all
	if (!HasLine(current_line) || GetLine(current_line) != "----") {
		if (expect_error_message && !original_sqlite_test) {
			Fail("Failed to parse statement: statement error and maybe needs to have an expected error message");
		}
		return string();
	}
	if (!expect_error_message) {
		Fail("Failed to parse statement: only statement error or maybe can have an expected error message, not "
		     "statement ok");
	}
	current_line++;
	string error;
	vector<string> error_lines;
	while (HasLine(current_line) && !GetLine(current_line).empty()) {
		error_lines.push_back(GetLine(current_line));
		current_line++;
	}
	error = StringUtil::Join(error_lines, "\n");
	return error;
}

void SQLLogicParser::FailRecursive(const string &msg, vector<ExceptionFormatValue> &values) {
	auto error_message =
	    file_name + ":" + to_string(current_line + 1) + ": " + ExceptionFormatValue::Format(msg, values);
	TEST_FAIL(error_message);
}

SQLLogicToken SQLLogicParser::Tokenize() {
	if (current_include) {
		return current_include->Tokenize();
	}
	SQLLogicToken result;
	if (!HasLine(current_line)) {
		result.type = SQLLogicTokenType::SQLLOGIC_INVALID;
		return result;
	}

	vector<string> argument_list;
	auto &line = GetLine(current_line);
	idx_t last_pos = 0;
	for (idx_t i = 0; i < line.size(); i++) {
		if (StringUtil::CharacterIsSpace(line[i])) {
			if (i == last_pos) {
				last_pos++;
			} else {
				argument_list.push_back(line.substr(last_pos, i - last_pos));
				last_pos = i + 1;
			}
		}
	}
	if (last_pos != line.size()) {
		argument_list.push_back(line.substr(last_pos, line.size() - last_pos));
	}
	if (argument_list.empty()) {
		Fail("Empty line!?");
	}
	result.type = CommandToToken(argument_list[0]);
	for (idx_t i = 1; i < argument_list.size(); i++) {
		result.parameters.push_back(std::move(argument_list[i]));
	}
	return result;
}

// Single line statements should throw a parser error if the next line is not a comment or a newline
bool SQLLogicParser::IsSingleLineStatement(SQLLogicToken &token) {
	switch (token.type) {
	case SQLLogicTokenType::SQLLOGIC_HASH_THRESHOLD:
	case SQLLogicTokenType::SQLLOGIC_HALT:
	case SQLLogicTokenType::SQLLOGIC_MODE:
	case SQLLogicTokenType::SQLLOGIC_SET:
	case SQLLogicTokenType::SQLLOGIC_RESET:
	case SQLLogicTokenType::SQLLOGIC_LOOP:
	case SQLLogicTokenType::SQLLOGIC_FOREACH:
	case SQLLogicTokenType::SQLLOGIC_CONCURRENT_LOOP:
	case SQLLogicTokenType::SQLLOGIC_CONCURRENT_FOREACH:
	case SQLLogicTokenType::SQLLOGIC_ENDLOOP:
	case SQLLogicTokenType::SQLLOGIC_REQUIRE:
	case SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV:
	case SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV_NOT:
	case SQLLogicTokenType::SQLLOGIC_TEST_ENV:
	case SQLLogicTokenType::SQLLOGIC_LOAD:
	case SQLLogicTokenType::SQLLOGIC_RESTART:
	case SQLLogicTokenType::SQLLOGIC_RECONNECT:
	case SQLLogicTokenType::SQLLOGIC_SLEEP:
	case SQLLogicTokenType::SQLLOGIC_UNZIP:
	case SQLLogicTokenType::SQLLOGIC_TAGS:
	case SQLLogicTokenType::SQLLOGIC_CONTINUE:
	case SQLLogicTokenType::SQLLOGIC_INCLUDE:
		return true;

	case SQLLogicTokenType::SQLLOGIC_SKIP_IF:
	case SQLLogicTokenType::SQLLOGIC_ONLY_IF:
	case SQLLogicTokenType::SQLLOGIC_INVALID:
	case SQLLogicTokenType::SQLLOGIC_STATEMENT:
	case SQLLogicTokenType::SQLLOGIC_QUERY:
		return false;

	default:
		throw std::runtime_error("Unknown SQLLogic token found!");
	}
}

// (All) Context statements must precede all non-header statements
bool SQLLogicParser::IsTestCommand(SQLLogicTokenType &type) {
	switch (type) {
	case SQLLogicTokenType::SQLLOGIC_QUERY:
	case SQLLogicTokenType::SQLLOGIC_STATEMENT:
		return true;

	case SQLLogicTokenType::SQLLOGIC_CONCURRENT_FOREACH:
	case SQLLogicTokenType::SQLLOGIC_CONCURRENT_LOOP:
	case SQLLogicTokenType::SQLLOGIC_ENDLOOP:
	case SQLLogicTokenType::SQLLOGIC_FOREACH:
	case SQLLogicTokenType::SQLLOGIC_HALT:
	case SQLLogicTokenType::SQLLOGIC_HASH_THRESHOLD:
	case SQLLogicTokenType::SQLLOGIC_INVALID:
	case SQLLogicTokenType::SQLLOGIC_LOAD:
	case SQLLogicTokenType::SQLLOGIC_LOOP:
	case SQLLogicTokenType::SQLLOGIC_MODE:
	case SQLLogicTokenType::SQLLOGIC_ONLY_IF:
	case SQLLogicTokenType::SQLLOGIC_RECONNECT:
	case SQLLogicTokenType::SQLLOGIC_REQUIRE:
	case SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV:
	case SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV_NOT:
	case SQLLogicTokenType::SQLLOGIC_RESET:
	case SQLLogicTokenType::SQLLOGIC_RESTART:
	case SQLLogicTokenType::SQLLOGIC_SET:
	case SQLLogicTokenType::SQLLOGIC_SKIP_IF:
	case SQLLogicTokenType::SQLLOGIC_SLEEP:
	case SQLLogicTokenType::SQLLOGIC_TAGS:
	case SQLLogicTokenType::SQLLOGIC_CONTINUE:
	case SQLLogicTokenType::SQLLOGIC_TEST_ENV:
	case SQLLogicTokenType::SQLLOGIC_UNZIP:
	case SQLLogicTokenType::SQLLOGIC_INCLUDE:
		return false;

	default:
		throw std::runtime_error("Unknown SQLLogic token found!");
	}
}

SQLLogicTokenType SQLLogicParser::CommandToToken(const string &token) {
	if (token == "skipif") {
		return SQLLogicTokenType::SQLLOGIC_SKIP_IF;
	} else if (token == "onlyif") {
		return SQLLogicTokenType::SQLLOGIC_ONLY_IF;
	} else if (token == "statement") {
		return SQLLogicTokenType::SQLLOGIC_STATEMENT;
	} else if (token == "query") {
		return SQLLogicTokenType::SQLLOGIC_QUERY;
	} else if (token == "hash-threshold") {
		return SQLLogicTokenType::SQLLOGIC_HASH_THRESHOLD;
	} else if (token == "halt") {
		return SQLLogicTokenType::SQLLOGIC_HALT;
	} else if (token == "mode") {
		return SQLLogicTokenType::SQLLOGIC_MODE;
	} else if (token == "set") {
		return SQLLogicTokenType::SQLLOGIC_SET;
	} else if (token == "reset") {
		return SQLLogicTokenType::SQLLOGIC_RESET;
	} else if (token == "loop") {
		return SQLLogicTokenType::SQLLOGIC_LOOP;
	} else if (token == "concurrentloop") {
		return SQLLogicTokenType::SQLLOGIC_CONCURRENT_LOOP;
	} else if (token == "foreach") {
		return SQLLogicTokenType::SQLLOGIC_FOREACH;
	} else if (token == "concurrentforeach") {
		return SQLLogicTokenType::SQLLOGIC_CONCURRENT_FOREACH;
	} else if (token == "endloop") {
		return SQLLogicTokenType::SQLLOGIC_ENDLOOP;
	} else if (token == "require") {
		return SQLLogicTokenType::SQLLOGIC_REQUIRE;
	} else if (token == "require-env") {
		return SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV;
	} else if (token == "require-env-not") {
		return SQLLogicTokenType::SQLLOGIC_REQUIRE_ENV_NOT;
	} else if (token == "test-env") {
		return SQLLogicTokenType::SQLLOGIC_TEST_ENV;
	} else if (token == "load") {
		return SQLLogicTokenType::SQLLOGIC_LOAD;
	} else if (token == "restart") {
		return SQLLogicTokenType::SQLLOGIC_RESTART;
	} else if (token == "reconnect") {
		return SQLLogicTokenType::SQLLOGIC_RECONNECT;
	} else if (token == "sleep") {
		return SQLLogicTokenType::SQLLOGIC_SLEEP;
	} else if (token == "unzip") {
		return SQLLogicTokenType::SQLLOGIC_UNZIP;
	} else if (token == "tags") {
		return SQLLogicTokenType::SQLLOGIC_TAGS;
	} else if (token == "continue") {
		return SQLLogicTokenType::SQLLOGIC_CONTINUE;
	} else if (token == "include") {
		return SQLLogicTokenType::SQLLOGIC_INCLUDE;
	}
	Fail("Unrecognized parameter %s", token);
	return SQLLogicTokenType::SQLLOGIC_INVALID;
}

} // namespace duckdb
