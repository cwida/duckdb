#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"

#include <atomic>
#include <thread>

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

TEST_CASE("INSERT ToString keeps the ORDER BY, LIMIT and OFFSET of a VALUES source", "[api][parser]") {
	RequireRoundTrip("INSERT INTO t VALUES (3), (1), (2) ORDER BY 1 LIMIT 1",
	                 "INSERT INTO t (VALUES (3), (1), (2)) ORDER BY 1 LIMIT 1");
	RequireRoundTrip("INSERT INTO t VALUES (3), (1), (2) LIMIT 2 OFFSET 1",
	                 "INSERT INTO t (VALUES (3), (1), (2)) LIMIT 2 OFFSET 1");
	RequireRoundTrip("INSERT INTO t VALUES (3), (1), (2) OFFSET 1", "INSERT INTO t (VALUES (3), (1), (2)) OFFSET 1");
	RequireRoundTrip("INSERT INTO t VALUES (3), (1), (2) LIMIT 50 PERCENT",
	                 "INSERT INTO t (VALUES (3), (1), (2)) LIMIT (50) %");
	RequireRoundTrip("INSERT INTO t VALUES (3, NULL), (1, 2) ORDER BY 2 DESC NULLS FIRST, 1",
	                 "INSERT INTO t (VALUES (3, NULL), (1, 2)) ORDER BY 2 DESC NULLS FIRST, 1");
	RequireRoundTrip("INSERT INTO t VALUES (3), (1) ORDER BY ALL",
	                 "INSERT INTO t (VALUES (3), (1)) ORDER BY COLUMNS(*)");
	RequireRoundTrip("INSERT INTO t (VALUES (3), (1)) ORDER BY 1 LIMIT 1",
	                 "INSERT INTO t (VALUES (3), (1)) ORDER BY 1 LIMIT 1");
	RequireRoundTrip(R"(INSERT INTO "My Table" ("Two Words") VALUES (3), (1) ORDER BY 1 LIMIT 1 RETURNING *)",
	                 R"(INSERT INTO "My Table" ("Two Words") (VALUES (3), (1)) ORDER BY 1 LIMIT 1 RETURNING *)");
}

TEST_CASE("INSERT ToString keeps the VALUES modifiers before ON CONFLICT", "[api][parser]") {
	RequireRoundTrip(
	    "INSERT INTO t AS x VALUES (3), (1) ORDER BY 1 LIMIT 1 ON CONFLICT (k) DO UPDATE SET v = x.v + 1",
	    "INSERT INTO t AS x (VALUES (3), (1)) ORDER BY 1 LIMIT 1 ON CONFLICT (k) DO UPDATE SET v = (x.v + 1)");
	RequireRoundTrip("INSERT OR REPLACE INTO t VALUES (3), (1) LIMIT 1",
	                 "INSERT OR REPLACE INTO t (VALUES (3), (1)) LIMIT 1");
	RequireRoundTrip("INSERT OR IGNORE INTO t VALUES (3), (1) LIMIT 1",
	                 "INSERT INTO t (VALUES (3), (1)) LIMIT 1 ON CONFLICT DO NOTHING");
}

TEST_CASE("INSERT ToString keeps a WITH clause before INSERT with the VALUES modifiers", "[api][parser]") {
	RequireRoundTrip("WITH c AS (SELECT 1) INSERT INTO t VALUES (4), (5) LIMIT 1",
	                 "WITH c AS (SELECT 1) INSERT INTO t (VALUES (4), (5)) LIMIT 1");
}

