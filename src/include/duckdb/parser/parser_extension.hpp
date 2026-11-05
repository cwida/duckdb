//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parser_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parser_options.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/peg/token_type.hpp"

namespace duckdb {
struct DBConfig;

//! A minimal token view handed to parser extensions: the token text together with its classified
//! TokenType.
struct SimpleToken {
	SimpleToken(string text_p, TokenType type_p) : text(std::move(text_p)), type(type_p) {
	}

	//! The raw token text as it appears in the query
	string text;
	//! The classified type of the token
	TokenType type;
};

//! The ParserExtensionInfo holds static information relevant to the parser extension
//! It is made available in the parse_function, and will be kept alive as long as the database system is kept alive
struct ParserExtensionInfo {
	virtual ~ParserExtensionInfo() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//===--------------------------------------------------------------------===//
// Parse
//===--------------------------------------------------------------------===//
enum class ParserExtensionResultType : uint8_t { PARSE_SUCCESSFUL, DISPLAY_ORIGINAL_ERROR, DISPLAY_EXTENSION_ERROR };

//! The ParserExtensionParseData holds the result of a successful parse step
//! It will be passed along to the subsequent plan function
struct ParserExtensionParseData {
	virtual ~ParserExtensionParseData() {
	}

	virtual unique_ptr<ParserExtensionParseData> Copy() const = 0;
	virtual string ToString() const = 0;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct ParserExtensionParseResult {
	ParserExtensionParseResult() : type(ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR) {
	}
	explicit ParserExtensionParseResult(string error_p)
	    : type(ParserExtensionResultType::DISPLAY_EXTENSION_ERROR), error(std::move(error_p)) {
	}
	explicit ParserExtensionParseResult(unique_ptr<ParserExtensionParseData> parse_data_p)
	    : type(ParserExtensionResultType::PARSE_SUCCESSFUL), parse_data(std::move(parse_data_p)) {
	}

	//! Whether or not parsing was successful
	ParserExtensionResultType type;
	//! The parse data (if successful)
	unique_ptr<ParserExtensionParseData> parse_data;
	//! The error message (if unsuccessful)
	string error;
	//! The error location (if unsuccessful)
	optional_idx error_location;
	//! How many leading tokens (from the start of the `tokens` view) the extension claimed:
	//!    > 0 : SUCCESS — the extension accepted that many tokens; the peeler resumes parsing
	//!          after them.
	//!   == 0 : the extension ran fine but did not claim any tokens (it's not taking this input) —
	//!          the peeler lets the next extension try.
	//!    < 0 : the extension wants to surface an error — the peeler throws (using `error` /
	//!          `error_location` if set).
	//! A positive count must not exceed the number of tokens in the view, or the peeler throws.
	int64_t consumed_tokens = 0;
};

//! Called when the PEG parser fails to parse a statement.
//!
//! `tokens` is the tokenized view of the source tail from the PEG failure point onward — one
//! SimpleToken (text + TokenType) per token, in source order. The extension dispatches on this
//! token stream and reports, via `ParserExtensionParseResult::consumed_tokens`, how many leading
//! tokens it claimed (> 0 success, 0 = ran but claimed nothing, < 0 = throw).
typedef ParserExtensionParseResult (*parse_function_t)(ParserExtensionInfo *info, const vector<SimpleToken> &tokens);
//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
struct ParserExtensionPlanResult { // NOLINT: work-around bug in clang-tidy
	//! The table function to execute
	TableFunction function;
	//! Parameters to the function
	vector<Value> parameters;
	//! The set of databases that will be modified by this statement (empty for a read-only statement)
	identifier_map_t<StatementProperties::ModificationInfo> modified_databases;
	//! Whether or not the statement requires a valid transaction to be executed
	bool requires_valid_transaction = true;
	//! What type of result set the statement returns
	StatementReturnType return_type = StatementReturnType::NOTHING;
};

typedef ParserExtensionPlanResult (*plan_function_t)(ParserExtensionInfo *info, ClientContext &context,
                                                     unique_ptr<ParserExtensionParseData> parse_data);

//===--------------------------------------------------------------------===//
// Parser override
//===--------------------------------------------------------------------===//
struct ParserOverrideResult {
	explicit ParserOverrideResult() : type(ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR) {};

