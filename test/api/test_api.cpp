#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/main/statement_iterator.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <chrono>
#include <fstream>
#include <thread>

using namespace duckdb;

TEST_CASE("Test comment in CPP API", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	con.Query("--ups");
	//! Should not crash
	REQUIRE(1);
}

TEST_CASE("Test StarExpression replace_list parameter", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto sql = "select * replace(i * $n as i) from range(1, 10) t(i)";
	auto stmts = con.ExtractStatements(sql);

	auto &select_stmt = stmts[0]->Cast<SelectStatement>();
	auto &select_node = select_stmt.node->Cast<SelectNode>();

	REQUIRE(select_node.select_list[0]->HasParameter());
}

TEST_CASE("Test ExtractStatements with an explicit transaction", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(a INT)"));

	// The ALTER expands into several statements when it runs, inside the user's transaction
	auto stmts = con.ExtractStatements("BEGIN; ALTER TABLE t ADD COLUMN c INT DEFAULT 1 NOT NULL; COMMIT;");
	REQUIRE(stmts.size() == 3);
	REQUIRE(stmts[1]->type == StatementType::MULTI_STATEMENT);
	for (auto &stmt : stmts) {
		REQUIRE_NO_FAIL(con.Query(std::move(stmt)));
	}
	REQUIRE(!con.context->transaction.HasActiveTransaction());
	auto result = con.Query("SELECT column_name FROM duckdb_columns() WHERE table_name = 't' ORDER BY column_index");
	REQUIRE(CHECK_COLUMN(result, 0, {"a", "c"}));
}

TEST_CASE("Test using connection after database is gone", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);
	// check that the connection works
	auto result = conn->Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	// destroy the database
	db.reset();
	// try to use the connection
	// it still works: the database remains until all connections are destroyed
	REQUIRE_NO_FAIL(conn->Query("SELECT 42"));

	// now try it with an open transaction
	db = make_uniq<DuckDB>(nullptr);
	conn = make_uniq<Connection>(*db);

	REQUIRE_NO_FAIL(conn->Query("BEGIN TRANSACTION"));
	result = conn->Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	db.reset();

	REQUIRE_NO_FAIL(conn->Query("SELECT 42"));
}

TEST_CASE("Test destroying connections with open transactions", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	{
		Connection con(*db);
		con.Query("BEGIN TRANSACTION");
		con.Query("CREATE TABLE test(i INTEGER);");
	}

	auto conn = make_uniq<Connection>(*db);
	REQUIRE_NO_FAIL(conn->Query("CREATE TABLE test(i INTEGER)"));
}

static void long_running_query(Connection *conn, bool *correct) {
	*correct = true;
	auto result = conn->Query("SELECT i1.i FROM integers i1, integers i2, integers i3, integers i4, integers i5, "
	                          "integers i6, integers i7, integers i8, integers i9, integers i10,"
	                          "integers i11, integers i12, integers i13");
	// the query should fail
	*correct = result->HasError();
}

TEST_CASE("Test closing database during long running query", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);
	// create the database
	REQUIRE_NO_FAIL(conn->Query("CREATE TABLE integers(i INTEGER)"));
	REQUIRE_NO_FAIL(conn->Query("INSERT INTO integers FROM range(10000)"));
	conn->DisableProfiling();
	// perform a long running query in the background (many cross products)
	bool correct = true;
	auto background_thread = std::thread(long_running_query, conn.get(), &correct);
	// wait a little bit
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	// destroy the database
	conn->Interrupt();
	db.reset();
	// wait for the thread
	background_thread.join();
	REQUIRE(correct);
	// try to use the connection
	REQUIRE_NO_FAIL(conn->Query("SELECT 42"));
}

TEST_CASE("Test closing result after database is gone", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);
	// check that the connection works
	auto result = conn->Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	// destroy the database
	db.reset();
	conn.reset();
	result.reset();

	// now the streaming result
	db = make_uniq<DuckDB>(nullptr);
	conn = make_uniq<Connection>(*db);
	// check that the connection works
	auto stream = OpenStream(*conn, "SELECT 42");
	// destroy the database
	db.reset();
	conn.reset();
	auto streaming_result = DrainStream(*stream);
	REQUIRE(CHECK_COLUMN(streaming_result, 0, {42}));
	stream.reset();
}

TEST_CASE("Test closing database with open prepared statements", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);

	auto p1 = conn->Prepare("CREATE TABLE a (i INTEGER)");
	REQUIRE_NO_FAIL(p1->Execute());
	auto p2 = conn->Prepare("INSERT INTO a VALUES (42)");
	REQUIRE_NO_FAIL(p2->Execute());

	db.reset();
	conn.reset();

	// the prepared statements live in the connection - they can no longer be executed once it is gone
	REQUIRE_FAIL(p2->Execute());
	p1.reset();
	p2.reset();
}

static void parallel_query(Connection *conn, bool *correct, size_t threadnr) {
	correct[threadnr] = true;
	for (size_t i = 0; i < 100; i++) {
		auto result = conn->Query("SELECT * FROM integers ORDER BY i");
		if (!CHECK_COLUMN(result, 0, {1, 2, 3, Value()})) {
			correct[threadnr] = false;
		}
	}
}

TEST_CASE("Test temp_directory defaults", "[api][.]") {
	const char *db_paths[] = {nullptr, "", ":memory:"};
	for (auto &path : db_paths) {
		auto db = make_uniq<DuckDB>(path);
		auto conn = make_uniq<Connection>(*db);

		REQUIRE(db->instance->config.options.temporary_directory == ".tmp");
	}
}

