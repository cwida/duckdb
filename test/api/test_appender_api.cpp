#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/common/types/hugeint.hpp"

using namespace duckdb;

template <class SRC>
void TestAppendingSingleDecimalValue(SRC value, Value expected_result, uint8_t width, uint8_t scale) {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);
	duckdb::unique_ptr<Appender> appender;
	duckdb::unique_ptr<QueryResult> result;
	REQUIRE_NO_FAIL(conn->Query(StringUtil::Format("CREATE TABLE decimals(i DECIMAL(%d,%d))", width, scale)));
	appender = make_uniq<Appender>(*conn, "decimals");

	appender->BeginRow();
	appender->Append<SRC>(value);
	appender->EndRow();

	appender->Flush();

	result = conn->Query("SELECT * FROM decimals");
	REQUIRE(CHECK_COLUMN(result, 0, {expected_result}));
}

TEST_CASE("Test appending to a decimal column", "[api]") {
	TestAppendingSingleDecimalValue<int32_t>(1, Value::DECIMAL(1000, 4, 3), 4, 3);
	TestAppendingSingleDecimalValue<int16_t>(-9999, Value::DECIMAL(-9999, 4, 0), 4, 0);
	TestAppendingSingleDecimalValue<int16_t>(9999, Value::DECIMAL(9999, 4, 0), 4, 0);
	TestAppendingSingleDecimalValue<int32_t>(99999999, Value::DECIMAL(99999999, 8, 0), 8, 0);
	TestAppendingSingleDecimalValue<const char *>("1.234", Value::DECIMAL(1234, 4, 3), 4, 3);
	TestAppendingSingleDecimalValue<const char *>("123.4", Value::DECIMAL(1234, 4, 1), 4, 1);
	hugeint_t hugeint_value;
	bool result;
	result = Hugeint::TryConvert<const char *>("3245234123123", hugeint_value);
	REQUIRE(result);
	TestAppendingSingleDecimalValue<const char *>("3245234.123123", Value::DECIMAL(hugeint_value, 19, 6), 19, 6);
	int64_t bigint_reference_value = 3245234123123;
	TestAppendingSingleDecimalValue<const char *>("3245234.123123", Value::DECIMAL(bigint_reference_value, 13, 6), 13,
	                                              6);
	TestAppendingSingleDecimalValue<float>(12.3124324f, Value::DECIMAL(123124323, 9, 7), 9, 7);

	// Precision loss
	result = Hugeint::TryConvert<const char *>("12345234234312432287744000", hugeint_value);
	REQUIRE(result);
	TestAppendingSingleDecimalValue<double>(12345234234.31243244234324, Value::DECIMAL(hugeint_value, 26, 15), 26, 15);
}

