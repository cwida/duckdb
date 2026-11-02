#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
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