	explicit ParserOverrideResult(vector<unique_ptr<SQLStatement>> statements_p)
	    : type(ParserExtensionResultType::PARSE_SUCCESSFUL), statements(std::move(statements_p)) {};

	explicit ParserOverrideResult(std::exception &error_p)
	    : type(ParserExtensionResultType::DISPLAY_EXTENSION_ERROR), error(error_p) {};

	ParserExtensionResultType type;
	vector<unique_ptr<SQLStatement>> statements;
	ErrorData error;
};

typedef ParserOverrideResult (*parser_override_function_t)(ParserExtensionInfo *info, const string &query,
                                                           ParserOptions &options);

//===--------------------------------------------------------------------===//
// ParserExtension
//===--------------------------------------------------------------------===//
class ParserExtension {
public:
	//! The parse function of the parser extension.
	//! Takes a query string as input and returns ParserExtensionParseData (on success) or an error
	parse_function_t parse_function = nullptr;

	//! The plan function of the parser extension
	//! Takes as input the result of the parse_function, and outputs various properties of the resulting plan
	plan_function_t plan_function = nullptr;

	//! Override the current parser with a new parser and return a vector of SQL statements
	parser_override_function_t parser_override = nullptr;

	//! Additional parser info passed to the parse function
	shared_ptr<ParserExtensionInfo> parser_info;

