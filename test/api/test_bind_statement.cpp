#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include <atomic>

using namespace duckdb;

static StatementSignature BindOne(Connection &con, const string &sql) {
	Parser parser(*con.context);
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	return con.context->BindStatement(std::move(parser.statements[0]));
}

TEST_CASE("BindStatement reports the result and parameter schema", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b VARCHAR)"));

	// Result columns, plus $1's type inferred from the comparison against a.
	auto sig = BindOne(con, "SELECT a, b FROM t WHERE a = $1");
	REQUIRE(sig.names.size() == 2);
	REQUIRE(sig.names[0] == "a");
	REQUIRE(sig.types[0] == LogicalType::INTEGER);
	REQUIRE(sig.names[1] == "b");
	REQUIRE(sig.types[1] == LogicalType::VARCHAR);
	REQUIRE(sig.parameters.size() == 1);
	REQUIRE(sig.parameters[0].identifier == "1");
	REQUIRE(sig.parameters[0].type == LogicalType::INTEGER);

	// A non-SELECT still reports a column: the changed-rows count.
	auto insert_sig = BindOne(con, "INSERT INTO t VALUES (1, 'x')");
	REQUIRE(insert_sig.names.size() == 1);
	REQUIRE(insert_sig.types[0] == LogicalType::BIGINT);
	REQUIRE(insert_sig.parameters.empty());

	// A bind error throws.
	REQUIRE_THROWS(BindOne(con, "SELECT * FROM no_such_table"));
}

TEST_CASE("BindStatement does not disturb a live streaming result", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);

	// Open a stream and consume one chunk, leaving it live mid-flight.
	auto stream = OpenStream(con, "SELECT i FROM range(5000) t(i)");
	auto first = stream->Fetch();
	REQUIRE(first);
	idx_t seen = first->size();

	// Binding another statement must not cancel the stream (Prepare would, via InitialCleanup).
	auto sig = BindOne(con, "SELECT 1");
	REQUIRE(sig.types.size() == 1);

	// Every remaining row still arrives.
	while (auto chunk = stream->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		seen += chunk->size();
	}
	REQUIRE(seen == 5000);
}

// Finds a parameter by identifier (StatementSignature does not order them).
static const StatementParameter &FindParam(const StatementSignature &sig, const string &id) {
	for (auto &p : sig.parameters) {
		if (p.identifier == id) {
			return p;
		}
	}
	FAIL("parameter not found: " + id);
	return sig.parameters.front();
}

static int64_t CountRows(Connection &con, const string &sql) {
	auto r = con.Query(sql);
	REQUIRE_NO_FAIL(*r);
	return r->Collection().GetValue(0, 0).GetValue<int64_t>();
}

TEST_CASE("A failing BindStatement leaves the enclosing transaction usable", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));

	// Binding is read-only, so a failing bind does not abort the caller's transaction:
	// the uncommitted INSERT stays visible and the transaction still commits.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1)"));
	REQUIRE_THROWS(BindOne(con, "SELECT * FROM no_such_table"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 1);
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (2)"));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 2);

	// This diverges from statement execution: a normal failing query still aborts the
	// transaction, because DuckDB aborts on any in-statement error. Bind is the
	// exception because it executes nothing.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE(con.Query("SELECT * FROM no_such_table")->HasError());
	REQUIRE(con.Query("SELECT 1")->HasError());
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));

	// Every kind of recoverable bind error leaves the transaction usable. Note these
	// span several exception classes (CatalogException for a missing table, various
	// BinderExceptions), which is why the fix catches broadly and re-throws only the
	// database-fatal case.
	vector<string> bad = {
	    "SELECT * FROM no_such_table",                      // catalog: table
	    "SELECT no_such_col FROM t",                        // binder: column
	    "SELECT no_such_fn(a) FROM t",                      // catalog: function
	    "SELECT a FROM t GROUP BY no_such_col",             // binder: group key
	    "INSERT INTO t VALUES (1, 2)",                      // binder: column count
	    "SELECT sum(a) OVER (ORDER BY no_such_col) FROM t", // binder: window key
	};
	for (auto &sql : bad) {
		REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
		REQUIRE_THROWS(BindOne(con, sql));
		REQUIRE_NO_FAIL(con.Query("SELECT 1"));
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
	}
}

