#include "catch.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"

using namespace duckdb;

static duckdb::unique_ptr<SQLStatement> ParseSingleStatement(const string &query) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	return std::move(parser.statements[0]);
}

static void RequireStatementRoundTrip(const string &query, StatementType expected_type) {
	auto statement = ParseSingleStatement(query);
	REQUIRE(statement->type == expected_type);

	auto copy = statement->Copy();
	REQUIRE(copy->type == expected_type);
	auto rendered = copy->ToString();
	CAPTURE(query, rendered);

	auto reparsed = ParseSingleStatement(rendered);
	REQUIRE(reparsed->type == expected_type);
}

static QueryNode &GetQueryNode(SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		return *statement.Cast<SelectStatement>().node;
	case StatementType::INSERT_STATEMENT:
		return *statement.Cast<InsertStatement>().node;
	case StatementType::UPDATE_STATEMENT:
		return *statement.Cast<UpdateStatement>().node;
	case StatementType::DELETE_STATEMENT:
		return *statement.Cast<DeleteStatement>().node;
	case StatementType::MERGE_INTO_STATEMENT:
		return *statement.Cast<MergeIntoStatement>().node;
	default:
		throw InternalException("Statement does not contain a query node");
	}
}

static bool HasTokenType(const duckdb::vector<SimplifiedToken> &tokens, SimplifiedTokenType type) {
	for (auto &token : tokens) {
		if (token.type == type) {
			return true;
		}
	}
	return false;
}

TEST_CASE("Parser statement copies round-trip through SQL", "[parser]") {
	duckdb::vector<pair<string, StatementType>> statements {
	    {"CALL pragma_version()", StatementType::CALL_STATEMENT},
	    {"EXPLAIN SELECT 42", StatementType::EXPLAIN_STATEMENT},
	    {"EXPLAIN (FORMAT JSON) SELECT 42", StatementType::EXPLAIN_STATEMENT},
	    {"EXPLAIN (ANALYZE, FORMAT JSON) SELECT 42", StatementType::EXPLAIN_STATEMENT},
	    {"LOAD json", StatementType::LOAD_STATEMENT},
	    {"LOAD my_package FROM my_repo AS package_alias", StatementType::LOAD_STATEMENT},
	    {"INSTALL AND LOAD json FROM core", StatementType::LOAD_STATEMENT},
	    {"FORCE INSTALL AND LOAD json", StatementType::LOAD_STATEMENT},
	    {"CREATE OR REPLACE EXTENSION REPOSITORY repo WITH PREFIX 'https://example.com' USING PUBLIC KEYS 'a', 'b'",
	     StatementType::LOAD_STATEMENT},
	    {"DROP EXTENSION REPOSITORY IF EXISTS repo", StatementType::LOAD_STATEMENT},
	    {"UPDATE EXTENSIONS", StatementType::UPDATE_EXTENSIONS_STATEMENT},
	    {"UPDATE EXTENSIONS (json, parquet)", StatementType::UPDATE_EXTENSIONS_STATEMENT},
	    {"DISCONNECT", StatementType::DISCONNECT_STATEMENT},
	    {"COPY FROM DATABASE source_db TO target_db (SCHEMA)", StatementType::COPY_DATABASE_STATEMENT},
	    {"COPY FROM DATABASE source_db TO target_db (DATA)", StatementType::COPY_DATABASE_STATEMENT},
	};

	for (auto &entry : statements) {
		RequireStatementRoundTrip(entry.first, entry.second);
	}
}

