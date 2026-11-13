#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

namespace {

//! Pauses the ALTER's rewrite until released
struct RewriteGate {
	mutex lock;
	std::condition_variable cv;
	bool armed = false;
	bool parked = false;
	bool released = false;

	void Arm() {
		lock_guard<mutex> guard(lock);
		armed = true;
		parked = false;
		released = false;
	}
	void Park() {
		unique_lock<mutex> guard(lock);
		if (!armed) {
			return;
		}
		armed = false;
		parked = true;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}
	bool WaitUntilParked() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(60), [&]() { return parked; });
	}
	void Release() {
		{
			lock_guard<mutex> guard(lock);
			released = true;
		}
		cv.notify_all();
	}
};

RewriteGate gate;

void GateExec(DataChunk &args, ExpressionState &state, Vector &result) {
	gate.Park();
	result.Reference(args.data[0]);
}

//! Commits the statement while an ALTER TYPE rewrites column d
unique_ptr<QueryResult> CommitDuringAlterType(DuckDB &db, const string &statement) {
	Connection con(db);
	Connection writer(db);
	Connection alterer(db);

	ScalarFunction fn("rewrite_gate", {LogicalType::INTEGER}, LogicalType::INTEGER, GateExec);
	fn.SetStability(FunctionStability::VOLATILE);
	CreateScalarFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, k INTEGER, d INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i, 0, 0 FROM range(10) r(i)"));

	REQUIRE_NO_FAIL(writer.Query("BEGIN"));
	REQUIRE_NO_FAIL(writer.Query(statement));

	gate.Arm();
	unique_ptr<QueryResult> alter_result;
	std::thread alter_thread(
	    [&]() { alter_result = alterer.Query("ALTER TABLE t ALTER COLUMN d TYPE BIGINT USING rewrite_gate(d)"); });
	bool parked = gate.WaitUntilParked();
	auto commit_result = writer.Query("COMMIT");
	gate.Release();
	alter_thread.join();
	REQUIRE(parked);
	REQUIRE_NO_FAIL(*alter_result);
	return commit_result;
}

} // namespace

TEST_CASE("An update committing during an ALTER TYPE rewrite conflicts", "[api][alter]") {
	DuckDB db(nullptr);
	auto commit_result = CommitDuringAlterType(db, "UPDATE t SET k = k + 1, d = d + 1 WHERE id = 5");
	REQUIRE(commit_result->HasError());
	REQUIRE(StringUtil::Contains(commit_result->GetError(), "another transaction has altered this table"));

	Connection con(db);
	auto result = con.Query("SELECT k, d, typeof(d) FROM t WHERE id = 5");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(CHECK_COLUMN(result, 1, {0}));
	REQUIRE(CHECK_COLUMN(result, 2, {"BIGINT"}));
}

TEST_CASE("A delete committing during an ALTER TYPE rewrite is kept", "[api][alter]") {
	DuckDB db(nullptr);
	auto commit_result = CommitDuringAlterType(db, "DELETE FROM t WHERE id = 5");
	REQUIRE_NO_FAIL(*commit_result);

	Connection con(db);
	auto result = con.Query("SELECT count(*), count(*) FILTER (id = 5) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {9}));
	REQUIRE(CHECK_COLUMN(result, 1, {0}));
}