TEST_CASE("BindStatement error under autocommit is isolated", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1)"));

	// With no explicit transaction, a failing bind rolls back only its own implicit
	// transaction and leaves the connection fully usable.
	REQUIRE_THROWS(BindOne(con, "SELECT * FROM no_such_table"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 1);
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (2)"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 2);
}

TEST_CASE("BindStatement is read-only within a transaction", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));

	// A successful bind neither commits nor rolls back the caller's transaction: the
	// INSERT survives inside the transaction and is discarded by ROLLBACK, proving
	// bind did not commit it.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (99)"));
	BindOne(con, "SELECT 1");
	REQUIRE(CountRows(con, "SELECT count(*) FROM t WHERE a = 99") == 1);
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t WHERE a = 99") == 0);

	// Bind resolves against transaction-local, uncommitted catalog state.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tmp(x INTEGER)"));
	auto tmp_sig = BindOne(con, "SELECT x FROM tmp");
	REQUIRE(tmp_sig.names.size() == 1);
	REQUIRE(tmp_sig.names[0] == "x");
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	// After rollback the table is gone, so binding it now fails.
	REQUIRE_THROWS(BindOne(con, "SELECT x FROM tmp"));
}

TEST_CASE("BindStatement parameter inference and coverage", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b VARCHAR)"));

	// Un-anchored parameter: type stays UNKNOWN.
	auto s1 = BindOne(con, "SELECT $1");
	REQUIRE(s1.parameters.size() == 1);
	REQUIRE(s1.parameters[0].identifier == "1");
	REQUIRE(s1.parameters[0].type == LogicalType(LogicalTypeId::UNKNOWN));

	// Two un-anchored parameters: both UNKNOWN.
	auto s2 = BindOne(con, "SELECT $1 + $2");
	REQUIRE(s2.parameters.size() == 2);
	REQUIRE(FindParam(s2, "1").type == LogicalType(LogicalTypeId::UNKNOWN));
	REQUIRE(FindParam(s2, "2").type == LogicalType(LogicalTypeId::UNKNOWN));

	// Anchored by cast: concrete types.
	auto s3 = BindOne(con, "SELECT $1::INTEGER, $2::VARCHAR");
	REQUIRE(FindParam(s3, "1").type == LogicalType::INTEGER);
	REQUIRE(FindParam(s3, "2").type == LogicalType::VARCHAR);

	// Gapped positional ($1, $3, no $2): both present, keyed by their index.
	auto s4 = BindOne(con, "SELECT * FROM t WHERE a = $1 AND a = $3");
	REQUIRE(s4.parameters.size() == 2);
	REQUIRE(FindParam(s4, "1").type == LogicalType::INTEGER);
	REQUIRE(FindParam(s4, "3").type == LogicalType::INTEGER);

	// Out-of-order textual placement: each parameter typed by its own context.
	auto s5 = BindOne(con, "SELECT * FROM t WHERE b = $2 AND a = $1");
	REQUIRE(s5.parameters.size() == 2);
	REQUIRE(FindParam(s5, "1").type == LogicalType::INTEGER);
	REQUIRE(FindParam(s5, "2").type == LogicalType::VARCHAR);

	// Repeated parameter: a single entry.
	auto s6 = BindOne(con, "SELECT * FROM t WHERE a = $1 OR a = $1 + 1");
	REQUIRE(s6.parameters.size() == 1);
	REQUIRE(FindParam(s6, "1").type == LogicalType::INTEGER);

	// Named parameter.
	auto s7 = BindOne(con, "SELECT $foo::INTEGER");
	REQUIRE(s7.parameters.size() == 1);
	REQUIRE(s7.parameters[0].identifier == "foo");
	REQUIRE(s7.parameters[0].type == LogicalType::INTEGER);

	// Parameters across many clauses: every parsed parameter appears and is typed.
	auto s8 = BindOne(con, "SELECT $1::INTEGER FROM t WHERE a > $2::INTEGER "
	                       "GROUP BY a HAVING count(*) > $3::BIGINT LIMIT $4::BIGINT");
	REQUIRE(s8.parameters.size() == 4);
	for (auto id : {"1", "2", "3", "4"}) {
		REQUIRE(FindParam(s8, id).type != LogicalType(LogicalTypeId::UNKNOWN));
	}

	// Parameter inside a scalar subquery.
	auto s9 = BindOne(con, "SELECT (SELECT $1::INTEGER)");
	REQUIRE(s9.parameters.size() == 1);
	REQUIRE(FindParam(s9, "1").type == LogicalType::INTEGER);
}

