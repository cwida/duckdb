#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: PreparedStatement. Prepare once and execute many, the
// honest reuse report, and the lifetimes the handle promises.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collects one BIGINT column. STANDARD_VECTOR_SIZE can be 2 in the assertion build, so
// this must not assume a single chunk.
std::vector<int64_t> CollectPreparedBigints(QueryResult result) {
	std::vector<int64_t> rows;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			rows.push_back(view.Data<int64_t>()[view.SelAt(i)]);
		}
	}
	return rows;
}

} // namespace

TEST_CASE("Stable C++API: PreparedStatement executes repeatedly", "[cpp_api][prepared_statement]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE t(x BIGINT)").Drain();
	conn.Execute("INSERT INTO t VALUES (1), (2), (3), (4)").Drain();

	// Value is move-only, so a parameter list is built by move rather than brace-init.
	auto Params = [&factory](std::initializer_list<int64_t> values) {
		std::vector<Value> params;
		for (auto value : values) {
			params.push_back(Value::Create(factory, int64_t(value)));
		}
		return params;
	};

	auto iter = conn.ParseSQL("SELECT x FROM t WHERE x > $1 ORDER BY x");
	auto stmt = iter.Next();
	auto prepared = conn.Prepare(stmt);

	// The statement was borrowed, not consumed.
	REQUIRE(static_cast<bool>(stmt));

	REQUIRE(CollectPreparedBigints(prepared.Execute(Params({0}))) == std::vector<int64_t> {1, 2, 3, 4});
	REQUIRE(CollectPreparedBigints(prepared.Execute(Params({2}))) == std::vector<int64_t> {3, 4});
	REQUIRE(CollectPreparedBigints(prepared.Execute(Params({0}))) == std::vector<int64_t> {1, 2, 3, 4});
}

TEST_CASE("Stable C++API: PreparedStatement binds named parameters", "[cpp_api][prepared_statement]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto iter = conn.ParseSQL("SELECT $a - $b");
	auto stmt = iter.Next();
	auto prepared = conn.Prepare(stmt);

	std::vector<NamedParam> params;
	params.push_back({"a", Value::Create(factory, int64_t(10))});
	params.push_back({"b", Value::Create(factory, int64_t(4))});
	REQUIRE(CollectPreparedBigints(prepared.Execute(params)) == std::vector<int64_t> {6});

	// Keyed by name, so swapping the entries changes nothing.
	std::vector<NamedParam> swapped;
	swapped.push_back({"b", Value::Create(factory, int64_t(4))});
	swapped.push_back({"a", Value::Create(factory, int64_t(10))});
	REQUIRE(CollectPreparedBigints(prepared.Execute(swapped)) == std::vector<int64_t> {6});
}

TEST_CASE("Stable C++API: PreparedStatement reports plan reuse", "[cpp_api][prepared_statement]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE t(x BIGINT)").Drain();

	auto Prepare = [&conn](const char *sql, bool require_cacheable = false) {
		auto iter = conn.ParseSQL(sql);
		auto stmt = iter.Next();
		return conn.Prepare(stmt, require_cacheable);
	};

	REQUIRE(Prepare("SELECT 42").ReusesPlan());
	REQUIRE(Prepare("SELECT $1::BIGINT + 1").ReusesPlan());
	// A table scan re-binds each execution so a catalog change is picked up.
	REQUIRE_FALSE(Prepare("SELECT x FROM t WHERE x = $1").ReusesPlan());

	// require_cacheable turns that into a failure at prepare time rather than a silent
	// slow path.
	REQUIRE_NOTHROW(Prepare("SELECT $1::BIGINT + 1", true));
	REQUIRE_THROWS_MATCHES(Prepare("SELECT x FROM t WHERE x = $1", true), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: PreparedStatement lifetimes", "[cpp_api][prepared_statement]") {
	auto db = Instance(":memory:");

	SECTION("a result outlives the statement that made it") {
		auto conn = db.Connect();
		conn.Execute("CREATE TABLE t(x BIGINT)").Drain();
		conn.Execute("INSERT INTO t VALUES (1), (2), (3), (4)").Drain();

		QueryResult result = [&]() {
			auto iter = conn.ParseSQL("SELECT x FROM t ORDER BY x");
			auto stmt = iter.Next();
			auto prepared = conn.Prepare(stmt);
			return prepared.Execute();
		}();
		REQUIRE(CollectPreparedBigints(std::move(result)) == std::vector<int64_t> {1, 2, 3, 4});
	}

	SECTION("the statement outlives its connection") {
		auto reader = db.Connect();
		reader.Execute("CREATE TABLE t(x BIGINT)").Drain();
		reader.Execute("INSERT INTO t VALUES (1), (2), (3), (4)").Drain();

		PreparedStatement prepared = [&]() {
			auto conn = db.Connect();
			auto iter = conn.ParseSQL("SELECT x FROM t ORDER BY x");
			auto stmt = iter.Next();
			return conn.Prepare(stmt);
		}();
		// The handle kept the session alive, the same guarantee an undrained result carries.
		REQUIRE(CollectPreparedBigints(prepared.Execute()) == std::vector<int64_t> {1, 2, 3, 4});
	}
}

TEST_CASE("Stable C++API: PreparedStatement error paths", "[cpp_api][prepared_statement]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	SECTION("a prepare-time catalog error throws") {
		auto iter = conn.ParseSQL("SELECT * FROM no_such_table");
		auto stmt = iter.Next();
		REQUIRE_THROWS_MATCHES(conn.Prepare(stmt), Exception, HasErrorCode(DUCKDB_V2_ERROR_DATABASE_CATALOG));
		REQUIRE(static_cast<bool>(stmt)); // intact: only a copy was prepared
	}

	SECTION("a live result blocks preparing and executing") {
		auto iter = conn.ParseSQL("SELECT 1::BIGINT");
		auto stmt = iter.Next();
		auto prepared = conn.Prepare(stmt);

		{
			auto live = conn.Execute("SELECT i FROM range(100000) t(i)");
			REQUIRE_THROWS_MATCHES(conn.Prepare(stmt), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
			REQUIRE_THROWS_MATCHES(prepared.Execute(), Exception, HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_IN_USE));
		}
		// The live result is gone, so the connection is free for both paths again.
		REQUIRE(CollectPreparedBigints(prepared.Execute()) == std::vector<int64_t> {1});
	}
}