TEST_CASE("Test parallel usage of single client", "[api][.]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);

	REQUIRE_NO_FAIL(conn->Query("CREATE TABLE integers(i INTEGER)"));
	REQUIRE_NO_FAIL(conn->Query("INSERT INTO integers VALUES (1), (2), (3), (NULL)"));

	bool correct[20];
	std::thread threads[20];
	for (size_t i = 0; i < 20; i++) {
		threads[i] = std::thread(parallel_query, conn.get(), correct, i);
	}
	for (size_t i = 0; i < 20; i++) {
		threads[i].join();
		REQUIRE(correct[i]);
	}
}

static void parallel_query_with_new_connection(DuckDB *db, bool *correct, size_t threadnr) {
	correct[threadnr] = true;
	for (size_t i = 0; i < 100; i++) {
		auto conn = make_uniq<Connection>(*db);
		auto result = conn->Query("SELECT * FROM integers ORDER BY i");
		if (!CHECK_COLUMN(result, 0, {1, 2, 3, Value()})) {
			correct[threadnr] = false;
		}
	}
}

TEST_CASE("Test making and dropping connections in parallel to a single database", "[api][.]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);

	REQUIRE_NO_FAIL(conn->Query("CREATE TABLE integers(i INTEGER)"));
	REQUIRE_NO_FAIL(conn->Query("INSERT INTO integers VALUES (1), (2), (3), (NULL)"));

	bool correct[20];
	std::thread threads[20];
	for (size_t i = 0; i < 20; i++) {
		threads[i] = std::thread(parallel_query_with_new_connection, db.get(), correct, i);
	}
	for (size_t i = 0; i < 100; i++) {
		auto result = conn->Query("SELECT * FROM integers ORDER BY i");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3, Value()}));
	}
	for (size_t i = 0; i < 20; i++) {
		threads[i].join();
		REQUIRE(correct[i]);
	}
	auto result = conn->Query("SELECT * FROM integers ORDER BY i");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3, Value()}));
}

TEST_CASE("Test multiple result sets", "[api]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	con.ForceParallelism();

	result = con.Query("SELECT 42; SELECT 84");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	result = std::move(result->next);
	REQUIRE(CHECK_COLUMN(result, 0, {84}));
	REQUIRE(!result->next);
}

TEST_CASE("Test streaming API errors", "[api]") {
	duckdb::unique_ptr<QueryResult> result, result2;
	DuckDB db(nullptr);
	Connection con(db);

	const string failing_query =
	    "SELECT x::INT FROM (SELECT x::VARCHAR x FROM range(10) tbl(x) UNION ALL SELECT 'hello' x) tbl(x);";

	// a second stream is refused while the first is open
	auto stream = OpenStream(con, "SELECT 42;");
	auto refused = con.Submit("SELECT 42;");
	REQUIRE(refused->HasError());
	REQUIRE(refused->GetErrorType() == ExceptionType::RESOURCE_IN_USE);
	result2 = DrainStream(*stream);
	REQUIRE(CHECK_COLUMN(result2, 0, {42}));
	auto stream2 = OpenStream(con, "SELECT 42;");
	result2 = DrainStream(*stream2);
	REQUIRE(CHECK_COLUMN(result2, 0, {42}));

	// a retained result is refused while a stream is open, and taken once the stream is closed
	stream = OpenStream(con, "SELECT 42;");
	result2 = con.Query("SELECT 42;");
	REQUIRE(result2->HasError());
	REQUIRE(result2->GetErrorType() == ExceptionType::RESOURCE_IN_USE);
	stream->Close();
	result2 = con.Query("SELECT 42;");
	REQUIRE(CHECK_COLUMN(result2, 0, {42}));

	// error in binding
	result = con.Query("SELECT * FROM nonexistanttable");
	REQUIRE(!result->ToString().empty());
	REQUIRE_FAIL(result);

	// error in stream that only happens after fetching
	stream = OpenStream(con, failing_query);
	while (!stream->HasError()) {
		auto chunk = stream->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
	}
	REQUIRE(stream->HasError());

	// same query, retained instead of streamed
	result = con.Submit(failing_query);
	result->Complete();
	REQUIRE(!result->ToString().empty());
	REQUIRE_FAIL(result);
}

TEST_CASE("Test fetch API", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	duckdb::unique_ptr<QueryResult> result;

	// fetch from an error
	result = con.Query("SELECT 'hello'::INT");
	REQUIRE_THROWS(result->Fetch());

	result = con.Query("CREATE TABLE test (a INTEGER);");

	result = con.Query("select a from test where 1 <> 1");
	REQUIRE(CHECK_COLUMN(result, 0, {}));

	result = con.Query("INSERT INTO test VALUES (42)");
	result = con.Query("SELECT a from test");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	auto materialized_result = con.Query("select a from test");
	REQUIRE(CHECK_COLUMN(materialized_result, 0, {42}));

	// one stream at a time: each drained stream frees the connection for the next
	for (idx_t i = 0; i < 4; i++) {
		auto stream = OpenStream(con, "SELECT a from test");
		result = DrainStream(*stream);
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
}

TEST_CASE("Test fetch API not to completion", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);
	// remove connection with active stream result
	auto stream = OpenStream(*conn, "SELECT 42");
	// close the connection
	conn.reset();
	// now try to fetch a chunk, this should not return a nullptr
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	// Only if we would call Fetch again would we close the stream
	// this is testing that it can get cleaned up without this.

	db.reset();
}

