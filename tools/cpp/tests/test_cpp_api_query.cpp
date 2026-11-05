#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

// ---------------------------------------------------------------------------
// Stable C++ API tests: statements, streaming results, prepared statements, column data.
// ---------------------------------------------------------------------------

namespace {

// A rendered box always contains the light-vertical box-drawing glyph (U+2502).
constexpr const char *kBoxVertical = "\342\224\202";
// The truncation ellipsis the renderer emits when a column is capped (U+2026).
constexpr const char *kEllipsis = "\342\200\246";

bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("Stable C++API: step loop drains a multi-chunk result", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto result = conn.Execute("SELECT i FROM range(100000) t(i)");

	idx_t total_rows = 0;
	idx_t chunk_count = 0;
	while (true) {
		auto step = result.Step();
		if (step.status == QueryResult::StepStatus::CHUNK) {
			REQUIRE(step.chunk);
			total_rows += step.chunk.GetRowCount();
			chunk_count++;
			continue;
		}
		REQUIRE(!step.chunk); // non-empty iff status is CHUNK
		if (step.status == QueryResult::StepStatus::WAITING) {
			result.Wait();
			continue;
		}
		REQUIRE(step.status == QueryResult::StepStatus::FINISHED);
		break;
	}
	REQUIRE(total_rows == 100000);
	REQUIRE(chunk_count > 1);

	// FINISHED is sticky, and waiting on a terminal result is a no-op.
	auto step = result.Step();
	REQUIRE(step.status == QueryResult::StepStatus::FINISHED);
	REQUIRE(!step.chunk);
	result.Wait();
}
TEST_CASE("Stable C++API: Drain applies side effects and reports rows changed", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	REQUIRE(conn.Execute("CREATE TABLE t (i INTEGER)").Drain() == 0);
	REQUIRE(conn.Execute("INSERT INTO t VALUES (1), (2), (3)").Drain() == 3);
	REQUIRE(conn.Execute("DELETE FROM t WHERE i = 1").Drain() == 1);
	REQUIRE(conn.Execute("SELECT i FROM range(1000) t(i)").Drain() == 0); // rows drained and discarded

	auto result = conn.Execute("SELECT i FROM t");
	idx_t rows = 0;
	while (auto chunk = result.FetchChunk()) {
		rows += chunk.GetRowCount();
	}
	REQUIRE(rows == 2);
}
TEST_CASE("Stable C++API: a busy connection refuses new work with RESOURCE_IN_USE", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto live = conn.Execute("SELECT i FROM range(100000) t(i)");

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1"), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));

	// Draining the live result frees the connection.
	while (live.FetchChunk()) {
	}
	auto second = conn.Execute("SELECT 1");
	REQUIRE(second.FetchChunk());
}
TEST_CASE("Stable C++API: Interrupt cancels a running query", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto result = conn.Execute("SELECT i FROM range(10000000) t(i)");
	conn.Interrupt();

	// Steps observe the cancellation as the sticky CANCELLED status.
	auto status = QueryResult::StepStatus::WAITING;
	for (int i = 0; i < 1000 && status != QueryResult::StepStatus::CANCELLED; i++) {
		status = result.Step().status;
	}
	REQUIRE(status == QueryResult::StepStatus::CANCELLED);

	// FetchChunk reports the same event on the error channel.
	REQUIRE_THROWS_MATCHES(result.FetchChunk(), Exception, HasErrorCode(DUCKDB_V2_ERROR_RUNTIME_INTERRUPT));

	// The cancelled result freed the connection.
	REQUIRE(conn.Execute("SELECT 1").Drain() == 0);
}
TEST_CASE("Stable C++API: GetQueryProgress reports idle values when no query is active", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto progress = conn.GetQueryProgress();
	REQUIRE(progress.percentage == -1.0);
	REQUIRE(progress.rows_processed == 0);
	REQUIRE(progress.total_rows_to_process == 0);
}

TEST_CASE("Stable C++API: GetQueryProgress reports unavailable progress", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.GetConfig().SetOption("enable_progress_bar", "true", SettingScope::SESSION);

	auto result = conn.Execute("SELECT sum(sin(i)) FROM unnest(range(100000)) AS t(i)");
	REQUIRE(result.FetchChunk());

	auto progress = conn.GetQueryProgress();
	REQUIRE(progress.percentage == 0.0);
	REQUIRE(progress.rows_processed == 0);
	REQUIRE(progress.total_rows_to_process == 0);
}

TEST_CASE("Stable C++API: ParseSQL iterates statements into Execute", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto statements = conn.ParseSQL("SELECT 42; SELECT 84; SELECT 126");
	int statement_count = 0;
	while (auto statement = statements.Next()) {
		auto result = conn.Execute(statement);
		REQUIRE(result.FetchChunk());
		result.Drain();
		statement_count++;
	}
	REQUIRE(statement_count == 3);

	// Exhaustion is idempotent.
	REQUIRE(!statements.Next());

	// The string-taking Execute is single-statement sugar.
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT 1; SELECT 2"), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(conn.Execute(""), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}
TEST_CASE("Stable C++API: SqlStatement parse-time metadata", "[cpp_api][sql_statement]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto statements = conn.ParseSQL("select $1; select 21");
	auto first = statements.Next();
	auto second = statements.Next();
	REQUIRE(!statements.Next());

	REQUIRE(first.GetStatementType() == StatementType::SELECT);
	REQUIRE(first.GetText() == "select $1; ");
	REQUIRE(first.GetParameterNames() == std::vector<std::string_view> {"1"});
	REQUIRE(conn.Bind(first).parameters.GetFieldName(0) == first.GetParameterNames()[0]);

	REQUIRE(second.GetStatementType() == StatementType::SELECT);
	REQUIRE(second.GetText() == "select 21");
	REQUIRE(second.GetParameterNames().empty());

	// The type is the parser's, before execution rewrites the statement; the enum reaches core's newest members.
	REQUIRE(conn.ParseSQL("PRAGMA version").Next().GetStatementType() == StatementType::PRAGMA);
	REQUIRE(conn.ParseSQL("CONNECT ':memory:'").Next().GetStatementType() == StatementType::CONNECT);
}

TEST_CASE("Stable C++API: Bind", "[cpp_api][statement_bind]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE t(a INTEGER, b VARCHAR)").Drain();

	auto iter = conn.ParseSQL("SELECT a, b FROM t WHERE a = $1");
	auto stmt = iter.Next();
	REQUIRE(static_cast<bool>(stmt));

	auto sig = conn.Bind(stmt);
	REQUIRE(sig.output.GetFieldCount() == 2);
	REQUIRE(sig.output.GetFieldName(0) == "a");
	REQUIRE(sig.output.GetFieldType(0) == factory.ParseType("INTEGER"));
	REQUIRE(sig.output.GetFieldName(1) == "b");
	REQUIRE(sig.output.GetFieldType(1) == factory.ParseType("VARCHAR"));
	REQUIRE(sig.parameters.GetFieldCount() == 1);
	REQUIRE(sig.parameters.GetFieldName(0) == "1");
	REQUIRE(sig.parameters.GetFieldType(0) == factory.ParseType("INTEGER"));

	// Non-consuming: the statement is still alive and re-bindable.
	REQUIRE(static_cast<bool>(stmt));
	auto sig2 = conn.Bind(stmt);
	REQUIRE(sig2.output.GetFieldCount() == 2);

	// A dynamic PIVOT expands into several statements, so its columns are known only once it executes.
	conn.Execute("CREATE TABLE sales(product VARCHAR, quarter VARCHAR, amount INTEGER)").Drain();
	auto piter = conn.ParseSQL("PIVOT sales ON quarter USING sum(amount)");
	auto pstmt = piter.Next();
	REQUIRE_THROWS_MATCHES(conn.Bind(pstmt), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED));
}
TEST_CASE("Stable C++API: QueryResult GetSchema", "[cpp_api][query_result]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto result = conn.Execute("SELECT 1 AS a, 'x' AS b");
	auto schema = result.GetSchema();
	REQUIRE(schema.GetFieldCount() == 2);
	REQUIRE(schema.GetFieldName(0) == "a");
	REQUIRE(schema.GetFieldName(1) == "b");
	REQUIRE(schema.GetFieldType(0) == factory.ParseType("INTEGER"));
}
TEST_CASE("Stable C++API: QueryResult result and statement types", "[cpp_api][query_result]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE rt(i INTEGER)").Drain();

	// Consume-vs-drain decided purely from GetResultType, no SQL inspection.
	auto run = [&](const char *sql) {
		auto result = conn.Execute(sql);
		auto types = std::make_pair(result.GetResultType(), result.GetStatementType());
		if (types.first == QueryResult::ResultType::QUERY_RESULT) {
			while (auto chunk = result.FetchChunk()) {
			}
		} else {
			result.Drain();
		}
		return types;
	};

	auto select = run("SELECT * FROM rt");
	REQUIRE(select.first == QueryResult::ResultType::QUERY_RESULT);
	REQUIRE(select.second == QueryResult::StatementType::SELECT);

	auto insert = run("INSERT INTO rt VALUES (1), (2)");
	REQUIRE(insert.first == QueryResult::ResultType::CHANGED_ROWS);
	REQUIRE(insert.second == QueryResult::StatementType::INSERT);

	auto ddl = run("CREATE TABLE rt2(i INTEGER)");
	REQUIRE(ddl.first == QueryResult::ResultType::NOTHING);
	REQUIRE(ddl.second == QueryResult::StatementType::CREATE);

	// The drain path applied the INSERT's side effects.
	auto verify = conn.Execute("SELECT count(*) FROM rt");
	auto chunk = verify.FetchChunk();
	auto view = chunk.GetVector(0).GetView();
	REQUIRE(view.Data<int64_t>()[view.SelAt(0)] == 2);
}
TEST_CASE("Stable C++API: prepared statements", "[cpp_api][prepared_statement]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE scores(id INTEGER, score INTEGER)").Drain();
	conn.Execute("INSERT INTO scores VALUES (1, 40), (2, 55), (3, 70), (4, 90)").Drain();

	// Value is move-only, so a parameter list is built by move, not brace-init.
	auto Params = [&factory](std::initializer_list<int64_t> values) {
		std::vector<Value> params;
		for (auto value : values) {
			params.push_back(Value::Create(factory, int64_t(value)));
		}
		return params;
	};

	SECTION("bind once, execute many with different parameters") {
		// Parse and bind once, then reuse across executions (binding neither executes
		// nor consumes): the parse-once, bind-once, execute-many pattern.
		auto iter = conn.ParseSQL("SELECT id, score FROM scores WHERE score >= $1 ORDER BY id");
		auto stmt = iter.Next();

		auto sig = conn.Bind(stmt);
		REQUIRE(sig.output.GetFieldCount() == 2);
		REQUIRE(sig.output.GetFieldName(0) == "id");
		REQUIRE(sig.output.GetFieldType(0) == factory.ParseType("INTEGER"));
		REQUIRE(sig.output.GetFieldName(1) == "score");
		REQUIRE(sig.parameters.GetFieldCount() == 1);
		REQUIRE(sig.parameters.GetFieldName(0) == "1");                          // $1 -> "1"
		REQUIRE(sig.parameters.GetFieldType(0) == factory.ParseType("INTEGER")); // inferred from score >= $1

		// Execute with one value, then another: different results, same statement, no
		// re-parse and no re-bind.
		auto high = Collect2<int32_t, int32_t>(conn.Execute(stmt, Params({50})), 0, 1);
		REQUIRE(high.size() == 3);
		REQUIRE(high[0].first == 2);
		REQUIRE(high[0].second == 55);
		REQUIRE(high[2].first == 4);

		auto higher = Collect2<int32_t, int32_t>(conn.Execute(stmt, Params({80})), 0, 1);
		REQUIRE(higher.size() == 1);
		REQUIRE(higher[0].first == 4);
		REQUIRE(higher[0].second == 90);

		// Still alive and re-bindable after executing.
		REQUIRE(static_cast<bool>(stmt));
		REQUIRE(conn.Bind(stmt).parameters.GetFieldCount() == 1);
	}

	SECTION("positional parameters bind in order") {
		auto iter = conn.ParseSQL("SELECT id, score FROM scores WHERE score >= $1 AND score < $2 ORDER BY id");
		auto stmt = iter.Next();
		REQUIRE(conn.Bind(stmt).parameters.GetFieldCount() == 2);

		// $1 = 50, $2 = 80 -> 50 <= score < 80.
		auto rows = Collect2<int32_t, int32_t>(conn.Execute(stmt, Params({50, 80})), 0, 1);
		REQUIRE(rows.size() == 2);
		REQUIRE(rows[0].first == 2);
		REQUIRE(rows[0].second == 55);
		REQUIRE(rows[1].first == 3);
		REQUIRE(rows[1].second == 70);
	}

	SECTION("a prepared DML statement reused to insert rows") {
		conn.Execute("CREATE TABLE log(v INTEGER)").Drain();
		auto iter = conn.ParseSQL("INSERT INTO log VALUES ($1)");
		auto stmt = iter.Next();

		auto sig = conn.Bind(stmt);
		REQUIRE(sig.parameters.GetFieldCount() == 1);
		REQUIRE(sig.output.GetFieldCount() == 1); // the changed-rows count column

		// Each execution inserts one row and reports one changed row.
		REQUIRE(conn.Execute(stmt, Params({10})).Drain() == 1);
		REQUIRE(conn.Execute(stmt, Params({20})).Drain() == 1);
		REQUIRE(conn.Execute(stmt, Params({30})).Drain() == 1);

		auto summary = Collect2<int64_t, int32_t>(conn.Execute("SELECT count(*) AS c, max(v) AS m FROM log"), 0, 1);
		REQUIRE(summary.size() == 1);
		REQUIRE(summary[0].first == 3);   // three rows inserted
		REQUIRE(summary[0].second == 30); // last value
	}
}
TEST_CASE("Stable C++API: Connection Execute binds named parameters", "[cpp_api][prepared_statement]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto statement = conn.ParseSQL("SELECT $left::BIGINT - $right::BIGINT").Next();

	std::vector<NamedParam> params;
	params.push_back({"right", Value::Create(factory, int64_t(4))});
	params.push_back({"left", Value::Create(factory, int64_t(10))});
	auto result = conn.Execute(statement, params);
	REQUIRE(result.FetchChunk().GetVector(0).GetValue(0).Get<int64_t>() == 6);
}
TEST_CASE("Stable C++API: RenderBox renders glyphs, the type row, and NULL cells", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto text = conn.Execute("SELECT * FROM (VALUES (1, 'x'), (2, NULL)) t(a, b)").RenderBox();

	// Box-drawing glyphs: this is the engine renderer, not a hand-rolled one.
	REQUIRE(Contains(text, kBoxVertical));
	// The type row shows the engine's short type names.
	REQUIRE(Contains(text, "int32"));   // column a (INTEGER)
	REQUIRE(Contains(text, "varchar")); // column b (VARCHAR)
	// A value cell and a default-rendered NULL cell.
	REQUIRE(Contains(text, "x"));
	REQUIRE(Contains(text, "NULL"));
}