TEST_CASE("Test using appender after connection is gone", "[api]") {
	auto db = make_uniq<DuckDB>(nullptr);
	auto conn = make_uniq<Connection>(*db);
	duckdb::unique_ptr<Appender> appender;
	duckdb::unique_ptr<QueryResult> result;
	// create an appender for a non-existing table fails
	REQUIRE_THROWS(make_uniq<Appender>(*conn, "integers"));
	// now create the table and create the appender
	REQUIRE_NO_FAIL(conn->Query("CREATE TABLE integers(i INTEGER)"));
	appender = make_uniq<Appender>(*conn, "integers");

	// we can use the appender
	appender->BeginRow();
	appender->Append<int32_t>(1);
	appender->EndRow();

	appender->Flush();

	result = conn->Query("SELECT * FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// removing the connection invalidates the appender
	conn.reset();
	appender->BeginRow();
	appender->Append<int32_t>(2);
	appender->EndRow();

	// the connection is gone
	REQUIRE_THROWS(appender->Flush());
}

TEST_CASE("Test appender and connection destruction order", "[api]") {
	for (idx_t i = 0; i < 6; i++) {
		auto db = make_uniq<DuckDB>(nullptr);
		auto con = make_uniq<Connection>(*db);
		REQUIRE_NO_FAIL(con->Query("CREATE TABLE integers(i INTEGER)"));
		auto appender = make_uniq<Appender>(*con, "integers");

		switch (i) {
		case 0:
			// db - con - appender
			db.reset();
			con.reset();
			appender.reset();
			break;
		case 1:
			// db - appender - con
			db.reset();
			appender.reset();
			con.reset();
			break;
		case 2:
			// con - db - appender
			con.reset();
			db.reset();
			appender.reset();
			break;
		case 3:
			// con - appender - db
			con.reset();
			appender.reset();
			db.reset();
			break;
		case 4:
			// appender - con - db
			appender.reset();
			con.reset();
			db.reset();
			break;
		default:
			// appender - db - con
			appender.reset();
			db.reset();
			con.reset();
			break;
		}
	}
}

TEST_CASE("Test using appender after table is dropped", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// create the table
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
	// now create the appender
	Appender appender(con, "integers");

	// appending works initially
	appender.BeginRow();
	appender.Append<int32_t>(1);
	appender.EndRow();
	appender.Flush();

	// now drop the table
	REQUIRE_NO_FAIL(con.Query("DROP TABLE integers"));
	// now appending fails
	appender.BeginRow();
	appender.Append<int32_t>(1);
	appender.EndRow();
	REQUIRE_THROWS(appender.Flush());
}

TEST_CASE("Test using appender after table is altered", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// create the table
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
	// now create the appender
	Appender appender(con, "integers");

	// appending works initially
	appender.BeginRow();
	appender.Append<int32_t>(1);
	appender.EndRow();

	// now create a new table with the same name but with different types
	REQUIRE_NO_FAIL(con.Query("DROP TABLE integers"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i VARCHAR, j INTEGER)"));
	// now appending fails
	appender.BeginRow();
	appender.Append<int32_t>(1);
	appender.EndRow();
	REQUIRE_THROWS(appender.Flush());
}

TEST_CASE("Test appenders and transactions", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::unique_ptr<QueryResult> result;
	// create the table
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
	// now create the appender
	Appender appender(con, "integers");

	// rollback an append
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	appender.BeginRow();
	appender.Append<int32_t>(1);
	appender.EndRow();
	appender.Flush();
	result = con.Query("SELECT * FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	result = con.Query("SELECT * FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {}));

	// we can still use the appender in auto commit mode
	appender.BeginRow();
	appender.Append<int32_t>(1);
	appender.EndRow();
	appender.Flush();

	result = con.Query("SELECT * FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}

TEST_CASE("Test using multiple appenders", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::unique_ptr<QueryResult> result;
	// create the table
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t1(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t2(i VARCHAR, j DATE)"));
	// now create the appender
	Appender a1(con, "t1");
	Appender a2(con, "t2");

	// begin appending from both
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));
	a1.BeginRow();
	a1.Append<int32_t>(1);
	a1.EndRow();
	a1.Flush();

	a2.BeginRow();
	a2.Append<const char *>("hello");
	a2.Append<Value>(Value::DATE(1992, 1, 1));
	a2.EndRow();
	a2.Flush();

	result = con.Query("SELECT * FROM t1");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	result = con.Query("SELECT * FROM t2");
	REQUIRE(CHECK_COLUMN(result, 0, {"hello"}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::DATE(1992, 1, 1)}));

	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	result = con.Query("SELECT * FROM t1");
	REQUIRE(CHECK_COLUMN(result, 0, {}));
}

TEST_CASE("Test usage of appender interleaved with connection usage", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::unique_ptr<QueryResult> result;
	// create the table
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t1(i INTEGER)"));
	Appender appender(con, "t1");

	appender.AppendRow(1);
	appender.Flush();

	result = con.Query("SELECT * FROM t1");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	appender.AppendRow(2);
	appender.Flush();

	result = con.Query("SELECT * FROM t1");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
}

TEST_CASE("Test appender during stack unwinding", "[api]") {
	// test appender exception
	DuckDB db;
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
	{
		Appender appender(con, "integers");
		appender.AppendRow(1);

		// closing the apender throws an exception, because we changed the table's columns
		REQUIRE_NO_FAIL(con.Query("ALTER TABLE integers ADD COLUMN j VARCHAR"));
		REQUIRE_THROWS(appender.Close());
	}
	REQUIRE_NO_FAIL(con.Query("DROP TABLE integers"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
	try {
		// now we do the same, but we trigger the destructor of the appender during stack unwinding
		Appender appender(con, "integers");
		appender.AppendRow(1);

		REQUIRE_NO_FAIL(con.Query("ALTER TABLE integers ADD COLUMN j VARCHAR"));
		{ throw std::runtime_error("Hello"); }
	} catch (...) {
	}
}