TEST_CASE("Test fetch API robustness", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);

	// remove connection with active stream result
	auto stream = OpenStream(*conn, "SELECT 42");
	// close the connection
	conn.reset();
	// now try to fetch a chunk, this should not return a nullptr
	auto chunk = stream->Fetch();
	REQUIRE(chunk);

	// now close the entire database
	conn = make_uniq<Connection>(*db);
	stream = OpenStream(*conn, "SELECT 42");

	db.reset();
	// fetch should not fail
	chunk = stream->Fetch();
	REQUIRE(chunk);
	// new queries on the connection should not fail either, once the stream has ended
	REQUIRE(!stream->Fetch());
	REQUIRE_NO_FAIL(conn->Query("SELECT 42"));

	// a second stream is refused while the first is open
	db = make_uniq<DuckDB>(nullptr);
	conn = make_uniq<Connection>(*db);
	auto stream1 = OpenStream(*conn, "SELECT 42");
	REQUIRE(!stream1->HasError());
	auto refused = conn->Submit("SELECT 84");
	REQUIRE(refused->HasError());
	REQUIRE(refused->GetErrorType() == ExceptionType::RESOURCE_IN_USE);

	// stream1 is untouched
	REQUIRE(stream1->Fetch());
	stream1->Close();
	auto stream2 = OpenStream(*conn, "SELECT 84");
	REQUIRE(stream2->Fetch());
	stream2->Close();

	// a retained result stays readable after another query starts
	auto materialized = conn->Query("SELECT 42");
	auto result2 = conn->Query("SELECT 84");
	REQUIRE(CHECK_COLUMN(materialized, 0, {42}));
	REQUIRE(CHECK_COLUMN(result2, 0, {84}));
}

template <class T>
static void VerifyStreamResult(duckdb::unique_ptr<T> result) {
	REQUIRE(result->GetTypes()[0] == LogicalType::INTEGER);
	size_t current_row = 0;
	int current_expected_value = 0;
	size_t expected_rows = 500 * 5;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		auto col1_data = FlatVector::GetData<int>(chunk->data[0]);
		for (size_t k = 0; k < chunk->size(); k++) {
			if (current_row % 500 == 0) {
				current_expected_value++;
			}
			REQUIRE(col1_data[k] == current_expected_value);
			current_row++;
		}
	}
	REQUIRE(current_row == expected_rows);
}

TEST_CASE("Test fetch API with big results", "[api][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	// create table that consists of multiple chunks
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test(a INTEGER)"));
	for (size_t i = 0; i < 500; i++) {
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (1); INSERT INTO test VALUES (2); INSERT INTO test VALUES "
		                          "(3); INSERT INTO test VALUES (4); INSERT INTO test VALUES (5);"));
	}
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	// stream the results using the Fetch() API
	VerifyStreamResult(OpenStream(con, "SELECT CAST(a AS INTEGER) FROM test ORDER BY a"));
	// we can also cursor over a retained result
	VerifyStreamResult(con.Query("SELECT CAST(a AS INTEGER) FROM test ORDER BY a"));
	// return multiple results from one query
	auto result = con.Query("SELECT CAST(a AS INTEGER) FROM test ORDER BY a; SELECT CAST(a AS INTEGER) FROM test ORDER "
	                        "BY a; SELECT CAST(a AS INTEGER) FROM test ORDER BY a;");
	auto next = std::move(result->next);
	while (next) {
		auto nextnext = std::move(next->next);
		VerifyStreamResult(std::move(nextnext));
		next = std::move(nextnext);
	}
	VerifyStreamResult(std::move(result));
}

TEST_CASE("Test TryFlushCachingOperators interrupted ExecutePushInternal", "[api][.]") {
	DuckDB db;
	Connection con(db);

	con.Query("create table tbl as select 100000 a from range(2) t(a);");
	con.Query("pragma threads=1");

	// Use PhysicalCrossProduct with a very low amount of produced tuples, this caches the result in the
	// CachingOperatorState This gets flushed with FinalExecute in PipelineExecutor::TryFlushCachingOperator
	auto pending_query = con.Submit("select unnest(range(a.a)) from tbl a, tbl b;");

	// Through `unnest(range(a.a.))` this FinalExecute multiple chunks, more than the ExecutionBudget can handle with
	// PROCESS_PARTIAL
	pending_query->ExecuteTask();

	// query the connection as normal after
	auto &res = pending_query;
	res->Complete();
	REQUIRE(!res->HasError());
	auto &materialized_res = *res;
	idx_t initial_tuples = 2 * 2;
	REQUIRE(materialized_res.RowCount() == initial_tuples * 100000);
	auto rows = materialized_res.Collection().GetRows();
	for (idx_t i = 0; i < initial_tuples; i++) {
		for (idx_t j = 0; j < 100000; j++) {
			auto value = static_cast<idx_t>(rows.GetValue(0, (i * 100000) + j).GetValue<int64_t>());
			REQUIRE(value == j);
		}
	}
}

TEST_CASE("Test streaming query during stack unwinding", "[api]") {
	DuckDB db;
	Connection con(db);

	try {
		auto stream = OpenStream(con, "SELECT * FROM range(1000000)");

		throw std::runtime_error("hello");
	} catch (...) {
	}
}

TEST_CASE("Test prepare dependencies with multiple connections", "[catalog]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	auto con2 = make_uniq<Connection>(db);
	auto con3 = make_uniq<Connection>(db);

	// simple prepare: begin transaction before the second client calls PREPARE
	REQUIRE_NO_FAIL(con->Query("CREATE TABLE integers(i INTEGER)"));
	// open a transaction in con2, this forces the prepared statement to be kept around until this transaction is closed
	REQUIRE_NO_FAIL(con2->Query("BEGIN TRANSACTION"));
	// we prepare a statement in con
	REQUIRE_NO_FAIL(con->Query("PREPARE s1 AS SELECT * FROM integers"));
	// now we drop con while the second client still has an active transaction
	con.reset();
	// now commit the transaction in the second client
	REQUIRE_NO_FAIL(con2->Query("COMMIT"));

	con = make_uniq<Connection>(db);
	// three transactions
	// open a transaction in con2, this forces the prepared statement to be kept around until this transaction is closed
	REQUIRE_NO_FAIL(con2->Query("BEGIN TRANSACTION"));
	// create a prepare, this creates a dependency from s1 -> integers
	REQUIRE_NO_FAIL(con->Query("PREPARE s1 AS SELECT * FROM integers"));
	// drop the client
	con.reset();
	// now begin a transaction in con3
	REQUIRE_NO_FAIL(con3->Query("BEGIN TRANSACTION"));
	// drop the table integers with cascade, this should drop s1 as well
	REQUIRE_NO_FAIL(con3->Query("DROP TABLE integers CASCADE"));
	REQUIRE_NO_FAIL(con2->Query("COMMIT"));
	REQUIRE_NO_FAIL(con3->Query("COMMIT"));
}