TEST_CASE("Stable C++API: RenderBox footer counts all rows even when max_rows bounds display", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// 100 rows, display bounded to 5: max_rows bounds DISPLAY, not the read.
	auto text = conn.Execute("SELECT i AS n FROM range(100) t(i)").RenderBox(/*max_rows=*/5);
	REQUIRE(Contains(text, "100 rows")); // exact total, matching the CLI
	REQUIRE(Contains(text, "5 shown"));  // display bounded to max_rows
	REQUIRE_FALSE(Contains(text, "100 shown"));
}

TEST_CASE("Stable C++API: RenderBox max_rows changes display and 0 selects the default", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto few = conn.Execute("SELECT i AS n FROM range(100) t(i)").RenderBox(/*max_rows=*/3);
	REQUIRE(Contains(few, "3 shown"));

	// 0 selects the renderer default (20).
	auto def = conn.Execute("SELECT i AS n FROM range(100) t(i)").RenderBox(/*max_rows=*/0);
	REQUIRE(Contains(def, "20 shown"));

	REQUIRE(few != def);
}

TEST_CASE("Stable C++API: RenderBox max_width and max_col_width change the layout", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// Five wide columns; explicit widths keep the test off the terminal probe.
	const char *sql = "SELECT repeat('a', 15) AS c1, repeat('b', 15) AS c2, repeat('c', 15) AS c3, "
	                  "repeat('d', 15) AS c4, repeat('e', 15) AS c5";

	// A tight max_width forces the renderer to hide columns: the hidden-column
	// ellipsis and an "N columns" footer appear.
	auto narrow = conn.Execute(sql).RenderBox(0, /*max_width=*/40);
	REQUIRE(Contains(narrow, kEllipsis));
	REQUIRE(Contains(narrow, "5 columns"));

	// A generous max_width fits every column: nothing is hidden.
	auto wide = conn.Execute(sql).RenderBox(0, /*max_width=*/200);
	REQUIRE_FALSE(Contains(wide, "5 columns"));
	REQUIRE(narrow != wide);

	// At the tight width, raising max_col_width wraps cells instead of hiding
	// columns: the layout changes and every column is shown again.
	auto wrapped = conn.Execute(sql).RenderBox(0, /*max_width=*/40, /*max_col_width=*/6);
	REQUIRE_FALSE(Contains(wrapped, "5 columns"));
	REQUIRE(wrapped != narrow);
}