TEST_CASE("Parser query nodes remain equal after copying and reparsing", "[parser]") {
	duckdb::vector<pair<string, StatementType>> statements {
	    {"SELECT DISTINCT ON (a) a, sum(b) FROM tbl WHERE a > 0 GROUP BY GROUPING SETS ((a), ()) "
	     "HAVING sum(b) > 0 QUALIFY row_number() OVER () = 1 ORDER BY a DESC NULLS LAST LIMIT 10 OFFSET 1",
	     StatementType::SELECT_STATEMENT},
	    {"SELECT * FROM tbl AT (VERSION => 42)", StatementType::SELECT_STATEMENT},
	    {"DESCRIBE SELECT 42 AS answer", StatementType::SELECT_STATEMENT},
	    {"SUMMARIZE SELECT 42 AS answer", StatementType::SELECT_STATEMENT},
	    {"PIVOT sales ON category IN ('a', 'b') USING sum(amount) GROUP BY id", StatementType::SELECT_STATEMENT},
	    {"UNPIVOT sales ON jan, feb INTO NAME month VALUE amount", StatementType::SELECT_STATEMENT},
	    {"SELECT 1 AS a UNION BY NAME SELECT 2 AS b ORDER BY ALL LIMIT 2", StatementType::SELECT_STATEMENT},
	    {"WITH RECURSIVE r(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 3) SELECT * FROM r",
	     StatementType::SELECT_STATEMENT},
	    {"INSERT INTO target AS t (id, value) VALUES (1, DEFAULT) ON CONFLICT (id) DO UPDATE SET "
	     "value = excluded.value WHERE t.value <> excluded.value RETURNING id, value",
	     StatementType::INSERT_STATEMENT},
	    {"UPDATE target AS t SET value = s.value, id = t.id + 1 FROM source AS s WHERE t.id = s.id RETURNING t.*",
	     StatementType::UPDATE_STATEMENT},
	    {"DELETE FROM target AS t USING source AS s WHERE t.id = s.id RETURNING t.id", StatementType::DELETE_STATEMENT},
	    {"MERGE INTO target AS t USING source AS s ON t.id = s.id "
	     "WHEN MATCHED AND s.value IS NULL THEN DELETE "
	     "WHEN MATCHED THEN UPDATE SET value = s.value "
	     "WHEN NOT MATCHED THEN INSERT (id, value) VALUES (s.id, s.value) "
	     "WHEN NOT MATCHED BY SOURCE THEN UPDATE SET value = 'missing' RETURNING merge_action, t.*",
	     StatementType::MERGE_INTO_STATEMENT},
	    {"WITH copied AS (COPY (SELECT 42 AS i) TO 'out.csv' (FORMAT CSV, RETURN_STATS)) SELECT * FROM copied",
	     StatementType::SELECT_STATEMENT},
	};

	for (auto &entry : statements) {
		auto statement = ParseSingleStatement(entry.first);
		REQUIRE(statement->type == entry.second);
		auto copy = statement->Copy();
		auto rendered = copy->ToString();
		CAPTURE(entry.first, rendered);
		REQUIRE(GetQueryNode(*statement).Equals(&GetQueryNode(*copy)));

		auto reparsed = ParseSingleStatement(rendered);
		REQUIRE(reparsed->type == entry.second);
		REQUIRE(GetQueryNode(*statement).Equals(&GetQueryNode(*reparsed)));
	}
}

TEST_CASE("Parser tokenizes errors for shell highlighting", "[parser]") {
	const string error = "Binder Error: Referenced column \"missing\" not found\n"
	                     "LINE 1: SELECT missing FROM tbl\n"
	                     "                       ^";
	auto tokens = Parser::TokenizeError(error);
	REQUIRE(tokens.size() == 11);
	REQUIRE(tokens[0].type == SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS);
	REQUIRE(tokens[0].start == 0);
	REQUIRE(HasTokenType(tokens, SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR));
	REQUIRE(HasTokenType(tokens, SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_SUGGESTION));
	REQUIRE(tokens[8].type == SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS);
	REQUIRE(tokens[8].start == error.find("FROM tbl"));

	for (auto &token : tokens) {
		REQUIRE(token.start < error.size());
	}

	auto unterminated = Parser::TokenizeError("plain error with 'unterminated");
	REQUIRE(unterminated.size() == 1);
	REQUIRE(unterminated[0].type == SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR);

	auto multiline_quote = Parser::TokenizeError("Error: bad 'quote\nstill bad");
	REQUIRE(multiline_quote.size() == 2);
}