TEST_CASE("Extract the plan of joins the optimizer rewrites", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a AS SELECT 1 AS id, DATE '2025-01-01' AS dt"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE r AS SELECT 1 AS id, DATE '2024-01-01' AS rd, NULL::DATE AS fd"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE p (pid BIGINT PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE knows (p1 BIGINT, p2 BIGINT, PRIMARY KEY (p1, p2))"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE interest (pid BIGINT, tag BIGINT, PRIMARY KEY (pid, tag))"));
	const vector<string> queries {
	    "SELECT a.id FROM a JOIN r ON r.id = a.id WHERE r.rd <= a.dt",
	    "SELECT a.id FROM a JOIN r ON r.id = a.id WHERE r.fd IS NULL OR r.fd > a.dt",
	    // an ANTI join that closes a join cycle
	    "SELECT count(i.tag) FROM p AS p1 JOIN knows AS k1 ON k1.p1 = p1.pid JOIN knows AS k2 ON k1.p2 = k2.p1 "
	    "JOIN p AS p3 ON k2.p2 = p3.pid JOIN interest AS i ON i.pid = p3.pid "
	    "ANTI JOIN knows AS k3 ON k3.p1 = p1.pid AND k3.p2 = p3.pid"};
	for (auto &query : queries) {
		auto plan = con.ExtractPlan(query);
		REQUIRE(plan);
		auto result = con.Query(query);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(plan->types == result->GetTypes());
	}
}

TEST_CASE("Test connection API", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// extract a plan node
	REQUIRE_NOTHROW(con.ExtractPlan("SELECT 42"));
	// can only extract one statement at a time
	REQUIRE_THROWS(con.ExtractPlan("SELECT 42; SELECT 84"));

	// append to a table
	con.Query("CREATE TABLE integers(i integer);");
	auto table_info = con.TableInfo("integers");

	// no transaction active
	REQUIRE_THROWS(con.Commit());
	REQUIRE_THROWS(con.Rollback());

	// cannot start a transaction within a transaction
	REQUIRE_NOTHROW(con.BeginTransaction());
	REQUIRE_THROWS(con.BeginTransaction());

	con.SetAutoCommit(false);
	REQUIRE(!con.IsAutoCommit());

	con.SetAutoCommit(true);
	REQUIRE(con.IsAutoCommit());
}

TEST_CASE("Test connection transaction API error scenarios", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));

	// a failed nested BEGIN does not abort the outer transaction
	REQUIRE_NOTHROW(con.BeginTransaction());
	REQUIRE_THROWS(con.BeginTransaction());
	REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (42)"));
	REQUIRE_NOTHROW(con.Commit());
	auto result = con.Query("SELECT i FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	// COMMIT on an invalidated transaction rolls back instead of throwing
	REQUIRE_NOTHROW(con.BeginTransaction());
	REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (84)"));
	ValidChecker::Invalidate(con.context->ActiveTransaction(), "test invalidation");
	REQUIRE_FAIL(con.Query("SELECT 42"));
	REQUIRE_NOTHROW(con.Commit());
	// the transaction was rolled back and the connection is usable again
	REQUIRE(con.IsAutoCommit());
	result = con.Query("SELECT count(*) FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// no transaction is active anymore - COMMIT and ROLLBACK throw again
	REQUIRE_THROWS(con.Commit());
	REQUIRE_THROWS(con.Rollback());
}

TEST_CASE("Test parser tokenize", "[api]") {
	string sql = "SELECT * FROM table WHERE i+1=3 AND j='hello'; --tokenize example query";
	auto tokens = Parser::Tokenize(sql);

	using T = SimplifiedTokenType;
	vector<pair<T, idx_t>> expected {
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 0},           // SELECT
	    {T::SIMPLIFIED_TOKEN_OPERATOR, 7},          // *
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 9},           // FROM
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 14},          // table
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 20},          // WHERE
	    {T::SIMPLIFIED_TOKEN_IDENTIFIER, 26},       // i
	    {T::SIMPLIFIED_TOKEN_OPERATOR, 27},         // +
	    {T::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT, 28}, // 1
	    {T::SIMPLIFIED_TOKEN_OPERATOR, 29},         // =
	    {T::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT, 30}, // 3
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 32},          // AND
	    {T::SIMPLIFIED_TOKEN_IDENTIFIER, 36},       // j
	    {T::SIMPLIFIED_TOKEN_OPERATOR, 37},         // =
	    {T::SIMPLIFIED_TOKEN_STRING_CONSTANT, 38},  // 'hello'
	    {T::SIMPLIFIED_TOKEN_OPERATOR, 45},         // ;
	    {T::SIMPLIFIED_TOKEN_COMMENT, 47},          // --tokenize example query
	};
	REQUIRE(tokens.size() >= expected.size());
	for (idx_t i = 0; i < expected.size(); i++) {
		REQUIRE(tokens[i].type == expected[i].first);
		REQUIRE(tokens[i].start == expected[i].second);
	}
	// The end-of-input sentinel is not a token: nothing starts at or past the input length.
	REQUIRE(tokens.back().start < sql.size());
	REQUIRE(tokens.size() == expected.size());
}