TEST_CASE("Stable C++API: RenderBox render_mode selects rows vs columns layout", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	const char *sql = "SELECT 1 AS a, 2 AS b, 3 AS c";
	auto rows = conn.Execute(sql).RenderBox(0, 0, 0, "", /*render_mode=*/0);
	auto cols = conn.Execute(sql).RenderBox(0, 0, 0, "", /*render_mode=*/1);
	REQUIRE(rows != cols);
}

TEST_CASE("Stable C++API: RenderBox custom null_value overrides the default NULL text", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto def = conn.Execute("SELECT CAST(NULL AS INTEGER) AS b").RenderBox();
	REQUIRE(Contains(def, "NULL"));

	auto custom = conn.Execute("SELECT CAST(NULL AS INTEGER) AS b").RenderBox(0, 0, 0, "<nil>");
	REQUIRE(Contains(custom, "<nil>"));
	// The type row renders "int32", the value renders "<nil>": no "NULL" anywhere.
	REQUIRE_FALSE(Contains(custom, "NULL"));
}

TEST_CASE("Stable C++API: RenderBox consumes the result and frees the connection", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto r = conn.Execute("SELECT i FROM range(5) t(i)");
	auto text = r.RenderBox();
	REQUIRE(Contains(text, kBoxVertical));

	// The one-live-result slot was released: a new query runs on the same connection.
	auto next = conn.Execute("SELECT 42 AS answer");
	auto chunk = next.FetchChunk();
	REQUIRE(chunk);
	REQUIRE(chunk.GetRowCount() == 1);
}