// A scalar function that counts the rows it processes, so a test can observe how
// far a streaming query's execution has advanced.
static std::atomic<int64_t> probe_rows {0};
static void ProbeExec(DataChunk &args, ExpressionState &state, Vector &result) {
	probe_rows.fetch_add(static_cast<int64_t>(args.size()));
	UnaryExecutor::Execute<int64_t, int64_t>(args.data[0], result, args.size(), [](int64_t v) { return v; });
}

TEST_CASE("BindStatement does not advance a suspended streaming query", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction probe("probe_rows", {LogicalType::BIGINT}, LogicalType::BIGINT, ProbeExec);
	CreateScalarFunctionInfo info(probe);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
	probe_rows.store(0);

	// A range far larger than the streaming buffer: the query cannot materialize, so
	// after one fetch it is suspended with most rows still unproduced.
	const idx_t N = 2000000;
	auto stream = OpenStream(con, "SELECT probe_rows(i) FROM range(" + std::to_string(N) + ") t(i)");

	auto first = stream->Fetch();
	REQUIRE(first);
	idx_t seen = first->size();
	int64_t produced = probe_rows.load();
	// Genuinely mid-flight: some rows produced, far from all.
	REQUIRE(produced > 0);
	REQUIRE(produced < static_cast<int64_t>(N));

	// Bind while suspended: it must not run the query forward. If it did, the probe
	// count would jump toward N; instead it stays bounded by the streaming buffer.
	auto sig = BindOne(con, "SELECT 1");
	REQUIRE(sig.types.size() == 1);
	REQUIRE(probe_rows.load() < static_cast<int64_t>(N));

	// Draining resumes the same query: every row arrives exactly once.
	while (auto chunk = stream->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		seen += chunk->size();
	}
	REQUIRE(seen == N);
	REQUIRE(probe_rows.load() == static_cast<int64_t>(N));
}

TEST_CASE("A failing BindStatement while a stream is live", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto stream = OpenStream(con, "SELECT i FROM range(5000) t(i)");
	auto first = stream->Fetch();
	REQUIRE(first);
	idx_t seen = first->size();
	// A failing bind reuses and aborts the stream's implicit (autocommit) transaction.
	REQUIRE_THROWS(BindOne(con, "SELECT * FROM no_such_table"));
	// Probe: does the live stream survive the failed bind, or does draining break?
	while (auto chunk = stream->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		seen += chunk->size();
	}
	REQUIRE(seen == 5000);
}

// A scalar function whose bind throws a FATAL (database-invalidating) error, to
// exercise the path where a bind failure is NOT swallowed.
static unique_ptr<FunctionData> BoomBind(BindScalarFunctionInput &input) {
	throw FatalException("boom during bind");
}
static void BoomExec(DataChunk &, ExpressionState &, Vector &) {
}