TEST_CASE("Test parser tokenize of words the grammar matches literally", "[api]") {
	// REPOSITORY, PREFIX, PUBLIC and KEYS are in none of the keyword lists, the grammar matches them literally;
	// highlighting shows them as keywords, while names and quoted identifiers stay identifiers
	string sql = "CREATE EXTENSION REPOSITORY r WITH PREFIX 'p' USING PUBLIC KEYS [\"prefix\"]";
	auto tokens = Parser::Tokenize(sql);

	using T = SimplifiedTokenType;
	vector<pair<T, idx_t>> expected {
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 0},          // CREATE
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 7},          // EXTENSION
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 17},         // REPOSITORY
	    {T::SIMPLIFIED_TOKEN_IDENTIFIER, 28},      // r
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 30},         // WITH
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 35},         // PREFIX
	    {T::SIMPLIFIED_TOKEN_STRING_CONSTANT, 42}, // 'p'
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 46},         // USING
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 52},         // PUBLIC
	    {T::SIMPLIFIED_TOKEN_KEYWORD, 59},         // KEYS
	    {T::SIMPLIFIED_TOKEN_OPERATOR, 64},        // [
	    {T::SIMPLIFIED_TOKEN_IDENTIFIER, 65},      // "prefix"
	};
	REQUIRE(tokens.size() >= expected.size());
	for (idx_t i = 0; i < expected.size(); i++) {
		REQUIRE(tokens[i].type == expected[i].first);
		REQUIRE(tokens[i].start == expected[i].second);
	}

	// the grammar also matches the quotes of quoted identifiers literally: an unfinished one stays an identifier
	for (auto &unfinished : {"\"", "`", "SELECT \""}) {
		auto unfinished_tokens = Parser::Tokenize(unfinished);
		REQUIRE(!unfinished_tokens.empty());
		REQUIRE(unfinished_tokens.back().type == T::SIMPLIFIED_TOKEN_IDENTIFIER);
	}
}

TEST_CASE("Test opening an invalid database file", "[api]") {
	duckdb::unique_ptr<DuckDB> db;
	bool success = false;
	try {
		db = make_uniq<DuckDB>("duckdb:data/parquet-testing/blob.parquet");
		success = true;
	} catch (std::exception &ex) {
		REQUIRE(StringUtil::Contains(ex.what(), "DuckDB"));
	}
	REQUIRE(!success);
	try {
		db = make_uniq<DuckDB>("duckdb:data/parquet-testing/h2oai/h2oai_group_small.parquet");
		success = true;
	} catch (std::exception &ex) {
		REQUIRE(StringUtil::Contains(ex.what(), "DuckDB"));
	}
	REQUIRE(!success);
}

TEST_CASE("Test opening a database with invalid metadata block index", "[api]") {
	auto path = TestCreatePath("invalid_metadata_block_index.db");
	TestDeleteFile(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT 42 AS i"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	vector<data_t> header_buffer(Storage::FILE_HEADER_SIZE);
	auto read_header = [&](std::fstream &file, idx_t location) {
		file.seekg(location);
		file.read(reinterpret_cast<char *>(header_buffer.data()), NumericCast<std::streamsize>(header_buffer.size()));
		REQUIRE(file.good());
		auto data = header_buffer.data() + Storage::DEFAULT_BLOCK_HEADER_SIZE;
		auto iteration = Load<uint64_t>(data);
		auto meta_block = Load<idx_t>(data + sizeof(uint64_t));
		return std::make_pair(iteration, meta_block);
	};

	std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
	REQUIRE(file.is_open());
	auto h1 = read_header(file, Storage::FILE_HEADER_SIZE);
	auto h2 = read_header(file, Storage::FILE_HEADER_SIZE * 2);
	auto active_header = h1.first > h2.first ? Storage::FILE_HEADER_SIZE : Storage::FILE_HEADER_SIZE * 2;
	auto active_meta_block = h1.first > h2.first ? h1.second : h2.second;

	file.seekg(active_header);
	file.read(reinterpret_cast<char *>(header_buffer.data()), NumericCast<std::streamsize>(header_buffer.size()));
	REQUIRE(file.good());
	auto data = header_buffer.data() + Storage::DEFAULT_BLOCK_HEADER_SIZE;
	auto invalid_meta_block =
	    (active_meta_block & ~(idx_t(0xFF) << 56ULL)) | (idx_t(MetadataManager::METADATA_BLOCK_COUNT) << 56ULL);
	Store<idx_t>(invalid_meta_block, data + sizeof(uint64_t));
	auto checksum = Checksum(data, Storage::FILE_HEADER_SIZE - Storage::DEFAULT_BLOCK_HEADER_SIZE);
	Store<uint64_t>(checksum, header_buffer.data());
	file.seekp(active_header);
	file.write(reinterpret_cast<const char *>(header_buffer.data()),
	           NumericCast<std::streamsize>(header_buffer.size()));
	REQUIRE(file.good());
	file.close();

	bool success = false;
	try {
		DuckDB db(path);
		success = true;
	} catch (std::exception &ex) {
		REQUIRE(StringUtil::Contains(ex.what(), "Metadata block index"));
	}
	REQUIRE(!success);
	TestDeleteFile(path);
}

TEST_CASE("Test large number of connections to a single database", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto context = make_uniq<ClientContext>((*db).instance);
	auto &connection_manager = ConnectionManager::Get(*context);

	duckdb::vector<duckdb::unique_ptr<Connection>> connections;
	size_t createdConnections = 5000;
	size_t remainingConnections = 500;
	size_t toRemove = createdConnections - remainingConnections;

	for (size_t i = 0; i < createdConnections; i++) {
		auto conn = make_uniq<Connection>(*db);
		connections.push_back(std::move(conn));
	}

	REQUIRE(connection_manager.GetConnectionCount() == createdConnections);

	for (size_t i = 0; i < toRemove; i++) {
		connections.erase(connections.begin());
	}

	REQUIRE(connection_manager.GetConnectionCount() == remainingConnections);
}

TEST_CASE("Issue #4583: Catch Insert/Update/Delete errors", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::unique_ptr<QueryResult> result;

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t0 (c0 int);"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t0 VALUES (1);"));

	result = con.Query(
	    "INSERT INTO t0(VALUES('\\x15\\x00\\x00\\x00\\x00@\\x01\\x0A\\x27:!\\x0A\\x00\\x00x12e\"\\x00'::BLOB));");
	//! Should not terminate the process
	REQUIRE_FAIL(result);

	result = con.Query("SELECT MIN(c0) FROM t0;");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}