TEST_CASE("Stable C++API: RenderBox on a partially consumed result renders the remainder", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// More than one chunk so a single fetch leaves a remainder.
	auto r = conn.Execute("SELECT i FROM range(5000) t(i)");
	auto first = r.FetchChunk();
	REQUIRE(first);
	auto consumed = first.GetRowCount();
	REQUIRE(consumed > 0);
	REQUIRE(consumed < 5000);

	auto text = r.RenderBox(); // renders only what remains
	auto remainder = idx_t(5000) - consumed;
	REQUIRE(Contains(text, std::to_string(remainder) + " rows"));
}

TEST_CASE("Stable C++API: RenderBox handles zero-row and no-row-output results", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// Zero-row SELECT: the schema is known and the footer reports 0 rows.
	auto empty = conn.Execute("SELECT i AS n FROM range(0) t(i)").RenderBox();
	REQUIRE(Contains(empty, kBoxVertical));
	REQUIRE(Contains(empty, "0 rows"));

	// A DDL statement produces no row output; rendering must not crash and the
	// side effect (the table) is applied by the drain inside RenderBox.
	auto ddl = conn.Execute("CREATE TABLE rb_ddl(x INTEGER)").RenderBox();
	REQUIRE_FALSE(ddl.empty());
	// Proof the CREATE took effect (the result was drained, not abandoned).
	REQUIRE(conn.Execute("INSERT INTO rb_ddl VALUES (1)").Drain() == 1);
}

TEST_CASE("Stable C++API: RenderBox limit yields an approximate '? rows' footer", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// The .show() idiom: wrap the query with LIMIT n and pass n as limit, so the
	// footer honestly reads "? rows" once the bound is filled rather than
	// reporting the truncated count as the exact total.
	auto bounded = conn.Execute("SELECT i FROM range(21) t(i)").RenderBox(0, 0, 0, "", 0, /*limit=*/21);
	REQUIRE(Contains(bounded, "? rows"));
	REQUIRE_FALSE(Contains(bounded, "21 rows"));

	// With limit 0 the count is known and exact, so no "? rows" appears.
	auto exact = conn.Execute("SELECT i FROM range(21) t(i)").RenderBox();
	REQUIRE_FALSE(Contains(exact, "? rows"));
}
