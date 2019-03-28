#include "parser/parser.hpp"

#include "parser/transformer.hpp"

namespace postgres {
#include "parser/parser.h"
}

using namespace postgres;

using namespace duckdb;
using namespace std;


struct PGParseContext {
	void *context = nullptr;
	List* result= nullptr;

	~PGParseContext() {
		if (context) {
//			pg_query_parse_finish(context);
//			pg_query_free_parse_result(result);
		}
	}
};

Parser::Parser(ClientContext &context) : context(context) {
}

void Parser::ParseQuery(string query) {
	// first try to parse any PRAGMA statements
	if (ParsePragma(query)) {
		// query parsed as pragma statement
		// if there was no error we were successful
		return;
	}

	PGParseContext parse_context;
	// use the postgres parser to parse the query
	//parse_context.context = pg_query_parse_init();
	parse_context.result = raw_parser(query.c_str());
	// check if it succeeded
//	if (parse_context.result.error) {
//		throw ParserException(string(parse_context.result.error->message) + "[" +
//		                      to_string(parse_context.result.error->lineno) + ":" +
//		                      to_string(parse_context.result.error->cursorpos) + "]");
//		return;
//	}

	if (!parse_context.result) {
		// empty statement
		return;
	}

	// if it succeeded, we transform the Postgres parse tree into a list of
	// SQLStatements
	Transformer transformer;
	transformer.TransformParseTree(parse_context.result, statements);
}

enum class PragmaType : uint8_t { NOTHING, ASSIGNMENT, CALL };

bool Parser::ParsePragma(string &query) {
	// check if there is a PRAGMA statement, this is done before calling the
	// postgres parser
	static const string pragma_string = "PRAGMA";
	auto query_cstr = query.c_str();

	// skip any spaces
	size_t pos = 0;
	while (isspace(query_cstr[pos]))
		pos++;

	if (pos + pragma_string.size() >= query.size()) {
		// query is too small, can't contain PRAGMA
		return false;
	}

	if (query.compare(pos, pragma_string.size(), pragma_string.c_str()) != 0) {
		// statement does not start with PRAGMA
		return false;
	}
	pos += pragma_string.size();
	// string starts with PRAGMA, parse the pragma
	// first skip any spaces
	while (isspace(query_cstr[pos]))
		pos++;
	// now look for the keyword
	size_t keyword_start = pos;
	while (query_cstr[pos] && query_cstr[pos] != ';' && query_cstr[pos] != '=' && query_cstr[pos] != '(' &&
	       !isspace(query_cstr[pos]))
		pos++;

	// no keyword found
	if (pos == keyword_start) {
		throw ParserException("Invalid PRAGMA: PRAGMA without keyword");
	}

	string keyword = query.substr(keyword_start, pos - keyword_start);

	while (isspace(query_cstr[pos]))
		pos++;

	PragmaType type;
	if (query_cstr[pos] == '=') {
		// assignment
		type = PragmaType::ASSIGNMENT;
	} else if (query_cstr[pos] == '(') {
		// function call
		type = PragmaType::CALL;
	} else {
		// nothing
		type = PragmaType::NOTHING;
	}

	if (keyword == "table_info") {
		if (type != PragmaType::CALL) {
			throw ParserException("Invalid PRAGMA table_info: expected table name");
		}
		ParseQuery("SELECT * FROM pragma_" + query.substr(keyword_start));
	} else if (keyword == "enable_profile" || keyword == "enable_profiling") {
		// enable profiling
		if (type == PragmaType::ASSIGNMENT) {
			string assignment = StringUtil::Replace(StringUtil::Lower(query.substr(pos + 1)), ";", "");
			if (assignment == "json") {
				context.profiler.automatic_print_format = AutomaticPrintFormat::JSON;
			} else if (assignment == "query_tree") {
				context.profiler.automatic_print_format = AutomaticPrintFormat::QUERY_TREE;
			} else {
				throw ParserException("Unrecognized print format %s, supported formats: [json, query_tree]",
				                      assignment.c_str());
			}
		} else if (type == PragmaType::NOTHING) {
			context.profiler.automatic_print_format = AutomaticPrintFormat::QUERY_TREE;
		} else {
			throw ParserException("Cannot call PRAGMA enable_profiling");
		}
		context.profiler.Enable();
	} else if (keyword == "disable_profile" || keyword == "disable_profiling") {
		// enable profiling
		context.profiler.Disable();
		context.profiler.automatic_print_format = AutomaticPrintFormat::NONE;
	} else if (keyword == "profiling_output" || keyword == "profile_output") {
		// set file location of where to save profiling output
		if (type != PragmaType::ASSIGNMENT) {
			throw ParserException("Profiling output must be an assignmnet");
		}
		string location = StringUtil::Replace(StringUtil::Lower(query.substr(pos + 1)), ";", "");
		context.profiler.save_location = location;
	} else {
		throw ParserException("Unrecognized PRAGMA keyword: %s", keyword.c_str());
	}

	return true;
}