TEST_CASE("A database-fatal bind error still invalidates", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction boom("boom", {}, LogicalType::INTEGER, BoomExec, BoomBind);
	CreateScalarFunctionInfo info(boom);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });

	// The bind throws a FATAL error. Unlike a recoverable bind error, BindStatement
	// does not swallow it: it propagates and the database is invalidated, so the next
	// query fails too.
	REQUIRE_THROWS(BindOne(con, "SELECT boom()"));
	REQUIRE(con.Query("SELECT 1")->HasError());
}

static string NestedFilterChain(idx_t depth) {
	string sql = "SELECT a, b FROM t";
	for (idx_t i = 0; i < depth; i++) {
		sql = "SELECT a, b FROM (" + sql + ") s" + to_string(i) + " WHERE a > -1";
	}
	return sql;
}

// The statement binds, so it is described; the planning after the bind refuses it, so Prepare and Query fail.
static void RequireDescribedThenRefused(Connection &con, const string &sql, const vector<string> &names,
                                        const vector<LogicalType> &types, const string &error) {
	auto sig = BindOne(con, sql);
	REQUIRE(sig.types == types);
	if (!names.empty()) {
		REQUIRE(sig.names.size() == names.size());
		for (idx_t i = 0; i < names.size(); i++) {
			REQUIRE(sig.names[i] == names[i]);
		}
	}
	REQUIRE(sig.parameters.empty());
	auto prepared = con.Prepare(sql);
	REQUIRE(prepared->HasError());
	REQUIRE(StringUtil::Contains(prepared->GetError(), error));
	auto result = con.Query(sql);
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), error));
}

TEST_CASE("BindStatement describes a statement whose plan is deeper than the tree depth limit",
          "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 'x')"));
	REQUIRE_NO_FAIL(con.Query("SET max_expression_depth=50"));

	// 30 nested subqueries stay below the binder depth limit of 50, while their plan, a projection and a filter
	// per level, is about 60 operators deep.
	RequireDescribedThenRefused(con, NestedFilterChain(30), {"a", "b"}, {LogicalType::INTEGER, LogicalType::VARCHAR},
	                            "Maximum tree depth of 50 exceeded in logical planner");

	// Below the limit the chain is described and runs.
	auto sig = BindOne(con, NestedFilterChain(20));
	REQUIRE(sig.types.size() == 2);
	REQUIRE(CountRows(con, "SELECT count(*) FROM (" + NestedFilterChain(20) + ")") == 1);

	// A chain deeper than the binder depth limit is refused by the bind itself, so describing it fails too.
	REQUIRE_THROWS_WITH(BindOne(con, NestedFilterChain(60)), Catch::Contains("Max expression depth limit of 50"));
}

TEST_CASE("BindStatement describes a correlated subquery that decorrelation refuses", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(a INTEGER, b VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 2)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (1, 'y')"));

	RequireDescribedThenRefused(con, "SELECT * FROM t WHERE EXISTS (SELECT * FROM u POSITIONAL JOIN (SELECT t.a) s)",
	                            {"a", "b"}, {LogicalType::INTEGER, LogicalType::INTEGER},
	                            "Positional join in correlated subqueries is not (yet) supported");
	RequireDescribedThenRefused(con, "SELECT (SELECT b FROM u POSITIONAL JOIN (SELECT t.a AS c) s) AS r FROM t", {"r"},
	                            {LogicalType::VARCHAR},
	                            "Positional join in correlated subqueries is not (yet) supported");

	// A correlated subquery that decorrelation accepts is described and runs.
	unique_ptr<QueryResult> result;
	auto sig = BindOne(con, "SELECT (SELECT b FROM u WHERE u.a = t.a) AS r FROM t");
	REQUIRE(sig.types.size() == 1);
	REQUIRE(sig.types[0] == LogicalType::VARCHAR);
	result = con.Query("SELECT (SELECT b FROM u WHERE u.a = t.a) AS r FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {"y"}));
}

TEST_CASE("BindStatement describes a write whose row trigger body decorrelation refuses", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(a INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(x INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (7)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TRIGGER trg AFTER INSERT ON t FOR EACH ROW "
	                          "INSERT INTO audit SELECT s.x FROM (SELECT NEW.a AS x) s POSITIONAL JOIN u"));

	// The changed rows column of a write with a row trigger is named by the expansion, so only its type is checked.
	RequireDescribedThenRefused(con, "INSERT INTO t VALUES (1, 2)", {}, {LogicalType::BIGINT},
	                            "Positional join in correlated subqueries is not (yet) supported");
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 0);
	REQUIRE(CountRows(con, "SELECT count(*) FROM audit") == 0);

	// Without the trigger the same write is described and runs.
	REQUIRE_NO_FAIL(con.Query("DROP TRIGGER trg ON t"));
	auto sig = BindOne(con, "INSERT INTO t VALUES (1, 2)");
	REQUIRE(sig.types.size() == 1);
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 2)"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 1);
}