TEST_CASE("INSERT ToString of a SELECT source and DEFAULT VALUES", "[api][parser]") {
	RequireRoundTrip("INSERT INTO t SELECT a FROM s ORDER BY a LIMIT 1 OFFSET 2",
	                 "INSERT INTO t SELECT a FROM s ORDER BY a LIMIT 1 OFFSET 2");
	RequireRoundTrip("INSERT INTO t SELECT DISTINCT a FROM s ORDER BY a LIMIT 1",
	                 "INSERT INTO t SELECT DISTINCT a FROM s ORDER BY a LIMIT 1");
	RequireRoundTrip("INSERT INTO t BY NAME SELECT 1 AS a ORDER BY 1 LIMIT 1",
	                 "INSERT INTO t BY NAME SELECT 1 AS a ORDER BY 1 LIMIT 1");
	RequireRoundTrip("INSERT INTO t (SELECT a FROM s UNION ALL SELECT b FROM s) ORDER BY 1 LIMIT 1",
	                 "INSERT INTO t (SELECT a FROM s) UNION ALL (SELECT b FROM s) ORDER BY 1 LIMIT 1");
	RequireRoundTrip("INSERT INTO t DEFAULT VALUES RETURNING a AS b", "INSERT INTO t DEFAULT VALUES RETURNING a AS b");
}

TEST_CASE("Printing one INSERT from several threads leaves its VALUES list unchanged", "[api][parser][interquery]") {
	auto parsed = ParseOne("INSERT INTO t VALUES (3), (1), (2) ORDER BY 1 LIMIT 1");
	const auto &statement = parsed->Cast<InsertStatement>();
	REQUIRE(statement.GetValuesList());
	const auto snapshot = statement.Copy();
	const auto alias = statement.GetValuesList()->alias;
	const auto expected = statement.ToString();

	// Printing must not write into the node, and only concurrent prints can see a write that is undone afterwards.
	std::atomic<idx_t> mismatches {0};
	vector<std::thread> threads;
	for (idx_t thread_idx = 0; thread_idx < 8; thread_idx++) {
		threads.emplace_back([&]() {
			for (idx_t print_idx = 0; print_idx < 2000; print_idx++) {
				if (statement.ToString() != expected) {
					mismatches++;
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(mismatches == 0);
	REQUIRE(statement.GetValuesList()->alias == alias);
	REQUIRE(StatementNode(statement).Equals(&StatementNode(*snapshot)));
}

static void RequireSameRows(Connection &con, const string &table, const string &sql, const vector<Value> &expected) {
	auto printed = ParseOne(sql)->ToString();
	INFO("printed: " << printed);
	for (auto &text : {sql, printed}) {
		INFO("running: " << text);
		REQUIRE_NO_FAIL(con.Query("DELETE FROM " + table));
		REQUIRE_NO_FAIL(con.Query(text));
		auto result = con.Query("SELECT a FROM " + table + " ORDER BY a NULLS FIRST");
		REQUIRE(CHECK_COLUMN(result, 0, expected));
	}
}

TEST_CASE("The printed INSERT inserts the rows the VALUES modifiers select", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '4GB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (a INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u (a INTEGER PRIMARY KEY)"));
	RequireSameRows(con, "t", "INSERT INTO t VALUES (3), (1), (2) ORDER BY 1 LIMIT 1", {1});
	RequireSameRows(con, "t", "INSERT INTO t VALUES (3), (1), (2) ORDER BY 1 DESC LIMIT 2 OFFSET 1", {1, 2});
	RequireSameRows(con, "t", "INSERT INTO t VALUES (3), (1), (2) ORDER BY 1 OFFSET 2", {3});
	RequireSameRows(con, "t", "INSERT INTO t VALUES (4), (3), (1), (2) ORDER BY 1 LIMIT 50 PERCENT", {1, 2});
	RequireSameRows(con, "t", "INSERT INTO t VALUES (3), (NULL), (1) ORDER BY 1 NULLS FIRST LIMIT 2", {Value(), 1});
	RequireSameRows(con, "t", "INSERT INTO t VALUES (2), (1), (2) ORDER BY 1 DESC LIMIT 2", {2, 2});
	RequireSameRows(con, "t", "INSERT INTO t WITH c AS (SELECT 1) VALUES (5), (6) ORDER BY 1 DESC LIMIT 1", {6});
	RequireSameRows(con, "u", "INSERT INTO u VALUES (3), (1), (2) ORDER BY 1 LIMIT 2 ON CONFLICT DO NOTHING", {1, 2});
	RequireSameRows(con, "u", "INSERT OR REPLACE INTO u VALUES (3), (1), (2) ORDER BY 1 DESC LIMIT 1", {3});
}
