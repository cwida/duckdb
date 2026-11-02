#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: Appender. A ColumnDataCollection plus a statement that
// reads it, bound to the statement with SqlStatement::AddCollection.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collect a single BIGINT column, asserting every row valid.
std::vector<int64_t> CollectAppended(QueryResult result) {
	std::vector<int64_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int64_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

// Buffers one chunk of BIGINTs through the appender.
void AppendValues(Connection &conn, Appender &appender, const std::vector<int64_t> &values) {
	auto &factory = conn.GetFactory();
	DataChunk chunk(factory, appender.ColumnTypes());
	auto vec = chunk.GetVector(0);
	auto *data = vec.GetDataMutable<int64_t>();
	for (size_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
	vec.SetSize(values.size());
	appender.AppendChunk(chunk);
}

// A single BIGINT column.
std::vector<LogicalType> BigintTypes(Factory &factory) {
	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	return types;
}

} // namespace

TEST_CASE("Stable C++API: appender buffers and flushes into a table", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE t (v BIGINT)").Drain();

	Appender appender(conn, "t");
	REQUIRE(appender.ColumnTypes().size() == 1);

	// Buffering touches nothing until the flush.
	AppendValues(conn, appender, {1, 2});
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t")) == std::vector<int64_t> {0});

	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT v FROM t ORDER BY v")) == std::vector<int64_t> {1, 2});

	// The buffer is empty and reusable afterwards, and a flush of nothing is a no-op.
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t")) == std::vector<int64_t> {2});

	// Several batches in a row.
	for (int64_t round = 0; round < 3; round++) {
		AppendValues(conn, appender, {10 + round, 20 + round});
		appender.Flush();
	}
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t")) == std::vector<int64_t> {8});
	REQUIRE(CollectAppended(conn.Execute("SELECT sum(v)::BIGINT FROM t")) == std::vector<int64_t> {3 + 33 + 63});
}

TEST_CASE("Stable C++API: appender buffers across several chunks before flushing", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE t (v BIGINT)").Drain();

	Appender appender(conn, "t");
	for (int64_t i = 0; i < 5; i++) {
		AppendValues(conn, appender, {i, i + 100});
	}
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t")) == std::vector<int64_t> {10});
	REQUIRE(CollectAppended(conn.Execute("SELECT v FROM t ORDER BY v LIMIT 2")) == std::vector<int64_t> {0, 1});
}

TEST_CASE("Stable C++API: appender Clear drops the buffer without writing", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE t (v BIGINT)").Drain();

	Appender appender(conn, "t");
	AppendValues(conn, appender, {1, 2});
	appender.Clear();
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t")) == std::vector<int64_t> {0});

	// Usable again after a clear.
	AppendValues(conn, appender, {7, 8});
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT v FROM t ORDER BY v")) == std::vector<int64_t> {7, 8});
}

TEST_CASE("Stable C++API: appender destruction drops unflushed rows", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("CREATE TABLE t (v BIGINT)").Drain();
	{
		Appender appender(conn, "t");
		AppendValues(conn, appender, {1, 2});
	}
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t")) == std::vector<int64_t> {0});
}

TEST_CASE("Stable C++API: appender with an explicit query", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE t (v BIGINT, tag VARCHAR DEFAULT 'seen')").Drain();

	// A subset of columns, which is what the table constructor cannot express.
	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	Appender appender(conn, "INSERT INTO t (v) SELECT amount FROM my_rows", std::move(types), "my_rows", {"amount"});

	AppendValues(conn, appender, {5, 6});
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT v FROM t ORDER BY v")) == std::vector<int64_t> {5, 6});
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t WHERE tag = 'seen'")) ==
	        std::vector<int64_t> {2});

	// The buffer is a table like any other, so it can drive a read too.
	std::vector<LogicalType> read_types;
	read_types.push_back(factory.ParseType("BIGINT"));
	Appender reader(conn, "SELECT amount FROM other_rows", std::move(read_types), "other_rows", {"amount"});
	AppendValues(conn, reader, {9, 9});
	// Flushing a SELECT just drains it; the rows are consumed either way.
	reader.Flush();
}

TEST_CASE("Stable C++API: appender buffer is only visible to its statement", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE t (v BIGINT)").Drain();

	Appender appender(conn, "INSERT INTO t SELECT * FROM scoped_rows", BigintTypes(factory), "scoped_rows");
	REQUIRE_THROWS_AS(conn.Execute("SELECT * FROM scoped_rows").Drain(), Exception);

	// So two appenders on one connection can share a buffer name.
	Appender twin(conn, "INSERT INTO t SELECT * FROM scoped_rows", BigintTypes(factory), "scoped_rows");
	AppendValues(conn, appender, {1});
	AppendValues(conn, twin, {2, 3});
	twin.Flush();
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT v FROM t ORDER BY v")) == std::vector<int64_t> {1, 2, 3});

	// The buffer shadows a table of the same name.
	conn.Execute("CREATE TABLE shadowed (v BIGINT)").Drain();
	Appender shadowing(conn, "INSERT INTO t SELECT * FROM shadowed", BigintTypes(factory), "shadowed");
	AppendValues(conn, shadowing, {4});
	shadowing.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT count(*)::BIGINT FROM t WHERE v = 4")) == std::vector<int64_t> {1});
}

TEST_CASE("Stable C++API: appender refuses a mismatching chunk", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	conn.Execute("CREATE TABLE t (v BIGINT)").Drain();

	Appender appender(conn, "t");

	std::vector<LogicalType> wrong;
	wrong.push_back(factory.ParseType("VARCHAR"));
	DataChunk chunk(factory, wrong);
	chunk.GetVector(0).SetSize(0);
	REQUIRE_THROWS_MATCHES(appender.AppendChunk(chunk), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// A refused chunk leaves the appender usable.
	AppendValues(conn, appender, {3});
	appender.Flush();
	REQUIRE(CollectAppended(conn.Execute("SELECT v FROM t")) == std::vector<int64_t> {3});
}

TEST_CASE("Stable C++API: appender construction refusals", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	// No columns.
	REQUIRE_THROWS_MATCHES(Appender(conn, "SELECT 1", {}, "buf"), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	// More than one statement.
	REQUIRE_THROWS_MATCHES(Appender(conn, "SELECT 1; SELECT 2", BigintTypes(factory), "buf"), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	// A statement that cannot read a buffer.
	REQUIRE_THROWS_MATCHES(Appender(conn, "CREATE TABLE c AS SELECT * FROM buf", BigintTypes(factory), "buf"),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	// A query defining the buffer's name itself.
	REQUIRE_THROWS_MATCHES(Appender(conn, "WITH buf AS (SELECT 1) SELECT * FROM buf", BigintTypes(factory), "buf"),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	// A table that does not exist.
	REQUIRE_THROWS_AS(Appender(conn, "no_such_table"), Exception);
}
