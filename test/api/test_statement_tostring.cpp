#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"

using namespace duckdb;

static unique_ptr<SQLStatement> ParseOne(const string &sql) {
	Parser parser(ParserOptions::Builtin());
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	return std::move(parser.statements[0]);
}

static const QueryNode &StatementNode(const SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::INSERT_STATEMENT:
		return *statement.Cast<InsertStatement>().node;
	case StatementType::MERGE_INTO_STATEMENT:
		return *statement.Cast<MergeIntoStatement>().node;
	default:
		FAIL("unexpected statement type");
		throw InternalException("unreachable");
	}
}

static void RequireRoundTrip(const string &sql, const string &expected) {
	auto original = ParseOne(sql);
	auto printed = original->ToString();
	INFO("parsed:  " << sql);
	INFO("printed: " << printed);
	REQUIRE(printed == expected);
	auto reparsed = ParseOne(printed);
	INFO("printed again: " << reparsed->ToString());
	REQUIRE(StatementNode(*reparsed).Equals(&StatementNode(*original)));
	REQUIRE(reparsed->ToString() == printed);
}

TEST_CASE("INSERT ToString without ON CONFLICT is the reference", "[api][parser]") {
	RequireRoundTrip("INSERT INTO t VALUES (1, 2)", "INSERT INTO t (VALUES (1, 2))");
	RequireRoundTrip(R"(INSERT INTO "My Table" ("Two Words", "select") VALUES (1, 2))",
	                 R"(INSERT INTO "My Table" ("Two Words", "select") (VALUES (1, 2)))");
}

TEST_CASE("INSERT ToString quotes ON CONFLICT target columns", "[api][parser]") {
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT ("Two Words") DO NOTHING)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT ("Two Words") DO NOTHING)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT ("MixedCase") DO NOTHING)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT (MixedCase) DO NOTHING)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT ("select") DO NOTHING)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT ("select") DO NOTHING)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT ("a""b") DO NOTHING)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT ("a""b") DO NOTHING)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT ("Two Words", MixedCase, "select", plain) DO NOTHING)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT ("Two Words", MixedCase, "select", plain) DO NOTHING)");
	RequireRoundTrip("INSERT INTO t VALUES (1) ON CONFLICT DO NOTHING",
	                 "INSERT INTO t (VALUES (1)) ON CONFLICT DO NOTHING");
}

TEST_CASE("INSERT ToString quotes DO UPDATE SET columns", "[api][parser]") {
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT (k) DO UPDATE SET "V v" = 1)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT (k) DO UPDATE SET "V v" = 1)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT (k) DO UPDATE SET "MixedCase" = 1)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT (k) DO UPDATE SET MixedCase = 1)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT (k) DO UPDATE SET "select" = excluded."select")",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT (k) DO UPDATE SET "select" = excluded."select")");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT (k) DO UPDATE SET "a""b" = 1, "V v" = 2, plain = 3)",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT (k) DO UPDATE SET "a""b" = 1, "V v" = 2, plain = 3)");
	RequireRoundTrip(R"(INSERT INTO t VALUES (1) ON CONFLICT DO UPDATE SET ("V v", "select") = (1, 2))",
	                 R"(INSERT INTO t (VALUES (1)) ON CONFLICT DO UPDATE SET "V v" = 1, "select" = 2)");
}

TEST_CASE("INSERT ToString keeps the ON CONFLICT WHERE clauses", "[api][parser]") {
	RequireRoundTrip(
	    R"(INSERT INTO t VALUES (1) ON CONFLICT ("Two Words") WHERE "Two Words" > 0 DO UPDATE SET "V v" = 1 WHERE "V v" < 5)",
	    R"(INSERT INTO t (VALUES (1)) ON CONFLICT ("Two Words") WHERE ("Two Words" > 0) DO UPDATE SET "V v" = 1 WHERE ("V v" < 5))");
	RequireRoundTrip(
	    R"(INSERT INTO t AS "my alias" VALUES (1) ON CONFLICT ("Two Words") DO UPDATE SET "V v" = "my alias"."V v" + excluded."V v" RETURNING "V v" AS "Out Col")",
	    R"(INSERT INTO t AS "my alias" (VALUES (1)) ON CONFLICT ("Two Words") DO UPDATE SET "V v" = ("my alias"."V v" + excluded."V v") RETURNING "V v" AS "Out Col")");
}

TEST_CASE("INSERT ToString of the OR REPLACE and OR IGNORE shorthands", "[api][parser]") {
	RequireRoundTrip(R"(INSERT OR REPLACE INTO "My Table" VALUES (1))",
	                 R"(INSERT OR REPLACE INTO "My Table" (VALUES (1)))");
	RequireRoundTrip(R"(INSERT OR IGNORE INTO "My Table" VALUES (1))",
	                 R"(INSERT INTO "My Table" (VALUES (1)) ON CONFLICT DO NOTHING)");
}

TEST_CASE("MERGE INTO ToString quotes USING columns", "[api][parser]") {
	RequireRoundTrip(R"(MERGE INTO t USING s USING ("Two Words", MixedCase, "select") WHEN MATCHED THEN DELETE)",
	                 R"(MERGE INTO t USING s USING ("Two Words", MixedCase, "select") WHEN MATCHED THEN DELETE)");
}