TEST_CASE("The bind_statement verification passes statements whose two plannings agree", "[api][bind_statement]") {
	DuckDB db(nullptr);
	Connection con(db);
	unique_ptr<QueryResult> result;
	REQUIRE_NO_FAIL(con.Query("SET debug_verify_statement='bind_statement'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER, b VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE u(a INTEGER, b VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE seq"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 'x'), (2, NULL)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO u SELECT a, b FROM t WHERE a = 1"));
	REQUIRE_NO_FAIL(con.Query("UPDATE u SET b = 'z' WHERE a IN (SELECT a FROM t)"));
	result = con.Query("SELECT (SELECT b FROM u WHERE u.a = t.a) FROM t ORDER BY a");
	REQUIRE(CHECK_COLUMN(result, 0, {"z", Value()}));
	result = con.Query("SELECT nextval('seq')");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// Parameters with values, and PREPARE and EXECUTE as statements, which pass through the verification.
	result = con.Query("SELECT a + $1 FROM t ORDER BY a", 10);
	REQUIRE(CHECK_COLUMN(result, 0, {11, 12}));
	REQUIRE_NO_FAIL(con.Query("PREPARE p AS SELECT b FROM t WHERE a = $1"));
	result = con.Query("EXECUTE p(2)");
	REQUIRE(CHECK_COLUMN(result, 0, {Value()}));

	// A bind error is the one of the statement, not a verification error.
	auto missing = con.Query("SELECT * FROM no_such_table");
	REQUIRE(missing->HasError());
	REQUIRE(missing->GetErrorType() == ExceptionType::CATALOG);

	// The three classes that fail only in the full planning still fail with their own error.
	REQUIRE_NO_FAIL(con.Query("SET max_expression_depth=50"));
	auto deep = con.Query(NestedFilterChain(30));
	REQUIRE(deep->HasError());
	REQUIRE(StringUtil::Contains(deep->GetError(), "Maximum tree depth of 50 exceeded"));
	REQUIRE_NO_FAIL(con.Query("RESET max_expression_depth"));
	auto refused = con.Query("SELECT * FROM t WHERE EXISTS (SELECT * FROM u POSITIONAL JOIN (SELECT t.a) s)");
	REQUIRE(refused->HasError());
	REQUIRE(StringUtil::Contains(refused->GetError(), "Positional join in correlated subqueries"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(x INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TRIGGER trg AFTER INSERT ON u FOR EACH ROW "
	                          "INSERT INTO audit SELECT s.x FROM (SELECT NEW.a AS x) s POSITIONAL JOIN t"));
	auto triggered = con.Query("INSERT INTO u VALUES (5, 'v')");
	REQUIRE(triggered->HasError());
	REQUIRE(StringUtil::Contains(triggered->GetError(), "Positional join in correlated subqueries"));
	REQUIRE_NO_FAIL(con.Query("DROP TRIGGER trg ON u"));

	// A failing statement in an explicit transaction aborts it exactly as without the verification.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (3, 'w')"));
	REQUIRE(con.Query("SELECT * FROM no_such_table")->HasError());
	REQUIRE(con.Query("SELECT 1")->HasError());
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 2);

	// Statements that expand are left to execution.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE p AS SELECT * FROM (VALUES ('k', 1), ('l', 2)) v(k, v)"));
	result = con.Query("PIVOT p ON k USING sum(v)");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {2}));
}