TEST_CASE("Issue #14130: InsertStatement::ToString causes InternalException later on", "[api][.]") {
	auto db = DuckDB(nullptr);
	auto conn = Connection(db);

	conn.Query("CREATE TABLE foo(a int, b varchar, c int)");

	auto query = "INSERT INTO Foo values (1, 'qwerty', 42)";

	auto stmts = conn.ExtractStatements(query);
	auto &stmt = stmts[0];

	// Issue was here: calling ToString destroyed the 'alias' of the ValuesList
	stmt->ToString();
	// Which caused an 'InternalException: expected non-empty binding_name' here
	auto prepared_stmt = conn.Prepare(std::move(stmt));
	REQUIRE(!prepared_stmt->HasError());
	REQUIRE_NO_FAIL(prepared_stmt->Execute());
}

TEST_CASE("Issue #6284: CachingPhysicalOperator in pull causes issues", "[api][.]") {
	DBConfig config;
	config.options.maximum_threads = 8;
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("select setseed(0.1); CREATE TABLE T0 AS SELECT DISTINCT (RANDOM()*9999999)::BIGINT "
	                          "record_nb, 0.0 x_0, 1.0 y_0 FROM range(1000000) tbl"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE T1 AS SELECT record_nb, 0.0 x_1, 1.0 y_1 FROM T0"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE T2 AS SELECT record_nb, 0.0 x_2, 1.0 y_2 FROM T0"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE T3 AS SELECT record_nb, 0.0 x_3, 1.0 y_3 FROM T0"));
	auto stream = OpenStream(con, R"(
        SELECT T0.record_nb,
            T1.x_1 x_1,
            T1.y_1 y_1,
            T2.x_2 x_2,
            T2.y_2 y_2,
            T3.x_3 x_3,
            T3.y_3 y_3
         FROM T0
           INNER JOIN T1 on T0.record_nb = T1.record_nb
           INNER JOIN T2 on T0.record_nb = T2.record_nb
           INNER JOIN T3 on T0.record_nb = T3.record_nb
    )");

	idx_t count = 0;
	while (true) {
		auto chunk = stream->Fetch();
		if (!chunk) {
			break;
		}
		if (chunk->size() == 0) {
			break;
		}
		count += chunk->size();
	}

	REQUIRE(951382 == count);
}

TEST_CASE("Fuzzer 50 - Alter table heap-use-after-free", "[api]") {
	// FIXME: not fixed yet
	return;
	DuckDB db(nullptr);
	Connection con(db);

	con.Query("CREATE TABLE t0(c0 INT);");
	con.Query("ALTER TABLE t0 ADD c1 TIMESTAMP_SEC;");
}

TEST_CASE("Test loading database with enable_external_access set to false", "[api]") {
	DBConfig config;
	config.SetOptionByName("enable_external_access", false);
	auto path = TestCreatePath("external_access_test.db");
	DuckDB db(path, &config);
	Connection con(db);

	REQUIRE_FAIL(con.Query("ATTACH 'mydb.db'"));
}

TEST_CASE("Test checkpointing initial database with enable_external_access set to false", "[api]") {
	DBConfig config;
	config.SetOptionByName("enable_external_access", false);
	auto path = TestCreatePath("external_access_test.db");
	DuckDB db(path, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
}

TEST_CASE("Test insert returning in CPP API", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.Query("CREATE TABLE test(val VARCHAR);");

	con.Query("INSERT INTO test(val) VALUES ('query_1')");
	auto res = con.Query("INSERT INTO test(val) VALUES ('query_2') returning *");
	REQUIRE(CHECK_COLUMN(res, 0, {"query_2"}));

	con.Query("INSERT INTO test(val) VALUES (?);", "query_arg_1");
	auto returning_args = con.Query("INSERT INTO test(val) VALUES (?) RETURNING *;", "query_arg_2");
	REQUIRE(CHECK_COLUMN(returning_args, 0, {"query_arg_2"}));

	con.Prepare("INSERT INTO test(val) VALUES (?);")->Execute("prepared_arg_1");
	auto prepared_returning_args =
	    con.Prepare("INSERT INTO test(val) VALUES (?) returning *;")->Execute("prepared_arg_2");
	REQUIRE(CHECK_COLUMN(prepared_returning_args, 0, {"prepared_arg_2"}));

	// make sure all inserts actually inserted
	auto result = con.Query("SELECT * from test;");
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"query_1", "query_2", "query_arg_1", "query_arg_2", "prepared_arg_1", "prepared_arg_2"}));
}

TEST_CASE("Test a logical execute still has types after an optimization pass", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.Query("PREPARE test AS SELECT 42::INTEGER;");
	const auto query_plan = con.ExtractPlan("EXECUTE test");
	REQUIRE((query_plan->type == LogicalOperatorType::LOGICAL_EXECUTE));
	REQUIRE((query_plan->types.size() == 1));
	REQUIRE((query_plan->types[0].id() == LogicalTypeId::INTEGER));
}

static bool HasFlippedDelimJoin(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN && op.Cast<LogicalComparisonJoin>().delim_flipped) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasFlippedDelimJoin(*child)) {
			return true;
		}
	}
	return false;
}

