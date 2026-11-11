#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

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

struct AppenderOffsetCast {
	template <class SRC, class DST>
	static DST Operation(SRC input) {
		return static_cast<DST>(input + 1);
	}
};

static bool AppenderBigintToTinyintCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return VectorCastHelpers::TemplatedCastLoop<int64_t, int8_t, AppenderOffsetCast>(source, result, count, parameters);
}

//! A chunk holding the BIGINTs 0, 1, 2
static void InitializeBigintChunk(DataChunk &chunk) {
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	auto writer = FlatVector::Writer<int64_t>(chunk.data[0], 3);
	for (int64_t i = 0; i < 3; i++) {
		writer.WriteValue(i);
	}
	chunk.CheckCardinality(3);
}

TEST_CASE("Test appender casts to extension types use the registered cast functions", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// an extension type: an alias registered in the system catalog, like the C API custom types
	auto offset_type = LogicalType(LogicalType::TINYINT).WithAlias("offset_tinyint");
	con.context->RunFunctionInTransaction([&]() {
		CreateTypeInfo info("offset_tinyint", offset_type);
		info.temporary = true;
		info.internal = true;
		Catalog::GetSystemCatalog(*con.context).CreateType(*con.context, info);
	});
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE offsets(i offset_tinyint)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tinyints(i TINYINT)"));
	{
		Appender appender(con, "offsets");
		REQUIRE(appender.GetActiveTypes()[0] == offset_type);
	}
	// both the extension type and the built-in pair get a cast that adds one
	auto &casts = DBConfig::GetConfig(*con.context).GetCastFunctions();
	casts.RegisterCastFunction(LogicalType::BIGINT, offset_type, BoundCastInfo(AppenderBigintToTinyintCast));
	casts.RegisterCastFunction(LogicalType::BIGINT, LogicalType::TINYINT, BoundCastInfo(AppenderBigintToTinyintCast));

	DataChunk chunk;
	InitializeBigintChunk(chunk);
	SECTION("Appender") {
		Appender appender(con, "offsets");
		appender.AppendDataChunk(chunk);
		appender.Close();
		Appender tinyint_appender(con, "tinyints");
		tinyint_appender.AppendDataChunk(chunk);
		tinyint_appender.Close();
	}
	SECTION("Query appender") {
		QueryAppender appender(con, "INSERT INTO offsets SELECT * FROM appended_data", {offset_type});
		appender.AppendDataChunk(chunk);
		appender.Close();
		QueryAppender tinyint_appender(con, "INSERT INTO tinyints SELECT * FROM appended_data", {LogicalType::TINYINT});
		tinyint_appender.AppendDataChunk(chunk);
		tinyint_appender.Close();
	}
	auto result = con.Query("SELECT i::TINYINT FROM offsets ORDER BY i");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));
	// built-in types keep the built-in casts
	result = con.Query("SELECT i FROM tinyints ORDER BY i");
	REQUIRE(CHECK_COLUMN(result, 0, {0, 1, 2}));

	Appender appender(con, "offsets");
	DataChunk target;
	target.Initialize(Allocator::DefaultAllocator(), {offset_type, LogicalType::TINYINT});
	target.SetChildCardinality(1);
	appender.Append(target, Value::BIGINT(41), 0, 0);
	appender.Append(target, Value::BIGINT(41), 1, 0);
	REQUIRE(target.GetValue(0, 0).GetValue<int8_t>() == 42);
	REQUIRE(target.GetValue(1, 0) == Value::TINYINT(41));
}

TEST_CASE("Test appender stores the same TIMESTAMPTZ on every append path", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET TimeZone='America/New_York'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tstz(ts TIMESTAMPTZ)"));
	const string midnight = "2024-01-01 00:00:00";
	auto midnight_ts = Timestamp::FromString(midnight, false);

	Appender appender(con, "tstz");
	// row-wise appends
	appender.BeginRow();
	appender.Append(midnight.c_str());
	appender.EndRow();
	appender.BeginRow();
	appender.Append(midnight_ts);
	appender.EndRow();
	appender.AppendRow(Value(midnight));
	appender.AppendRow(Value::TIMESTAMP(midnight_ts));
	// data chunk appends
	DataChunk varchar_chunk;
	varchar_chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
	varchar_chunk.SetChildCardinality(1);
	varchar_chunk.data[0].SetValue(0, Value(midnight));
	appender.AppendDataChunk(varchar_chunk);
	DataChunk timestamp_chunk;
	timestamp_chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::TIMESTAMP});
	timestamp_chunk.SetChildCardinality(1);
	timestamp_chunk.data[0].SetValue(0, Value::TIMESTAMP(midnight_ts));
	appender.AppendDataChunk(timestamp_chunk);
	appender.Close();

	REQUIRE_NO_FAIL(con.Query("SET TimeZone='UTC'"));
	auto result = con.Query("SELECT ts::VARCHAR FROM tstz");
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"2024-01-01 00:00:00+00", "2024-01-01 00:00:00+00", "2024-01-01 00:00:00+00",
	                      "2024-01-01 00:00:00+00", "2024-01-01 00:00:00+00", "2024-01-01 00:00:00+00"}));
}