static std::atomic<idx_t> post_bind_count {0};

static void CountPostBind(PlannerExtensionInput &input, BoundStatement &statement) {
	post_bind_count++;
}

TEST_CASE("BindStatement runs the post bind hooks once, and the bind_statement verification plans in both modes",
          "[api][bind_statement]") {
	DBConfig config;
	PlannerExtension extension;
	extension.post_bind_function = CountPostBind;
	PlannerExtension::Register(config, extension);
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto before = post_bind_count.load();
	BindOne(con, "SELECT 42");
	REQUIRE(post_bind_count.load() - before == 1);

	before = post_bind_count.load();
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE(post_bind_count.load() - before == 1);

	REQUIRE_NO_FAIL(con.Query("SET debug_verify_statement='bind_statement'"));
	before = post_bind_count.load();
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE(post_bind_count.load() - before == 3);
}

// An InternalException aborts the process when DUCKDB_CRASH_ON_ASSERT is set.
#ifndef DUCKDB_CRASH_ON_ASSERT
enum class DisagreeingHook { RENAME, RETYPE, REBIND, FAIL_BIND_ONLY, FAIL_WITH_TWO_TYPES };

static DisagreeingHook disagreeing_hook = DisagreeingHook::RENAME;
static std::atomic<idx_t> disagreeing_hook_calls {0};

// Under the bind_statement verification the full planning binds first and the bind only planning second.
static void DisagreeingPostBind(PlannerExtensionInput &input, BoundStatement &statement) {
	auto call = ++disagreeing_hook_calls;
	switch (disagreeing_hook) {
	case DisagreeingHook::RENAME:
		if (call == 2) {
			statement.names[0] = Identifier("renamed");
		}
		break;
	case DisagreeingHook::RETYPE:
		if (call == 2) {
			statement.types[0] = LogicalType::VARCHAR;
		}
		break;
	case DisagreeingHook::REBIND:
		if (call == 2) {
			input.binder.GetStatementProperties().always_require_rebind = true;
		}
		break;
	case DisagreeingHook::FAIL_BIND_ONLY:
		if (call == 2) {
			throw BinderException("refused in the second planning");
		}
		break;
	case DisagreeingHook::FAIL_WITH_TWO_TYPES:
		if (call == 1) {
			throw BinderException("refused in the first planning");
		}
		if (call == 2) {
			throw InvalidInputException("refused in the second planning");
		}
		break;
	}
}

TEST_CASE("The bind_statement verification reports two plannings that disagree", "[api][bind_statement]") {
	DBConfig config;
	PlannerExtension extension;
	extension.post_bind_function = DisagreeingPostBind;
	PlannerExtension::Register(config, extension);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET debug_verify_statement='bind_statement'"));

	auto run = [&](DisagreeingHook hook) {
		disagreeing_hook = hook;
		disagreeing_hook_calls = 0;
		return con.Query("SELECT 42 AS x");
	};
	auto require_internal = [&](unique_ptr<QueryResult> result, const string &message) {
		REQUIRE(result->HasError());
		REQUIRE(result->GetErrorType() == ExceptionType::INTERNAL);
		REQUIRE(StringUtil::Contains(result->GetError(), message));
	};
	require_internal(run(DisagreeingHook::RENAME), "Bind only planning reports different names than full planning");
	require_internal(run(DisagreeingHook::RETYPE), "Bind only planning reports different types than full planning");
	require_internal(run(DisagreeingHook::REBIND),
	                 "Bind only planning reports different always require rebind than full planning");
	require_internal(run(DisagreeingHook::FAIL_BIND_ONLY),
	                 "Bind only planning failed where full planning succeeded: Binder Error: refused in the second "
	                 "planning");
	require_internal(run(DisagreeingHook::FAIL_WITH_TWO_TYPES), "Bind only planning failed with");

	// Without the verification the statement plans once, so the hook changes nothing that is compared.
	REQUIRE_NO_FAIL(con.Query("SET debug_verify_statement='none'"));
	unique_ptr<QueryResult> result = run(DisagreeingHook::RENAME);
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(disagreeing_hook_calls.load() == 1);
}
#endif