TEST_CASE("Test executing an optimized plan with a flipped delim join", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (1), (2), (3), (NULL)"));

	REQUIRE_NO_FAIL(con.Query("SET delim_join_as_cte = false"));
	con.BeginTransaction();
	Parser parser(*con.context);
	parser.ParseQuery(
	    "SELECT a.i, b.i, x FROM integers a, integers b JOIN LATERAL (VALUES (a.i)) ss(x) ON (true) ORDER BY ALL");
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *con.context);
	auto plan = optimizer.Optimize(std::move(planner.plan));
	con.Commit();
	REQUIRE(HasFlippedDelimJoin(*plan));
	REQUIRE_NO_FAIL(con.Query("SET delim_join_as_cte = true"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_optimizer"));
	auto result = con.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 16);
}

TEST_CASE("Test SqlStatement::ToString for UPDATE, INSERT, DELETE statements with alias of RETURNING clause", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	std::string sql;
	con.Query("CREATE TABLE test(id INT);");

	sql = "INSERT INTO test (id) VALUES (1) RETURNING id AS inserted";
	auto stmts = con.ExtractStatements(sql);
	REQUIRE(stmts[0]->ToString() == "INSERT INTO test (id) (VALUES (1)) RETURNING id AS inserted");

	sql = "UPDATE test SET id = 1 RETURNING id AS updated";
	stmts = con.ExtractStatements(sql);
	REQUIRE(stmts[0]->ToString() == sql);

	sql = "DELETE FROM test WHERE (id = 1) RETURNING id AS deleted";
	stmts = con.ExtractStatements(sql);
	REQUIRE(stmts[0]->ToString() == sql);
}

TEST_CASE("Test buffer managed query result", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto con = make_uniq<Connection>(*db);
	auto close_database = [&]() {
		con.reset();
		db.reset();
	};
	auto reopen_database = [&]() {
		db = make_uniq<DuckDB>(nullptr);
		con = make_uniq<Connection>(*db);
	};

	// An in-memory result outlives the database
	QueryParameters parameters;
	parameters.format = ChunkFormat::InMemory();
	auto result = con->context->Query("SELECT 42;", parameters);
	REQUIRE_NOTHROW(result->ToString());
	close_database();
	REQUIRE_NOTHROW(result->ToString());

	// A buffer-managed result dies with the database
	reopen_database();
	parameters.format = ChunkFormat::BufferManaged();
	result = con->context->Query("SELECT 42;", parameters);
	REQUIRE_NOTHROW(result->ToString());
	close_database();
	REQUIRE_THROWS(result->ToString());

	reopen_database();
	REQUIRE_NO_FAIL(con->Query("SET preserve_insertion_order=false;"));
	result = con->context->Query("SELECT 42;", parameters);
	REQUIRE_NOTHROW(result->ToString());
	close_database();
	REQUIRE_THROWS(result->ToString());

	reopen_database();
	REQUIRE_NO_FAIL(con->Query("CREATE TABLE integers AS SELECT range AS i FROM range(10000);"));
	result = con->context->Query("SELECT * FROM integers;", parameters);
	REQUIRE(result->RowCount() == 10000);
	close_database();
	REQUIRE_THROWS(result->ToString());

	reopen_database();
	result = con->Submit("SELECT 42;", ChunkFormat::BufferManaged());
	result->Materialize();
	REQUIRE_NOTHROW(result->ToString());
	close_database();
	REQUIRE_THROWS(result->ToString());

	// Through the statement overload the shell uses
	reopen_database();
	auto statements = con->ExtractStatements("SELECT 42;");
	result = con->Query(std::move(statements[0]), ChunkFormat::BufferManaged());
	REQUIRE_NOTHROW(result->ToString());
	close_database();
	REQUIRE_THROWS(result->ToString());

	// The parameterized Query template keeps the in-memory default of the other Query forms
	reopen_database();
	result = con->Query("SELECT ?::INTEGER;", 42);
	REQUIRE_NOTHROW(result->ToString());
	close_database();
	REQUIRE_NOTHROW(result->ToString());
}

