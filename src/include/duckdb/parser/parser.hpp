//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parser.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/simplified_token.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class ClientContext;
struct CompiledGrammar;
struct MatcherToken;
class TokenIterator;
class GroupByNode;
struct UnicodeSpace {
	UnicodeSpace(idx_t pos, idx_t bytes) : pos(pos), bytes(bytes) {
	}

	idx_t pos;
	idx_t bytes;
};

//! The parser is responsible for parsing the query and converting it into a set
//! of parsed statements. The parsed statements can then be converted into a
//! plan and executed.
class Parser {
public:
	//! Snapshot the connection's parser settings, extensions and cached grammar.
	explicit Parser(ClientContext &context);
	//! Preserve identifier spelling when reparsing generated SQL.
	explicit Parser(ClientContext &context, IdentifierCaseMode identifier_case_mode);
	//! Explicit standalone configuration, without a client context.
	explicit Parser(const ParserOptions &options);
	Parser(Parser &&other) noexcept;
	//! Built-in semantics and shared built-in grammar, independent of any connection.
	static Parser GetBuiltinParser();
	~Parser();

	//! The parsed SQL statements from an invocation to ParseQuery.
	vector<unique_ptr<SQLStatement>> statements;

public:
	//! Attempts to parse a query into a series of SQL statements. Returns
	//! whether or not the parsing was successful. If the parsing was
	//! successful, the parsed statements will be stored in the statements
	//! variable.
	void ParseQuery(const string &query);

	//! Parse a single TopLevelStatement from the already-tokenized `query`. On success advances
	//! `token_iterator` past the consumed tokens and returns the SQLStatement. Returns nullptr at
	//! end-of-input or when the matched TLS was a separator-only run (no statement). A segment the
	//! PEG grammar rejects is offered to the `parse_function` extensions first; only if none claims
	//! it is a ParserException with the detailed syntax error thrown.
	//!
	//! Does NOT populate `stmt->query` — the caller owns the source string and can slice it
	//! using `stmt->stmt_location` if needed.
	DUCKDB_API unique_ptr<SQLStatement> ParseTopLevelStatement(TokenIterator &token_iterator, const string &query);

	//! Tokenize a query, returning the raw tokens together with their locations
	static vector<SimplifiedToken> Tokenize(const string &query);

	//! Tokenize an error message, returning the raw tokens together with their locations
	static vector<SimplifiedToken> TokenizeError(const string &error_msg);

	//! Returns true if the given text matches a keyword of the parser
	static KeywordCategory IsKeyword(const string &text);
	//! Returns a list of all keywords in the parser
	static vector<ParserKeyword> KeywordList();
	// Returns the Keyword category
	static KeywordCategory ToKeywordCategory(const string &text);
	//! Parses a list of expressions (i.e. the list found in a SELECT clause)
	DUCKDB_API vector<unique_ptr<ParsedExpression>> ParseExpressionList(const string &select_list);
	//! Parses exactly one expression, throwing an InternalException otherwise
	DUCKDB_API unique_ptr<ParsedExpression> ParseSingleExpression(const string &expression);
	//! Parses a single SELECT statement into its node
	DUCKDB_API unique_ptr<QueryNode> ParseSelectNode(const string &query);
	//! Parses a list of GROUP BY expressions
	GroupByNode ParseGroupByList(const string &group_by);
	//! Parses a list as found in an ORDER BY expression (i.e. including optional ASCENDING/DESCENDING modifiers)
	vector<OrderByNode> ParseOrderList(const string &select_list);
	//! Parses an update list (i.e. the list found in the SET clause of an UPDATE statement)
	void ParseUpdateList(const string &update_list, vector<Identifier> &update_columns,
	                     vector<unique_ptr<ParsedExpression>> &expressions);
	//! Parses a VALUES list (i.e. the list of expressions after a VALUES clause)
	vector<vector<unique_ptr<ParsedExpression>>> ParseValuesList(const string &value_list);
	//! Parses a column list (i.e. as found in a CREATE TABLE statement)
	ColumnList ParseColumnList(const string &column_list);
	ColumnDefinition ParseColumnDefinition(const string &column_definition);

	static bool StripUnicodeSpaces(const string &query_str, string &new_query);

	//! Normalize a query string before parsing: validate UTF-8 (throws on invalid), then strip
	//! non-ASCII Unicode spaces
	static string NormalizeSQLString(const string &query);

	void ThrowParserOverrideError(ParserOverrideResult &result);

private:
	//! Run the `parse_function` extensions over the unconsumed tail of `query`. Returns the produced
	//! `ExtensionStatement` and advances `token_iterator` past the tokens the extension claimed, or
	//! nullptr if no extension claims the segment.
	unique_ptr<SQLStatement> TryParseExtensionStatement(TokenIterator &token_iterator, const string &query);
	CompiledGrammar &GetGrammar();

	ParserOptions options;
	shared_ptr<CompiledGrammar> compiled_grammar;
};
} // namespace duckdb