	static void Register(DBConfig &config, ParserExtension extension);
};

//===--------------------------------------------------------------------===//
// PostParseExtension
//===--------------------------------------------------------------------===//
//! What a post_parse_function is given along with the statement
struct PostParseExtensionInput {
	//! The connection the statement was parsed for
	ClientContext &context;
	//! The PostParseExtension's parser_info
	optional_ptr<ParserExtensionInfo> info;
	//! The statement wraps a fragment of SQL rather than being a statement the client wrote. To parse an expression
	//! list, an ORDER BY or GROUP BY list, an UPDATE SET list, a VALUES list or a column list (type strings are
	//! parsed as one), the parser wraps the fragment in a statement it builds itself: SELECT <list>, SELECT * FROM tbl
	//! ORDER BY <list>, SELECT 42 GROUP BY <list>, UPDATE tbl SET <list>, VALUES <list>, or CREATE TABLE tbl
	//! (<columns>). It is always exactly one statement of the wrapper's type: a fragment that parses into anything
	//! else is rejected with a ParserException before any hook sees it. Only the statement type and the text the parser
	//! puts around the fragment (such as FROM tbl, UPDATE tbl and CREATE TABLE tbl) are the parser's; everything else
	//! in the statement is the client's SQL, including the tables and table functions a subquery in it names. A hook
	//! that checks statement types or table names should skip only the parser's parts, and check the rest as it would
	//! a statement the client wrote. Find the parser's parts by their position in the wrapper (the outer FROM clause,
	//! the UPDATE target, the created table), not by the name tbl: the client's SQL can name a table tbl too.
	//! These reach the hook for the relational API's expression strings, COPY's ORDER_BY option, and type strings such
	//! as read_csv(columns = {...}), including type strings in SQL the engine builds for the client's statement (the
	//! column types the file log storage reads its files with).
	bool is_fragment;
};

//! Called on every statement parsed from SQL text that runs for a client, after it is parsed (by the parser or a
//! parser_override extension) and before it is bound, and on the deserialized statement json_execute_serialized_sql
//! runs. That is:
//!  - each statement of a client's query, prepare or extract-statements input (C++, C API v1 and v2), before
//!    statement preprocessing. That includes SQL text engine or extension code runs on a Connection;
//!  - each statement a PRAGMA expands to, during preprocessing, all of them before the first is bound; for IMPORT
//!    DATABASE, the SQL in the directory's files;
//!  - the SQL text given to Connection::RelationFromQuery, and the statement of an appender at every flush: a query
//!    appender's query, or the INSERT a table appender (or Connection::Append) builds from the table;
//!  - SQL parsed while the client's statement is bound, so the hook runs inside that binding: the argument of
//!    query(), the SQL query_table() builds from its table names, and the subquery a C API v2 replacement scan
//!    supplies;
//!  - the argument of json_serialize_plan, each time the function is evaluated (all of its statements before the
//!    first is bound). That is while the client's statement executes, once for each row of a column argument and on
//!    the worker threads that evaluate it; a constant argument may also be evaluated while the statement is planned
//!    (constant folding);
//!  - the statement json_execute_serialized_sql runs. The table function binds and runs the deserialized statement
//!    on a connection of its own, and first gives it to the hooks with the client's context, while the client's
//!    statement is bound; it has no query text (SQLStatement::query is empty), so a hook must check the syntax tree.
//!    The PRAGMA renders the statement as SQL, which reaches the hook like any statement a PRAGMA expands to;
//!  - fragments of the client's SQL, wrapped in a statement the parser builds (see PostParseExtensionInput).
//! The statement is passed as parsed, before preprocessing unpacks it: a PIVOT without an IN list, for example, is
//! one MULTI_STATEMENT, and its CREATE TYPE and SELECT are its children (MultiStatement::statements).
//! The hook may throw to reject the statement, which fails the client's request (json_serialize_plan returns the
//! error as its error result). Reject with an exception other than ParserException, such as PermissionException: a
//! caller may catch ParserException to report input that does not parse in its own words (a type string that does not
//! parse is reported as an invalid type), which would replace the hook's message. A rejected statement of a client's
//! input fails where a parse error of that statement would, and no later statement of the same input runs.
//! The hook can run during the execution of the client's statement, and on several threads at once for the same
//! context (json_serialize_plan above): it must be safe to call concurrently.
//! The hook must not run statements on `context`: on most of these paths it runs with the context's lock held, and
//! that lock is not reentrant. Use a separate Connection.
//! Not called for:
//!  - definitions read from storage instead of parsed from the client's SQL: the views, macros, column defaults and
//!    CHECK constraints of a DuckDB database file a client attaches or opens. They are bound as part of the client's
//!    statement without passing this hook, so a consumer that relies on it must control which databases a client
//!    can attach (the ATTACH statement, with its path, does pass the hook). A catalog that stores definitions as SQL
//!    text decides by how it parses them: with a Parser(ClientContext &), as CreateViewInfo::FromCreateView does,
//!    each definition reaches the hook when it is loaded, as a statement that is not a fragment (a view as a CREATE
//!    VIEW statement, so a hook that rejects CREATE statements fails reads of the view); parsed without a context,
//!    none does. A consumer should rely on neither;
//!  - SQL the engine parses for itself: built-in macro and view definitions, the SQL SHOW and DESCRIBE generate, and
//!    the subqueries duckdb_logs() reads the logs with (type strings in them still reach the hook as fragments);
//!  - the statement query verification (a debug setting) parses again, which the hook has already seen;
//!  - json_serialize_sql, which only parses;
//!  - a statement C++ code passes in already parsed, such as to Connection::Query(unique_ptr<SQLStatement>).
//! Parse SQL that a client's request runs with Parser(ClientContext &), which passes each statement to the hooks,
//! and the engine's own SQL listed above without a context (Parser::WithoutPostParseHooks or
//! Parser::GetBuiltinParser). Call PostParseExtension::CallPostParseFunctions only on statements a client's SQL was
//! parsed into without such a Parser, as ParseIterator does, and on statements a client's request runs that were not
//! parsed from SQL text, as json_execute_serialized_sql does.
typedef void (*post_parse_function_t)(PostParseExtensionInput &input, const SQLStatement &statement);

//! Registers a post_parse_function. Not a member of ParserExtension: extensions built against an older header pass
//! ParserExtension to ParserExtension::Register by value, so its layout must not change.
class PostParseExtension {
public:
	//! Inspects a statement after parsing; throw to reject it (see post_parse_function_t)
	post_parse_function_t post_parse_function = nullptr;

	//! Passed to post_parse_function
	shared_ptr<ParserExtensionInfo> parser_info;

	static void Register(DBConfig &config, PostParseExtension extension);
	//! Calls the post_parse_function of every registered PostParseExtension, in registration order, on a statement
	//! parsed from SQL text that runs for a client (see post_parse_function_t). is_fragment says the statement wraps
	//! a fragment (see PostParseExtensionInput). An exception a hook throws propagates.
	static void CallPostParseFunctions(ClientContext &context, const SQLStatement &statement, bool is_fragment = false);
};

} // namespace duckdb
