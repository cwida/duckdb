#include "capi_tester.hpp"
#include <regex>

using namespace duckdb;

static void require_hugeint_eq(duckdb_hugeint left, duckdb_hugeint right) {
	REQUIRE(left.lower == right.lower);
	REQUIRE(left.upper == right.upper);
}

static void require_hugeint_eq(duckdb_hugeint left, uint64_t lower, int64_t upper) {
	duckdb_hugeint temp;
	temp.lower = lower;
	temp.upper = upper;
	require_hugeint_eq(left, temp);
}

static void require_uhugeint_eq(duckdb_uhugeint left, duckdb_uhugeint right) {
	REQUIRE(left.lower == right.lower);
	REQUIRE(left.upper == right.upper);
}

static void require_uhugeint_eq(duckdb_uhugeint left, uint64_t lower, uint64_t upper) {
	duckdb_uhugeint temp;
	temp.lower = lower;
	temp.upper = upper;
	require_uhugeint_eq(left, temp);
}

TEST_CASE("Basic test of C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	REQUIRE_NO_FAIL(tester.Query("SET default_null_order='nulls_first'"));
	// select scalar value
	result = tester.Query("SELECT CAST(42 AS BIGINT)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnType(0) == DUCKDB_TYPE_BIGINT);
	REQUIRE(result->ColumnData<int64_t>(0)[0] == 42);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 42);
	REQUIRE(!result->IsNull(0, 0));
	// out of range fetch
	REQUIRE(result->Fetch<int64_t>(1, 0) == 0);
	REQUIRE(result->Fetch<int64_t>(0, 1) == 0);
	// cannot fetch data chunk after using the value API
	REQUIRE(result->FetchChunk(0) == nullptr);

	// select scalar NULL
	result = tester.Query("SELECT NULL");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
	REQUIRE(result->IsNull(0, 0));

	// select scalar string
	result = tester.Query("SELECT 'hello'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<string>(0, 0) == "hello");
	REQUIRE(!result->IsNull(0, 0));

	result = tester.Query("SELECT 1=1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<bool>(0, 0) == true);
	REQUIRE(!result->IsNull(0, 0));

	result = tester.Query("SELECT 1=0");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<bool>(0, 0) == false);
	REQUIRE(!result->IsNull(0, 0));

	result = tester.Query("SELECT i FROM (values (true), (false)) tbl(i) group by i order by i");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 2);
	REQUIRE(result->Fetch<bool>(0, 0) == false);
	REQUIRE(result->Fetch<bool>(0, 1) == true);
	REQUIRE(!result->IsNull(0, 0));

	// multiple insertions
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test (a INTEGER, b INTEGER);"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO test VALUES (11, 22)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO test VALUES (NULL, 21)"));
	result = tester.Query("INSERT INTO test VALUES (13, 22)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->rows_changed() == 1);

	// NULL selection
	result = tester.Query("SELECT a, b FROM test ORDER BY a");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->rows_changed() == 0);
	// NULL, 11, 13
	REQUIRE(result->IsNull(0, 0));
	REQUIRE(result->Fetch<int32_t>(0, 1) == 11);
	REQUIRE(result->Fetch<int32_t>(0, 2) == 13);
	// 21, 22, 22
	REQUIRE(result->Fetch<int32_t>(1, 0) == 21);
	REQUIRE(result->Fetch<int32_t>(1, 1) == 22);
	REQUIRE(result->Fetch<int32_t>(1, 2) == 22);

	REQUIRE(result->ColumnName(0) == "a");
	REQUIRE(result->ColumnName(1) == "b");
	REQUIRE(result->ColumnName(2) == "");

	result = tester.Query("UPDATE test SET a = 1 WHERE b=22");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->rows_changed() == 2);

	// several error conditions
	REQUIRE(duckdb_value_is_null(nullptr, 0, 0) == false);
	REQUIRE(duckdb_column_type(nullptr, 0) == DUCKDB_TYPE_INVALID);
	REQUIRE(duckdb_column_count(nullptr) == 0);
	REQUIRE(duckdb_row_count(nullptr) == 0);
	REQUIRE(duckdb_rows_changed(nullptr) == 0);
	REQUIRE(duckdb_result_error(nullptr) == nullptr);
	REQUIRE(duckdb_nullmask_data(nullptr, 0) == nullptr);
	REQUIRE(duckdb_column_data(nullptr, 0) == nullptr);
	REQUIRE(duckdb_result_error_type(nullptr) == DUCKDB_ERROR_INVALID);
}