struct ClientCallbackError : public std::runtime_error {
	using std::runtime_error::runtime_error;
};

struct ClientUnwind {};

static unique_ptr<TableRef> ThrowingReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                                    optional_ptr<ReplacementScanData> data) {
	if (input.table_name == "client_frame") {
		throw ClientCallbackError("lookup failed in the client");
	}
	if (input.table_name == "client_unwind") {
		throw ClientUnwind();
	}
	return nullptr;
}

TEST_CASE("A client's own exception from a bind callback leaves BindStatement as the same object",
          "[api][bind_statement]") {
	DuckDB db(nullptr);
	DBConfig::GetConfig(*db.instance).replacement_scans.emplace_back(ThrowingReplacementScan);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));

	// Autocommit: the bind's own transaction ends and the connection stays usable.
	REQUIRE_THROWS_AS(BindOne(con, "SELECT * FROM client_frame"), ClientCallbackError);
	REQUIRE_THROWS_WITH(BindOne(con, "SELECT * FROM client_frame"), Catch::Equals("lookup failed in the client"));
	REQUIRE_THROWS_AS(BindOne(con, "SELECT * FROM client_unwind"), ClientUnwind);
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1)"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 1);

	// Explicit transaction: as for an engine bind error, the transaction stays usable and commits its work.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (2)"));
	REQUIRE_THROWS_AS(BindOne(con, "SELECT * FROM client_frame"), ClientCallbackError);
	REQUIRE_THROWS_AS(BindOne(con, "SELECT * FROM client_unwind"), ClientUnwind);
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 2);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 2);

	// An engine bind error still leaves as the engine's exception.
	REQUIRE_THROWS_AS(BindOne(con, "SELECT * FROM no_such_table"), Exception);
	REQUIRE_THROWS_WITH(BindOne(con, "SELECT * FROM no_such_table"), Catch::Contains("\"exception_type\":\"Catalog\""));
}

TEST_CASE("A client exception of any type ends the autocommit transaction of a bind", "[api][bind_statement]") {
	DuckDB db(nullptr);
	DBConfig::GetConfig(*db.instance).replacement_scans.emplace_back(ThrowingReplacementScan);
	Connection con(db);
	Connection other(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INTEGER)"));

	// The Relation route binds through the transaction wrapper, which must end the transaction it began.
	REQUIRE_THROWS_AS(con.RelationFromQuery("SELECT * FROM client_unwind"), ClientUnwind);
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	REQUIRE_NO_FAIL(other.Query("INSERT INTO t VALUES (1)"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 1);
	REQUIRE_THROWS_AS(con.RelationFromQuery("SELECT * FROM client_frame"), ClientCallbackError);
	REQUIRE(!con.context->transaction.HasActiveTransaction());

	// In an explicit transaction such an exception aborts it, as a standard exception does under the default policy.
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_THROWS_AS(con.RelationFromQuery("SELECT * FROM client_unwind"), ClientUnwind);
	REQUIRE(con.Query("SELECT 1")->HasError());
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(CountRows(con, "SELECT count(*) FROM t") == 1);
}