TEST_CASE("Test ClientInterruptState suppresses interrupts after irreversible operations", "[api]") {
	// Verify the three-state interrupt mechanism that prevents a completed COMMIT
	// from being incorrectly reported as failed due to a late Interrupt() call.

	DuckDB db;
	Connection con(db);
	auto &context = *con.context;

	SECTION("Normal interrupt works") {
		context.Interrupt();
		REQUIRE(context.IsInterrupted());
		REQUIRE_THROWS(context.InterruptCheck());
		context.ClearInterrupt();
		REQUIRE(!context.IsInterrupted());
		REQUIRE_NOTHROW(context.InterruptCheck());
	}

	SECTION("SuppressInterrupts blocks subsequent Interrupt calls") {
		context.SuppressInterrupts();
		// Interrupt() uses CAS: NOT_INTERRUPTED -> INTERRUPTED
		// Since state is SUPPRESSED, CAS fails and interrupt is discarded
		context.Interrupt();
		REQUIRE(!context.IsInterrupted());
		REQUIRE_NOTHROW(context.InterruptCheck());
		context.ClearInterrupt();
	}

	SECTION("SuppressInterrupts overrides a pending interrupt") {
		context.Interrupt();
		REQUIRE(context.IsInterrupted());
		// SuppressInterrupts unconditionally stores SUPPRESSED,
		// overriding the INTERRUPTED state
		context.SuppressInterrupts();
		REQUIRE(!context.IsInterrupted());
		REQUIRE_NOTHROW(context.InterruptCheck());
		context.ClearInterrupt();
	}

	SECTION("ClearInterrupt resets from SUPPRESSED to allow future interrupts") {
		context.SuppressInterrupts();
		context.ClearInterrupt();
		// Now back to NOT_INTERRUPTED, Interrupt() should work again
		context.Interrupt();
		REQUIRE(context.IsInterrupted());
		REQUIRE_THROWS(context.InterruptCheck());
		context.ClearInterrupt();
	}

	SECTION("End-to-end: COMMIT suppresses interrupts") {
		REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE suppress_test (x INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		// After COMMIT, state should be SUPPRESSED — Interrupt() should be discarded
		context.Interrupt();
		REQUIRE(!context.IsInterrupted());
		REQUIRE_NOTHROW(context.InterruptCheck());
		// Cleanup
		context.ClearInterrupt();
		REQUIRE_NO_FAIL(con.Query("DROP TABLE suppress_test"));
	}
}

TEST_CASE("Test internal connections follow the interrupts of their base client", "[api]") {
	DuckDB db;
	Connection con(db);
	auto child = Connection::CreateInternal(*con.context);
	auto grandchild = Connection::CreateInternal(*child.context);
	REQUIRE(grandchild.context->base_context == con.context);
	REQUIRE(grandchild.context->internal_depth == 2);

	SECTION("Interrupting the base interrupts every internal connection") {
		con.context->Interrupt();
		REQUIRE(child.context->IsInterrupted());
		REQUIRE(grandchild.context->IsInterrupted());
		REQUIRE_THROWS(grandchild.context->InterruptCheck());
		con.context->ClearInterrupt();
		REQUIRE(!grandchild.context->IsInterrupted());
	}

	SECTION("Interrupting an internal connection does not interrupt the base") {
		child.context->Interrupt();
		REQUIRE(!con.context->IsInterrupted());
		child.context->ClearInterrupt();
	}

	SECTION("The base client's query deadline applies to internal connections while it is set") {
		REQUIRE_NOTHROW(grandchild.context->BlockingInterruptCheck());
		// a deadline in the past, as a base query that ran out of time would leave it
		con.context->query_deadline = 1;
		REQUIRE_THROWS(grandchild.context->BlockingInterruptCheck());
		REQUIRE_THROWS(child.context->BlockingInterruptCheck());
		con.context->query_deadline.SetInvalid();
		REQUIRE_NOTHROW(grandchild.context->BlockingInterruptCheck());
	}

	SECTION("A query running on behalf of the base stops when the base is interrupted") {
		REQUIRE_NO_FAIL(
		    con.Query("CREATE MACRO slow_deleter(h) AS TABLE SELECT sum(i) AS s FROM range(100000000000) t(i)"));
		auto start = std::chrono::steady_clock::now();
		std::thread interrupter([&]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			con.Interrupt();
		});
		auto result = con.Query("FROM destroy_external_resource('slow_deleter', 1)");
		interrupter.join();
		REQUIRE(result->HasError());
		REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
	}
}

TEST_CASE("Test ExtractStatements error location", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_THROWS_WITH(con.ExtractStatements("FROM (VALUES ('1', '2'), ('1'))"),
	                    Catch::Matchers::Contains("LINE 1: FROM (VALUES ('1', '2'), ('1'))"));
}

TEST_CASE("Test a parser error in Submit invalidates the transaction", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("SET current_transaction_invalidation_policy='ALL_ERRORS_INVALIDATE_TRANSACTION'"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (21)"));
	auto result = con.Submit("inser into audit_backup from audit");
	REQUIRE(result->HasError());
	REQUIRE_FAIL(con.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(con.Query("ABORT"));

	result = con.Query("SELECT COUNT(*) FROM audit");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("Test a parser error while iterating statements invalidates the transaction", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	auto iterator = con.context->IterateStatements("INSERT INTO audit VALUES (21); inser into audit_backup from audit");
	REQUIRE(iterator.Peek());
	REQUIRE_NO_FAIL(con.Query(iterator.GetStatementForExecution()));
	REQUIRE_THROWS_WITH(iterator.Peek(), Catch::Matchers::Contains("syntax error"));
	REQUIRE_FAIL(con.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(con.Query("ABORT"));

	auto result = con.Query("SELECT COUNT(*) FROM audit");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("Test invalid UTF-8 while iterating statements invalidates the transaction", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (21)"));
	REQUIRE_THROWS_WITH(con.context->IterateStatements("SELECT '\xff'"), Catch::Matchers::Contains("Invalid UTF-8"));
	REQUIRE_FAIL(con.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(con.Query("ABORT"));

	auto result = con.Query("SELECT COUNT(*) FROM audit");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("Test iterating statements without applying the invalidation policy", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (21)"));
	auto iterator = con.context->IterateStatements("inser into audit_backup from audit", false);
	REQUIRE_THROWS_WITH(iterator.Peek(), Catch::Matchers::Contains("syntax error"));
	REQUIRE_THROWS_WITH(con.context->IterateStatements("SELECT '\xff'", false),
	                    Catch::Matchers::Contains("Invalid UTF-8"));
	REQUIRE_THROWS_WITH(con.ExtractStatements("inser into audit_backup from audit", false),
	                    Catch::Matchers::Contains("syntax error"));
	REQUIRE_THROWS_WITH(con.GetTableNames("inser into audit_backup from audit"),
	                    Catch::Matchers::Contains("syntax error"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	auto result = con.Query("SELECT COUNT(*) FROM audit");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
}

TEST_CASE("Test a parser error while iterating statements follows the invalidation policy", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("SET current_transaction_invalidation_policy='SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (21)"));
	auto iterator = con.context->IterateStatements("inser into audit_backup from audit");
	REQUIRE_THROWS_WITH(iterator.Peek(), Catch::Matchers::Contains("syntax error"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	auto result = con.Query("SELECT COUNT(*) FROM audit");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
}

TEST_CASE("Test a parser error while iterating statements in autocommit mode", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit(i INT)"));

	auto iterator = con.context->IterateStatements("inser into audit_backup from audit");
	REQUIRE_THROWS_WITH(iterator.Peek(), Catch::Matchers::Contains("syntax error"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO audit VALUES (21)"));

	auto result = con.Query("SELECT COUNT(*) FROM audit");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}