TEST_CASE("Test different types of C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("SET default_null_order='nulls_first'"));

	// integer columns
	duckdb::vector<string> types = {"TINYINT",  "SMALLINT",  "INTEGER",  "BIGINT",  "HUGEINT",
	                                "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT", "UHUGEINT"};
	for (auto &type : types) {
		// create the table and insert values
		REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE integers(i " + type + ")"));
		REQUIRE_NO_FAIL(tester.Query("INSERT INTO integers VALUES (1), (NULL)"));

		result = tester.Query("SELECT * FROM integers ORDER BY i");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->IsNull(0, 0));
		REQUIRE(result->Fetch<int8_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int16_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int32_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
		REQUIRE(result->Fetch<uint8_t>(0, 0) == 0);
		REQUIRE(result->Fetch<uint16_t>(0, 0) == 0);
		REQUIRE(result->Fetch<uint32_t>(0, 0) == 0);
		REQUIRE(result->Fetch<uint64_t>(0, 0) == 0);
		REQUIRE(duckdb_uhugeint_to_double(result->Fetch<duckdb_uhugeint>(0, 0)) == 0);
		REQUIRE(duckdb_hugeint_to_double(result->Fetch<duckdb_hugeint>(0, 0)) == 0);
		REQUIRE(result->Fetch<string>(0, 0) == "");
		REQUIRE(ApproxEqual(result->Fetch<float>(0, 0), 0.0f));
		REQUIRE(ApproxEqual(result->Fetch<double>(0, 0), 0.0));

		REQUIRE(!result->IsNull(0, 1));
		REQUIRE(result->Fetch<int8_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int16_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int32_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int64_t>(0, 1) == 1);
		REQUIRE(result->Fetch<uint8_t>(0, 1) == 1);
		REQUIRE(result->Fetch<uint16_t>(0, 1) == 1);
		REQUIRE(result->Fetch<uint32_t>(0, 1) == 1);
		REQUIRE(result->Fetch<uint64_t>(0, 1) == 1);
		REQUIRE(duckdb_uhugeint_to_double(result->Fetch<duckdb_uhugeint>(0, 1)) == 1);
		REQUIRE(duckdb_hugeint_to_double(result->Fetch<duckdb_hugeint>(0, 1)) == 1);
		REQUIRE(ApproxEqual(result->Fetch<float>(0, 1), 1.0f));
		REQUIRE(ApproxEqual(result->Fetch<double>(0, 1), 1.0));
		REQUIRE(result->Fetch<string>(0, 1) == "1");

		REQUIRE_NO_FAIL(tester.Query("ROLLBACK"));
	}
	// real/double columns
	types = {"REAL", "DOUBLE"};
	for (auto &type : types) {
		// create the table and insert values
		REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE doubles(i " + type + ")"));
		REQUIRE_NO_FAIL(tester.Query("INSERT INTO doubles VALUES (1), (NULL)"));

		result = tester.Query("SELECT * FROM doubles ORDER BY i");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->IsNull(0, 0));
		REQUIRE(result->Fetch<int8_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int16_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int32_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
		REQUIRE(result->Fetch<string>(0, 0) == "");
		REQUIRE(ApproxEqual(result->Fetch<float>(0, 0), 0.0f));
		REQUIRE(ApproxEqual(result->Fetch<double>(0, 0), 0.0));

		REQUIRE(!result->IsNull(0, 1));
		REQUIRE(result->Fetch<int8_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int16_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int32_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int64_t>(0, 1) == 1);
		REQUIRE(ApproxEqual(result->Fetch<float>(0, 1), 1.0f));
		REQUIRE(ApproxEqual(result->Fetch<double>(0, 1), 1.0));

		REQUIRE_NO_FAIL(tester.Query("ROLLBACK"));
	}
	// date columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE dates(d DATE)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO dates VALUES ('1992-09-20'), (NULL), ('30000-09-20')"));

	result = tester.Query("SELECT * FROM dates ORDER BY d");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	duckdb_date_struct date = duckdb_from_date(result->Fetch<duckdb_date>(0, 1));
	REQUIRE(date.year == 1992);
	REQUIRE(date.month == 9);
	REQUIRE(date.day == 20);
	REQUIRE(result->Fetch<string>(0, 1) == Value::DATE(1992, 9, 20).ToString());
	date = duckdb_from_date(result->Fetch<duckdb_date>(0, 2));
	REQUIRE(date.year == 30000);
	REQUIRE(date.month == 9);
	REQUIRE(date.day == 20);
	REQUIRE(result->Fetch<string>(0, 2) == Value::DATE(30000, 9, 20).ToString());

	// time columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE times(d TIME)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO times VALUES ('12:00:30.1234'), (NULL), ('02:30:01')"));

	result = tester.Query("SELECT * FROM times ORDER BY d");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	duckdb_time_struct time_val = duckdb_from_time(result->Fetch<duckdb_time>(0, 1));
	REQUIRE(time_val.hour == 2);
	REQUIRE(time_val.min == 30);
	REQUIRE(time_val.sec == 1);
	REQUIRE(time_val.micros == 0);
	REQUIRE(result->Fetch<string>(0, 1) == Value::TIME(2, 30, 1, 0).ToString());
	time_val = duckdb_from_time(result->Fetch<duckdb_time>(0, 2));
	REQUIRE(time_val.hour == 12);
	REQUIRE(time_val.min == 0);
	REQUIRE(time_val.sec == 30);
	REQUIRE(time_val.micros == 123400);
	REQUIRE(result->Fetch<string>(0, 2) == Value::TIME(12, 0, 30, 123400).ToString());

	// time_ns columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE time_ns_values(d TIME_NS)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO time_ns_values VALUES ('12:34:56.789123456'), (NULL), ('12:34:56')"));

	result = tester.Query("SELECT * FROM time_ns_values ORDER BY d");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	duckdb_time_ns time_ns_val = result->Fetch<duckdb_time_ns>(0, 1);
	REQUIRE(time_ns_val.nanos == 45296000000000);
	REQUIRE(result->Fetch<string>(0, 1) == Value::TIME_NS(dtime_ns_t(45296000000000)).ToString());
	time_ns_val = result->Fetch<duckdb_time_ns>(0, 2);
	REQUIRE(time_ns_val.nanos == 45296789123456);
	REQUIRE(result->Fetch<string>(0, 2) == Value::TIME_NS(dtime_ns_t(45296789123456)).ToString());

	// blob columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE blobs(b BLOB)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO blobs VALUES ('hello\\x12world'), ('\\x00'), (NULL)"));

	result = tester.Query("SELECT * FROM blobs");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(!result->IsNull(0, 0));
	duckdb_blob blob = result->Fetch<duckdb_blob>(0, 0);
	REQUIRE(blob.size == 11);
	REQUIRE(memcmp(blob.data, "hello\012world", 11));
	REQUIRE(result->Fetch<string>(0, 1) == "\\x00");
	REQUIRE(result->IsNull(0, 2));
	blob = result->Fetch<duckdb_blob>(0, 2);
	REQUIRE(blob.data == nullptr);
	REQUIRE(blob.size == 0);

	// boolean columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE booleans(b BOOLEAN)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO booleans VALUES (42 > 60), (42 > 20), (42 > NULL)"));

	result = tester.Query("SELECT * FROM booleans ORDER BY b");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	REQUIRE(!result->Fetch<bool>(0, 0));
	REQUIRE(!result->Fetch<bool>(0, 1));
	REQUIRE(result->Fetch<bool>(0, 2));
	REQUIRE(result->Fetch<string>(0, 2) == "true");

	// decimal columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE decimals(dec DECIMAL(18, 4) NULL)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO decimals VALUES (NULL), (12.3)"));

	result = tester.Query("SELECT * FROM decimals ORDER BY dec");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	duckdb_decimal decimal = result->Fetch<duckdb_decimal>(0, 1);
	REQUIRE(duckdb_decimal_to_double(decimal) == 12.3);
	// test more decimal physical types
	result = tester.Query("SELECT "
	                      "1.2::DECIMAL(4,1),"
	                      "100.3::DECIMAL(9,1),"
	                      "-320938.4298::DECIMAL(18,4),"
	                      "49082094824.904820482094::DECIMAL(30,12),"
	                      "NULL::DECIMAL");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(duckdb_decimal_to_double(result->Fetch<duckdb_decimal>(0, 0)) == 1.2);
	REQUIRE(duckdb_decimal_to_double(result->Fetch<duckdb_decimal>(1, 0)) == 100.3);
	REQUIRE(duckdb_decimal_to_double(result->Fetch<duckdb_decimal>(2, 0)) == -320938.4298);
	REQUIRE(duckdb_decimal_to_double(result->Fetch<duckdb_decimal>(3, 0)) == 49082094824.904820482094);
	REQUIRE(duckdb_decimal_to_double(result->Fetch<duckdb_decimal>(4, 0)) == 0.0);

	REQUIRE(!result->IsNull(0, 0));
	REQUIRE(!result->IsNull(1, 0));
	REQUIRE(!result->IsNull(2, 0));
	REQUIRE(!result->IsNull(3, 0));
	REQUIRE(result->IsNull(4, 0));

	REQUIRE(result->Fetch<bool>(0, 0) == true);
	REQUIRE(result->Fetch<bool>(1, 0) == true);
	REQUIRE(result->Fetch<bool>(2, 0) == true);
	REQUIRE(result->Fetch<bool>(3, 0) == true);
	REQUIRE(result->Fetch<bool>(4, 0) == false);

	REQUIRE(result->Fetch<int8_t>(0, 0) == 1);
	REQUIRE(result->Fetch<int8_t>(1, 0) == 100);
	REQUIRE(result->Fetch<int8_t>(2, 0) == 0); // overflow
	REQUIRE(result->Fetch<int8_t>(3, 0) == 0); // overflow
	REQUIRE(result->Fetch<int8_t>(4, 0) == 0);

	REQUIRE(result->Fetch<uint8_t>(0, 0) == 1);
	REQUIRE(result->Fetch<uint8_t>(1, 0) == 100);
	REQUIRE(result->Fetch<uint8_t>(2, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint8_t>(3, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint8_t>(4, 0) == 0);

	REQUIRE(result->Fetch<int16_t>(0, 0) == 1);
	REQUIRE(result->Fetch<int16_t>(1, 0) == 100);
	REQUIRE(result->Fetch<int16_t>(2, 0) == 0); // overflow
	REQUIRE(result->Fetch<int16_t>(3, 0) == 0); // overflow
	REQUIRE(result->Fetch<int16_t>(4, 0) == 0);

	REQUIRE(result->Fetch<uint16_t>(0, 0) == 1);
	REQUIRE(result->Fetch<uint16_t>(1, 0) == 100);
	REQUIRE(result->Fetch<uint16_t>(2, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint16_t>(3, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint16_t>(4, 0) == 0);

	REQUIRE(result->Fetch<int32_t>(0, 0) == 1);
	REQUIRE(result->Fetch<int32_t>(1, 0) == 100);
	REQUIRE(result->Fetch<int32_t>(2, 0) == -320938);
	REQUIRE(result->Fetch<int32_t>(3, 0) == 0); // overflow
	REQUIRE(result->Fetch<int32_t>(4, 0) == 0);

	REQUIRE(result->Fetch<uint32_t>(0, 0) == 1);
	REQUIRE(result->Fetch<uint32_t>(1, 0) == 100);
	REQUIRE(result->Fetch<uint32_t>(2, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint32_t>(3, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint32_t>(4, 0) == 0);

	REQUIRE(result->Fetch<int64_t>(0, 0) == 1);
	REQUIRE(result->Fetch<int64_t>(1, 0) == 100);
	REQUIRE(result->Fetch<int64_t>(2, 0) == -320938);
	REQUIRE(result->Fetch<int64_t>(3, 0) == 49082094825); // ceiling
	REQUIRE(result->Fetch<int64_t>(4, 0) == 0);

	REQUIRE(result->Fetch<uint64_t>(0, 0) == 1);
	REQUIRE(result->Fetch<uint64_t>(1, 0) == 100);
	REQUIRE(result->Fetch<uint64_t>(2, 0) == 0); // overflow
	REQUIRE(result->Fetch<uint64_t>(3, 0) == 49082094825);
	REQUIRE(result->Fetch<uint64_t>(4, 0) == 0);

	require_hugeint_eq(result->Fetch<duckdb_hugeint>(0, 0), 1, 0);
	require_hugeint_eq(result->Fetch<duckdb_hugeint>(1, 0), 100, 0);
	require_hugeint_eq(result->Fetch<duckdb_hugeint>(2, 0), 18446744073709230678ul, -1);
	require_hugeint_eq(result->Fetch<duckdb_hugeint>(3, 0), 49082094825, 0);
	require_hugeint_eq(result->Fetch<duckdb_hugeint>(4, 0), 0, 0);

	require_uhugeint_eq(result->Fetch<duckdb_uhugeint>(0, 0), 1, 0);
	require_uhugeint_eq(result->Fetch<duckdb_uhugeint>(1, 0), 100, 0);
	require_uhugeint_eq(result->Fetch<duckdb_uhugeint>(2, 0), 0, 0); // overflow
	require_uhugeint_eq(result->Fetch<duckdb_uhugeint>(3, 0), 49082094825, 0);
	require_uhugeint_eq(result->Fetch<duckdb_uhugeint>(4, 0), 0, 0);

	REQUIRE(result->Fetch<float>(0, 0) == 1.2f);
	REQUIRE(result->Fetch<float>(1, 0) == 100.3f);
	REQUIRE(floor(result->Fetch<float>(2, 0)) == -320939);
	REQUIRE((int64_t)floor(result->Fetch<float>(3, 0)) == 49082093568);
	REQUIRE(result->Fetch<float>(4, 0) == 0.0);

	REQUIRE(result->Fetch<double>(0, 0) == 1.2);
	REQUIRE(result->Fetch<double>(1, 0) == 100.3);
	REQUIRE(result->Fetch<double>(2, 0) == -320938.4298);
	REQUIRE(result->Fetch<double>(3, 0) == 49082094824.904820482094);
	REQUIRE(result->Fetch<double>(4, 0) == 0.0);

	REQUIRE(result->Fetch<string>(0, 0) == "1.2");
	REQUIRE(result->Fetch<string>(1, 0) == "100.3");
	REQUIRE(result->Fetch<string>(2, 0) == "-320938.4298");
	REQUIRE(result->Fetch<string>(3, 0) == "49082094824.904820482094");
	REQUIRE(result->Fetch<string>(4, 0) == "");

	result = tester.Query("SELECT -123.45::DECIMAL(5,2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<bool>(0, 0) == true);
	REQUIRE(result->Fetch<int8_t>(0, 0) == -123);
	REQUIRE(result->Fetch<uint8_t>(0, 0) == 0);
	REQUIRE(result->Fetch<int16_t>(0, 0) == -123);
	REQUIRE(result->Fetch<uint16_t>(0, 0) == 0);
	REQUIRE(result->Fetch<int32_t>(0, 0) == -123);
	REQUIRE(result->Fetch<uint32_t>(0, 0) == 0);
	REQUIRE(result->Fetch<int64_t>(0, 0) == -123);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 0);

	hugeint_t expected_hugeint_val;
	Hugeint::TryConvert(-123, expected_hugeint_val);
	duckdb_hugeint expected_val;
	expected_val.lower = expected_hugeint_val.lower;
	expected_val.upper = expected_hugeint_val.upper;
	require_hugeint_eq(result->Fetch<duckdb_hugeint>(0, 0), expected_val);

	REQUIRE(result->Fetch<float>(0, 0) == -123.45f);
	REQUIRE(result->Fetch<double>(0, 0) == -123.45);
	REQUIRE(result->Fetch<string>(0, 0) == "-123.45");
}

TEST_CASE("decompose timetz with duckdb_from_time_tz", "[capi]") {
	CAPITester tester;

	REQUIRE(tester.OpenDatabase(nullptr));

	auto res = tester.Query("SELECT TIMETZ '11:30:00.123456-02:00'");
	REQUIRE(res->success);

	auto chunk = res->FetchChunk(0);

	REQUIRE(chunk->ColumnCount() == 1);
	REQUIRE(res->ColumnType(0) == DUCKDB_TYPE_TIME_TZ);

	auto data = (duckdb_time_tz *)chunk->GetData(0);

	auto time_tz = duckdb_from_time_tz(data[0]);

	REQUIRE(time_tz.time.hour == 11);
	REQUIRE(time_tz.time.min == 30);
	REQUIRE(time_tz.time.sec == 0);
	REQUIRE(time_tz.time.micros == 123456);

	REQUIRE(time_tz.offset == -7200);
}

TEST_CASE("decompose every finite timestamp with duckdb_from_timestamp", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	// a finite timestamp before the first midnight of the range is stored and read back
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t AS SELECT make_timestamp(-9223372036854775806) AS ts"));
	auto result = tester.Query("SELECT ts FROM t");
	REQUIRE_NO_FAIL(*result);
	auto chunk = result->FetchChunk(0);
	auto ts = static_cast<duckdb_timestamp *>(chunk->GetData(0))[0];
	REQUIRE(duckdb_is_finite_timestamp(ts));
	auto parts = duckdb_from_timestamp(ts);
	REQUIRE(parts.date.year == -290308);
	REQUIRE(parts.date.month == 12);
	REQUIRE(parts.date.day == 21);
	REQUIRE(parts.time.hour == 19);
	REQUIRE(parts.time.min == 59);
	REQUIRE(parts.time.sec == 5);
	REQUIRE(parts.time.micros == 224194);

	// the parts of every finite timestamp convert back to it
	for (int64_t micros :
	     {NumericLimits<int64_t>::Minimum(), int64_t(-9223372036854775806), int64_t(-9223372022400000001),
	      int64_t(-9223372022400000000), int64_t(0), int64_t(9223372036854775806)}) {
		CAPTURE(micros);
		duckdb_timestamp finite {micros};
		REQUIRE(duckdb_to_timestamp(duckdb_from_timestamp(finite)).micros == micros);
	}

	// infinite timestamps get the parts of the infinite dates at midnight
	for (auto infinite : {timestamp_t::infinity(), timestamp_t::ninfinity()}) {
		auto infinite_parts = duckdb_from_timestamp(duckdb_timestamp {infinite.value});
		auto date_parts = duckdb_from_date(duckdb_date {Timestamp::GetDate(infinite).days});
		REQUIRE(infinite_parts.date.year == date_parts.year);
		REQUIRE(infinite_parts.date.month == date_parts.month);
		REQUIRE(infinite_parts.date.day == date_parts.day);
		REQUIRE(infinite_parts.time.hour == 0);
		REQUIRE(infinite_parts.time.micros == 0);
	}
}

TEST_CASE("recompose invalid dates and timestamps without throwing", "[capi]") {
	duckdb_time_struct midnight {0, 0, 0, 0};

	// invalid or out of range dates become 0
	for (auto date :
	     {duckdb_date_struct {2021, 2, 30}, duckdb_date_struct {2021, 2, 29}, duckdb_date_struct {2021, 13, 1},
	      duckdb_date_struct {2021, 0, 1}, duckdb_date_struct {2021, 1, 0}, duckdb_date_struct {2021, 4, 31},
	      duckdb_date_struct {NumericLimits<int32_t>::Maximum(), 1, 1},
	      duckdb_date_struct {NumericLimits<int32_t>::Minimum(), 1, 1}}) {
		CAPTURE(date.year, date.month, date.day);
		REQUIRE(duckdb_to_date(date).days == 0);
		REQUIRE(duckdb_to_timestamp(duckdb_timestamp_struct {date, midnight}).micros == 0);
	}

	// valid dates outside the timestamp range become 0 as timestamps
	for (auto date : {duckdb_date_struct {294248, 1, 1}, duckdb_date_struct {-290309, 1, 1}}) {
		CAPTURE(date.year);
		REQUIRE(duckdb_to_date(date).days != 0);
		REQUIRE(duckdb_to_timestamp(duckdb_timestamp_struct {date, midnight}).micros == 0);
	}

	// valid parts still convert
	REQUIRE(duckdb_to_date(duckdb_date_struct {2024, 2, 29}).days == 19782);
	auto ts = duckdb_to_timestamp(duckdb_timestamp_struct {{2024, 2, 29}, {1, 2, 3, 4}});
	REQUIRE(ts.micros == 1709168523000004);
}

TEST_CASE("create time_tz value") {
	duckdb_time_struct time;
	time.hour = 4;
	time.min = 2;
	time.sec = 6;
	time.micros = 9;
	int offset = 8000;

	auto micros = duckdb_to_time(time);
	auto res = duckdb_create_time_tz(micros.micros, offset);

	// and back again

	auto inverse = duckdb_from_time_tz(res);
	REQUIRE(offset == inverse.offset);

	REQUIRE(inverse.time.hour == 4);
	REQUIRE(inverse.time.min == 2);
	REQUIRE(inverse.time.sec == 6);
	REQUIRE(inverse.time.micros == 9);
}

TEST_CASE("Test errors in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	// cannot open database in random directory
	REQUIRE(!tester.OpenDatabase("/bla/this/directory/should/not/exist/hopefully/awerar333"));
	REQUIRE(tester.OpenDatabase(nullptr));

	// syntax error in query
	REQUIRE_FAIL(tester.Query("SELEC * FROM TABLE"));
	// bind error
	REQUIRE_FAIL(tester.Query("SELECT * FROM TABLE"));

	duckdb_result res;
	duckdb_prepared_statement stmt = nullptr;
	// fail prepare API calls
	REQUIRE(duckdb_prepare(NULL, "SELECT 42", &stmt) == DuckDBError);
	REQUIRE(duckdb_prepare(tester.connection, NULL, &stmt) == DuckDBError);
	REQUIRE(stmt == nullptr);

	REQUIRE(duckdb_prepare(tester.connection, "SELECT * from INVALID_TABLE", &stmt) == DuckDBError);
	REQUIRE(duckdb_prepare_error(nullptr) == nullptr);
	REQUIRE(stmt != nullptr);
	REQUIRE(duckdb_prepare_error(stmt) != nullptr);
	duckdb_destroy_prepare(&stmt);

	REQUIRE(duckdb_bind_boolean(NULL, 0, true) == DuckDBError);
	REQUIRE(duckdb_execute_prepared(NULL, &res) == DuckDBError);
	duckdb_destroy_prepare(NULL);

	// fail to query arrow
	duckdb_arrow out_arrow;
	REQUIRE(duckdb_query_arrow(tester.connection, "SELECT * from INVALID_TABLE", &out_arrow) == DuckDBError);
	REQUIRE(duckdb_query_arrow_error(out_arrow) != nullptr);
	duckdb_destroy_arrow(&out_arrow);

	// various edge cases/nullptrs
	REQUIRE(duckdb_query_arrow_schema(out_arrow, nullptr) == DuckDBSuccess);
	REQUIRE(duckdb_query_arrow_array(out_arrow, nullptr) == DuckDBSuccess);

	// default duckdb_value_date on invalid date
	result = tester.Query("SELECT 1, true, 'a'");
	REQUIRE_NO_FAIL(*result);
	duckdb_date_struct d = result->Fetch<duckdb_date_struct>(0, 0);
	REQUIRE(d.year == 1970);
	REQUIRE(d.month == 1);
	REQUIRE(d.day == 1);
	d = result->Fetch<duckdb_date_struct>(1, 0);
	REQUIRE(d.year == 1970);
	REQUIRE(d.month == 1);
	REQUIRE(d.day == 1);
	d = result->Fetch<duckdb_date_struct>(2, 0);
	REQUIRE(d.year == 1970);
	REQUIRE(d.month == 1);
	REQUIRE(d.day == 1);
}

TEST_CASE("Test C API config", "[capi]") {
	duckdb_database db = nullptr;
	duckdb_connection con = nullptr;
	duckdb_config config = nullptr;
	duckdb_result result;

	// enumerate config options
	auto config_count = duckdb_config_count();
	for (size_t i = 0; i < config_count; i++) {
		const char *name = nullptr;
		const char *description = nullptr;
		duckdb_get_config_flag(i, &name, &description);
		REQUIRE(strlen(name) > 0);
		REQUIRE(strlen(description) > 0);
	}

	// test config creation
	REQUIRE(duckdb_create_config(&config) == DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "access_mode", "invalid_access_mode") == DuckDBError);
	REQUIRE(duckdb_set_config(config, "access_mode", "read_only") == DuckDBSuccess);

	auto dbdir = TestCreatePath("capi_read_only_db");

	// open the database & connection
	// cannot open an in-memory database in read-only mode
	char *error = nullptr;
	REQUIRE(duckdb_open_ext(":memory:", &db, config, &error) == DuckDBError);
	REQUIRE(strlen(error) > 0);
	duckdb_free(error);
	// now without the error
	REQUIRE(duckdb_open_ext(":memory:", &db, config, nullptr) == DuckDBError);
	// cannot open a database that does not exist
	REQUIRE(duckdb_open_ext(dbdir.c_str(), &db, config, &error) == DuckDBError);
	REQUIRE(strlen(error) > 0);
	duckdb_free(error);
	// we can create the database and add some tables
	{
		DuckDB cppdb(dbdir);
		Connection cppcon(cppdb);
		cppcon.Query("CREATE TABLE integers(i INTEGER)");
		cppcon.Query("INSERT INTO integers VALUES (42)");
	}

	// now we can connect
	REQUIRE(duckdb_open_ext(dbdir.c_str(), &db, config, &error) == DuckDBSuccess);

	// test unrecognized configuration
	REQUIRE(duckdb_set_config(config, "aaaa_invalidoption", "read_only") == DuckDBSuccess);
	REQUIRE(((DBConfig *)config)->options.unrecognized_options["aaaa_invalidoption"] == "read_only");
	REQUIRE(duckdb_open_ext(dbdir.c_str(), &db, config, &error) == DuckDBError);
	REQUIRE_THAT(error, Catch::Matchers::Contains("The following options were not recognized"));
	duckdb_free(error);

	// we can destroy the config right after duckdb_open
	duckdb_destroy_config(&config);
	// we can spam this
	duckdb_destroy_config(&config);
	duckdb_destroy_config(&config);

	REQUIRE(duckdb_connect(db, nullptr) == DuckDBError);
	REQUIRE(duckdb_connect(nullptr, &con) == DuckDBError);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);

	// we can query
	REQUIRE(duckdb_query(con, "SELECT 42::INT", &result) == DuckDBSuccess);
	REQUIRE(duckdb_value_int32(&result, 0, 0) == 42);
	duckdb_destroy_result(&result);
	REQUIRE(duckdb_query(con, "SELECT i::INT FROM integers", &result) == DuckDBSuccess);
	REQUIRE(duckdb_value_int32(&result, 0, 0) == 42);
	duckdb_destroy_result(&result);

	// but we cannot create new tables
	REQUIRE(duckdb_query(con, "CREATE TABLE new_table(i INTEGER)", nullptr) == DuckDBError);

	duckdb_disconnect(&con);
	duckdb_close(&db);

	// api abuse
	REQUIRE(duckdb_create_config(nullptr) == DuckDBError);
	REQUIRE(duckdb_get_config_flag(9999999, nullptr, nullptr) == DuckDBError);
	REQUIRE(duckdb_set_config(nullptr, nullptr, nullptr) == DuckDBError);
	REQUIRE(duckdb_create_config(nullptr) == DuckDBError);
	duckdb_destroy_config(nullptr);
	duckdb_destroy_config(nullptr);
}

TEST_CASE("Issue #2058: Cleanup after execution of invalid SQL statement causes segmentation fault", "[capi]") {
	duckdb_database db;
	duckdb_connection con;
	duckdb_result result;
	duckdb_result result_count;

	REQUIRE(duckdb_open(NULL, &db) != DuckDBError);
	REQUIRE(duckdb_connect(db, &con) != DuckDBError);

	REQUIRE(duckdb_query(con, "CREATE TABLE integers(i INTEGER, j INTEGER);", NULL) != DuckDBError);
	REQUIRE((duckdb_query(con, "SELECT count(*) FROM integers;", &result_count) != DuckDBError));

	duckdb_destroy_result(&result_count);

	REQUIRE(duckdb_query(con, "non valid SQL", &result) == DuckDBError);

	duckdb_destroy_result(&result); // segmentation failure happens here

	duckdb_disconnect(&con);
	duckdb_close(&db);
}

TEST_CASE("Decimal -> Double casting issue", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	result = tester.Query("select -0.5;");
	REQUIRE_NO_FAIL(*result);

	REQUIRE(result->ColumnType(0) == DUCKDB_TYPE_DECIMAL);
	auto double_from_decimal = result->Fetch<double>(0, 0);
	REQUIRE(double_from_decimal == (double)-0.5);

	auto string_from_decimal = result->Fetch<string>(0, 0);
	REQUIRE(string_from_decimal == "-0.5");
}

TEST_CASE("Test custom_user_agent config", "[capi]") {
	{
		duckdb_database db;
		duckdb_connection con;
		duckdb_result result;

		// Default custom_user_agent value
		REQUIRE(duckdb_open_ext(NULL, &db, nullptr, NULL) != DuckDBError);
		REQUIRE(duckdb_connect(db, &con) != DuckDBError);

		duckdb_query(con, "PRAGMA user_agent", &result);

		REQUIRE(duckdb_row_count(&result) == 1);
		char *user_agent_value = duckdb_value_varchar(&result, 0, 0);
		REQUIRE_THAT(user_agent_value, Catch::Matchers::Matches("duckdb/.*(.*) capi"));

		duckdb_free(user_agent_value);
		duckdb_destroy_result(&result);
		duckdb_disconnect(&con);
		duckdb_close(&db);
	}

	{
		// Custom custom_user_agent value

		duckdb_database db;
		duckdb_connection con;
		duckdb_result result_custom_user_agent;
		duckdb_result result_full_user_agent;

		duckdb_config config;
		REQUIRE(duckdb_create_config(&config) != DuckDBError);
		REQUIRE(duckdb_set_config(config, "custom_user_agent", "CUSTOM_STRING") != DuckDBError);

		REQUIRE(duckdb_open_ext(NULL, &db, config, NULL) != DuckDBError);
		REQUIRE(duckdb_connect(db, &con) != DuckDBError);

		duckdb_query(con, "SELECT current_setting('custom_user_agent')", &result_custom_user_agent);
		duckdb_query(con, "PRAGMA user_agent", &result_full_user_agent);

		REQUIRE(duckdb_row_count(&result_custom_user_agent) == 1);
		REQUIRE(duckdb_row_count(&result_full_user_agent) == 1);

		char *custom_user_agent_value = duckdb_value_varchar(&result_custom_user_agent, 0, 0);
		REQUIRE(string(custom_user_agent_value) == "CUSTOM_STRING");

		char *full_user_agent_value = duckdb_value_varchar(&result_full_user_agent, 0, 0);
		REQUIRE_THAT(full_user_agent_value, Catch::Matchers::Matches("duckdb/.*(.*) capi CUSTOM_STRING"));

		duckdb_destroy_config(&config);
		duckdb_free(custom_user_agent_value);
		duckdb_free(full_user_agent_value);
		duckdb_destroy_result(&result_custom_user_agent);
		duckdb_destroy_result(&result_full_user_agent);
		duckdb_disconnect(&con);
		duckdb_close(&db);
	}
}

TEST_CASE("Test unsupported types in the deprecated C API", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	string query_1 = R"EOF(
		CREATE TABLE test(
			id BIGINT,
			one DECIMAL(18,3)[]
		);
	)EOF";
	string query_2 = "INSERT INTO test VALUES (410, '[]');";
	string query_3 = "INSERT INTO test VALUES (412, '[]');";
	string query_4 = "SELECT id, one FROM test;";
	REQUIRE_NO_FAIL(tester.Query(query_1));
	REQUIRE_NO_FAIL(tester.Query(query_2));
	REQUIRE_NO_FAIL(tester.Query(query_3));

	// Passes, but does return invalid data for unsupported types.
	auto result = tester.Query(query_4);
	auto &result_c = result->InternalResult();

	auto first_bigint_row = duckdb_value_string(&result_c, 0, 0).data;
	REQUIRE(!string(first_bigint_row).compare("410"));
	duckdb_free(first_bigint_row);
	REQUIRE(duckdb_value_string(&result_c, 1, 0).data == nullptr);

	auto second_bigint_row = duckdb_value_string(&result_c, 0, 1).data;
	REQUIRE(!string(second_bigint_row).compare("412"));
	duckdb_free(second_bigint_row);
	REQUIRE(duckdb_value_string(&result_c, 1, 1).data == nullptr);
}

TEST_CASE("Test creating DUCKDB_ERROR_INTERNAL error data", "[capi]") {
	auto error_data = duckdb_create_error_data(DUCKDB_ERROR_INTERNAL, "foo");
	REQUIRE(error_data);
	duckdb_destroy_error_data(&error_data);
}

TEST_CASE("Test creating DUCKDB_ERROR_DATA_CORRUPTION error data", "[capi]") {
	auto error_data = duckdb_create_error_data(DUCKDB_ERROR_DATA_CORRUPTION, "corrupt");
	REQUIRE(error_data);
	REQUIRE(duckdb_error_data_error_type(error_data) == DUCKDB_ERROR_DATA_CORRUPTION);
	duckdb_destroy_error_data(&error_data);
}

TEST_CASE("Test creating DUCKDB_ERROR_RESOURCE_IN_USE error data", "[capi]") {
	auto error_data = duckdb_create_error_data(DUCKDB_ERROR_RESOURCE_IN_USE, "in use");
	REQUIRE(error_data);
	REQUIRE(duckdb_error_data_error_type(error_data) == DUCKDB_ERROR_RESOURCE_IN_USE);
	duckdb_destroy_error_data(&error_data);
}
