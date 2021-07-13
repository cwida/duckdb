#include "catch.hpp"
#include "duckdb.h"
#include "test_helpers.hpp"
#include "duckdb/common/arrow.hpp"
#include "duckdb/common/exception.hpp"

using namespace duckdb;
using namespace std;

class CAPIResult {
public:
	~CAPIResult() {
		duckdb_destroy_result(&result);
	}
	void Query(duckdb_connection connection, string query) {
		success = (duckdb_query(connection, query.c_str(), &result) == DuckDBSuccess);
	}

	idx_t ColumnCount() {
		return result.column_count;
	}

	idx_t row_count() {
		return result.row_count;
	}

	idx_t rows_changed() {
		return result.rows_changed;
	}

	template <class T>
	T Fetch(idx_t col, idx_t row) {
		throw NotImplementedException("Unimplemented type for fetch");
	}

	bool IsNull(idx_t col, idx_t row) {
		return result.columns[col].nullmask[row];
	}

	string ColumnName(idx_t col) {
		auto colname = duckdb_column_name(&result, col);
		return colname ? string(colname) : string();
	}

	duckdb_result &InternalResult() {
		return result;
	}

public:
	bool success = false;

private:
	duckdb_result result;
};

static bool NO_FAIL(CAPIResult &result) {
	return result.success;
}

static bool NO_FAIL(unique_ptr<CAPIResult> result) {
	return NO_FAIL(*result);
}

template <>
bool CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_boolean(&result, col, row);
}

template <>
int8_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_int8(&result, col, row);
}

template <>
int16_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_int16(&result, col, row);
}

template <>
int32_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_int32(&result, col, row);
}

template <>
int64_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_int64(&result, col, row);
}

template <>
uint8_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_uint8(&result, col, row);
}

template <>
uint16_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_uint16(&result, col, row);
}

template <>
uint32_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_uint32(&result, col, row);
}

template <>
uint64_t CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_uint64(&result, col, row);
}

template <>
float CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_float(&result, col, row);
}

template <>
double CAPIResult::Fetch(idx_t col, idx_t row) {
	return duckdb_value_double(&result, col, row);
}

template <>
duckdb_date CAPIResult::Fetch(idx_t col, idx_t row) {
	auto data = (duckdb_date *)result.columns[col].data;
	return data[row];
}

template <>
duckdb_time CAPIResult::Fetch(idx_t col, idx_t row) {
	auto data = (duckdb_time *)result.columns[col].data;
	return data[row];
}

template <>
duckdb_timestamp CAPIResult::Fetch(idx_t col, idx_t row) {
	auto data = (duckdb_timestamp *)result.columns[col].data;
	return data[row];
}

template <>
duckdb_blob CAPIResult::Fetch(idx_t col, idx_t row) {
	auto data = (duckdb_blob *)result.columns[col].data;
	return data[row];
}

template <>
string CAPIResult::Fetch(idx_t col, idx_t row) {
	auto value = duckdb_value_varchar(&result, col, row);
	string strval = string(value);
	free((void *)value);
	return strval;
}

class CAPITester {
public:
	CAPITester() : database(nullptr), connection(nullptr) {
	}
	~CAPITester() {
		Cleanup();
	}

	void Cleanup() {
		if (connection) {
			duckdb_disconnect(&connection);
			connection = nullptr;
		}
		if (database) {
			duckdb_close(&database);
			database = nullptr;
		}
	}

	bool OpenDatabase(const char *path) {
		Cleanup();
		if (duckdb_open(path, &database) != DuckDBSuccess) {
			return false;
		}
		if (duckdb_connect(database, &connection) != DuckDBSuccess) {
			return false;
		}
		return true;
	}

	unique_ptr<CAPIResult> Query(string query) {
		auto result = make_unique<CAPIResult>();
		result->Query(connection, query);
		return result;
	}

	duckdb_database database = nullptr;
	duckdb_connection connection = nullptr;
};

TEST_CASE("Basic test of C API", "[capi]") {
	CAPITester tester;
	unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	// select scalar value
	result = tester.Query("SELECT CAST(42 AS BIGINT)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ColumnCount() == 1);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 42);
	REQUIRE(!result->IsNull(0, 0));

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
}

TEST_CASE("Test different types of C API", "[capi]") {
	CAPITester tester;
	unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	// integer columns
	vector<string> types = {"TINYINT", "SMALLINT", "INTEGER", "BIGINT"};
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
		REQUIRE(ApproxEqual(result->Fetch<float>(0, 0), 0.0f));
		REQUIRE(ApproxEqual(result->Fetch<double>(0, 0), 0.0));

		REQUIRE(!result->IsNull(0, 1));
		REQUIRE(result->Fetch<int8_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int16_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int32_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int64_t>(0, 1) == 1);
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
	duckdb_date date = result->Fetch<duckdb_date>(0, 1);
	REQUIRE(date.year == 1992);
	REQUIRE(date.month == 9);
	REQUIRE(date.day == 20);
	REQUIRE(result->Fetch<string>(0, 1) == Value::DATE(1992, 9, 20).ToString());
	date = result->Fetch<duckdb_date>(0, 2);
	REQUIRE(date.year == 30000);
	REQUIRE(date.month == 9);
	REQUIRE(date.day == 20);
	REQUIRE(result->Fetch<string>(0, 2) == Value::DATE(30000, 9, 20).ToString());

	// timestamp columns
	REQUIRE_NO_FAIL(tester.Query(
	    "CREATE TABLE timestamp (sec TIMESTAMP_S, milli TIMESTAMP_MS,micro TIMESTAMP_US, nano TIMESTAMP_NS );"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO timestamp VALUES (NULL,NULL,NULL,NULL )"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO timestamp VALUES ('1992-09-20 12:01:30','1992-09-20 "
	                             "12:01:30','1992-09-20 12:01:30','1992-09-20 12:01:30')"));

	result = tester.Query("SELECT * FROM timestamp ORDER BY sec");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	REQUIRE(result->IsNull(1, 0));
	REQUIRE(result->IsNull(2, 0));
	REQUIRE(result->IsNull(3, 0));
	for (idx_t i = 0; i < 4; i++) {
		duckdb_timestamp stamp = result->Fetch<duckdb_timestamp>(i, 1);
		REQUIRE(stamp.date.year == 1992);
		REQUIRE(stamp.date.month == 9);
		REQUIRE(stamp.date.day == 20);
		REQUIRE(stamp.time.hour == 12);
		REQUIRE(stamp.time.min == 1);
		REQUIRE(stamp.time.sec == 30);
		REQUIRE(stamp.time.micros == 0);
		auto result_string = result->Fetch<string>(i, 1);
		auto timestamp_string = Value::TIMESTAMP(1992, 9, 20, 12, 1, 30, 0).ToString();
		REQUIRE(result->Fetch<string>(i, 1) == timestamp_string);
	}

	// time columns
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE times(d TIME)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO times VALUES ('12:00:30.1234'), (NULL), ('02:30:01')"));

	result = tester.Query("SELECT * FROM times ORDER BY d");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
	duckdb_time time_val = result->Fetch<duckdb_time>(0, 1);
	REQUIRE(time_val.hour == 2);
	REQUIRE(time_val.min == 30);
	REQUIRE(time_val.sec == 1);
	REQUIRE(time_val.micros == 0);
	REQUIRE(result->Fetch<string>(0, 1) == Value::TIME(2, 30, 1, 0).ToString());
	time_val = result->Fetch<duckdb_time>(0, 2);
	REQUIRE(time_val.hour == 12);
	REQUIRE(time_val.min == 0);
	REQUIRE(time_val.sec == 30);
	REQUIRE(time_val.micros == 123400);
	REQUIRE(result->Fetch<string>(0, 2) == Value::TIME(12, 0, 30, 123400).ToString());

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
	REQUIRE(result->Fetch<string>(0, 2) == Value::BOOLEAN(true).ToString());
}

TEST_CASE("Test errors in C API", "[capi]") {
	CAPITester tester;
	unique_ptr<CAPIResult> result;

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
}

TEST_CASE("Test prepared statements in C API", "[capi]") {
	CAPITester tester;
	unique_ptr<CAPIResult> result;
	duckdb_result res;
	duckdb_prepared_statement stmt = nullptr;
	duckdb_state status;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	status = duckdb_prepare(tester.connection, "SELECT CAST($1 AS BIGINT)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	status = duckdb_bind_boolean(stmt, 1, 1);
	REQUIRE(status == DuckDBSuccess);
	status = duckdb_bind_boolean(stmt, 2, 1);
	REQUIRE(status == DuckDBError);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 1);
	duckdb_destroy_result(&res);

	duckdb_bind_int8(stmt, 1, 8);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 8);
	duckdb_destroy_result(&res);

	duckdb_bind_int16(stmt, 1, 16);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 16);
	duckdb_destroy_result(&res);

	duckdb_bind_int32(stmt, 1, 32);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 32);
	duckdb_destroy_result(&res);

	duckdb_bind_int64(stmt, 1, 64);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 64);
	duckdb_destroy_result(&res);

	duckdb_bind_float(stmt, 1, 42.0);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 42);
	duckdb_destroy_result(&res);

	duckdb_bind_double(stmt, 1, 43.0);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 43);
	duckdb_destroy_result(&res);

	duckdb_bind_varchar(stmt, 1, "44");
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 44);
	duckdb_destroy_result(&res);

	duckdb_bind_null(stmt, 1);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(res.columns[0].nullmask[0] == true);
	duckdb_destroy_result(&res);

	duckdb_destroy_prepare(&stmt);
	// again to make sure it does not crash
	duckdb_destroy_result(&res);
	duckdb_destroy_prepare(&stmt);

	status = duckdb_prepare(tester.connection, "SELECT CAST($1 AS VARCHAR)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	duckdb_bind_varchar_length(stmt, 1, "hello world", 5);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	auto value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "hello");
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_bind_blob(stmt, 1, "hello\0world", 11);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "hello\\x00world");
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_destroy_prepare(&stmt);

	status = duckdb_query(tester.connection, "CREATE TABLE a (i INTEGER)", NULL);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_prepare(tester.connection, "INSERT INTO a VALUES (?)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);
	idx_t nparams;
	REQUIRE(duckdb_nparams(stmt, &nparams) == DuckDBSuccess);
	REQUIRE(nparams == 1);

	for (int32_t i = 1; i <= 1000; i++) {
		duckdb_bind_int32(stmt, 1, i);
		status = duckdb_execute_prepared(stmt, nullptr);
		REQUIRE(status == DuckDBSuccess);
	}
	duckdb_destroy_prepare(&stmt);

	status = duckdb_prepare(tester.connection, "SELECT SUM(i)*$1-$2 FROM a", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);
	duckdb_bind_int32(stmt, 1, 2);
	duckdb_bind_int32(stmt, 2, 1000);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int32(&res, 0, 0) == 1000000);
	duckdb_destroy_result(&res);
	duckdb_destroy_prepare(&stmt);

	// not-so-happy path
	status = duckdb_prepare(tester.connection, "SELECT XXXXX", &stmt);
	REQUIRE(status == DuckDBError);
	duckdb_destroy_prepare(&stmt);

	status = duckdb_prepare(tester.connection, "SELECT CAST($1 AS INTEGER)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBError);
	duckdb_destroy_result(&res);
	duckdb_destroy_prepare(&stmt);

	// test duckdb_malloc explicitly
	auto malloced_data = duckdb_malloc(100);
	memcpy(malloced_data, "hello\0", 6);
	REQUIRE(string((char *)malloced_data) == "hello");
	duckdb_free(malloced_data);
}

TEST_CASE("Test appender statements in C API", "[capi]") {
	CAPITester tester;
	unique_ptr<CAPIResult> result;
	duckdb_state status;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE TABLE test (i INTEGER, d double, s string)");
	duckdb_appender appender;

	status = duckdb_appender_create(tester.connection, nullptr, "nonexistant-table", &appender);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_create(tester.connection, nullptr, "test", nullptr);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_create(tester.connection, nullptr, "test", &appender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_begin_row(appender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_int32(appender, 42);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_double(appender, 4.2);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_varchar(appender, "Hello, World");
	REQUIRE(status == DuckDBSuccess);

	// out of cols here
	status = duckdb_append_int32(appender, 42);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_end_row(appender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_flush(appender);
	REQUIRE(status == DuckDBSuccess);

	// we can flush again why not
	status = duckdb_appender_flush(appender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_close(appender);
	REQUIRE(status == DuckDBSuccess);

	result = tester.Query("SELECT * FROM test");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int32_t>(0, 0) == 42);
	REQUIRE(result->Fetch<double>(1, 0) == 4.2);
	REQUIRE(result->Fetch<string>(2, 0) == "Hello, World");

	status = duckdb_appender_destroy(&appender);
	REQUIRE(status == DuckDBSuccess);

	// this has been destroyed

	status = duckdb_appender_close(appender);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_flush(appender);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_begin_row(appender);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_end_row(appender);
	REQUIRE(status == DuckDBError);

	status = duckdb_append_int32(appender, 42);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_destroy(&appender);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_close(nullptr);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_flush(nullptr);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_begin_row(nullptr);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_end_row(nullptr);
	REQUIRE(status == DuckDBError);

	status = duckdb_append_int32(nullptr, 42);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_destroy(nullptr);
	REQUIRE(status == DuckDBError);

	status = duckdb_appender_destroy(nullptr);
	REQUIRE(status == DuckDBError);

	// many types
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE many_types(bool boolean, t TINYINT, s SMALLINT, b BIGINT, ut UTINYINT, "
	                             "us USMALLINT, ui UINTEGER, ub UBIGINT, uf REAL, ud DOUBLE, txt VARCHAR, blb BLOB)"));
	duckdb_appender tappender;

	status = duckdb_appender_create(tester.connection, nullptr, "many_types", &tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_begin_row(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_bool(tappender, true);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_int8(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_int16(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_int64(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_uint8(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_uint16(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_uint32(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_uint64(tappender, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_float(tappender, 0.5f);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_varchar_length(tappender, "hello world", 5);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_blob(tappender, "hello", 5);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_end_row(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_begin_row(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_null(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_end_row(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_flush(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_close(tappender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_destroy(&tappender);
	REQUIRE(status == DuckDBSuccess);

	result = tester.Query("SELECT * FROM many_types");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<bool>(0, 0) == true);
	REQUIRE(result->Fetch<int8_t>(1, 0) == 1);
	REQUIRE(result->Fetch<int16_t>(2, 0) == 1);
	REQUIRE(result->Fetch<int64_t>(3, 0) == 1);
	REQUIRE(result->Fetch<uint8_t>(4, 0) == 1);
	REQUIRE(result->Fetch<uint16_t>(5, 0) == 1);
	REQUIRE(result->Fetch<uint32_t>(6, 0) == 1);
	REQUIRE(result->Fetch<uint64_t>(7, 0) == 1);
	REQUIRE(result->Fetch<float>(8, 0) == 0.5f);
	REQUIRE(result->IsNull(9, 0));
	REQUIRE(result->Fetch<string>(10, 0) == "hello");

	auto blob = duckdb_value_blob(&result->InternalResult(), 11, 0);
	REQUIRE(blob.size == 5);
	REQUIRE(memcmp(blob.data, "hello", 5) == 0);
	duckdb_free(blob.data);

	REQUIRE(result->IsNull(0, 1));
	REQUIRE(result->IsNull(1, 1));
	REQUIRE(result->IsNull(2, 1));
	REQUIRE(result->IsNull(3, 1));
	REQUIRE(result->IsNull(4, 1));
	REQUIRE(result->IsNull(5, 1));
	REQUIRE(result->IsNull(6, 1));
	REQUIRE(result->IsNull(7, 1));
	REQUIRE(result->IsNull(8, 1));
	REQUIRE(result->IsNull(9, 1));
	REQUIRE(result->IsNull(10, 1));
	REQUIRE(result->IsNull(11, 1));
}

TEST_CASE("Test arrow in C API", "[capi]") {
	CAPITester tester;
	unique_ptr<CAPIResult> result;
	duckdb_prepared_statement stmt = nullptr;
	duckdb_arrow arrow_result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	// test query arrow
	{
		REQUIRE(duckdb_query_arrow(tester.connection, "SELECT 42 AS VALUE", &arrow_result) == DuckDBSuccess);

		// query schema
		ArrowSchema *arrow_schema = new ArrowSchema();
		REQUIRE(duckdb_query_arrow_schema(arrow_result, (duckdb_arrow_schema *)&arrow_schema) == DuckDBSuccess);
		REQUIRE(string(arrow_schema->name) == "duckdb_query_result");
		// User need to release the data themselves
		arrow_schema->release(arrow_schema);
		delete arrow_schema;

		// query array data
		ArrowArray *arrow_array = new ArrowArray();
		REQUIRE(duckdb_query_arrow_array(arrow_result, (duckdb_arrow_array *)&arrow_array) == DuckDBSuccess);
		REQUIRE(arrow_array->length == 1);
		arrow_array->release(arrow_array);
		delete arrow_array;

		duckdb_arrow_array null_array = nullptr;
		REQUIRE(duckdb_query_arrow_array(arrow_result, &null_array) == DuckDBSuccess);
		REQUIRE(null_array == nullptr);

		// destroy result
		duckdb_destroy_arrow(&arrow_result);
	}

	// test multiple chunks
	{
		// create table that consists of multiple chunks
		REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test(a INTEGER)"));
		for (size_t i = 0; i < 500; i++) {
			REQUIRE_NO_FAIL(
			    tester.Query("INSERT INTO test VALUES (1); INSERT INTO test VALUES (2); INSERT INTO test VALUES "
			                 "(3); INSERT INTO test VALUES (4); INSERT INTO test VALUES (5);"));
		}
		REQUIRE_NO_FAIL(tester.Query("COMMIT"));

		REQUIRE(duckdb_query_arrow(tester.connection, "SELECT CAST(a AS INTEGER) AS a FROM test ORDER BY a",
		                           &arrow_result) == DuckDBSuccess);

		ArrowSchema *arrow_schema = new ArrowSchema();
		REQUIRE(duckdb_query_arrow_schema(arrow_result, (duckdb_arrow_schema *)&arrow_schema) == DuckDBSuccess);
		REQUIRE(arrow_schema->release != nullptr);
		arrow_schema->release(arrow_schema);
		delete arrow_schema;

		int total_count = 0;
		while (true) {
			ArrowArray *arrow_array = new ArrowArray();
			REQUIRE(duckdb_query_arrow_array(arrow_result, (duckdb_arrow_array *)&arrow_array) == DuckDBSuccess);
			if (arrow_array->length == 0) {
				delete arrow_array;
				REQUIRE(total_count == 2500);
				break;
			}
			REQUIRE(arrow_array->length > 0);
			total_count += arrow_array->length;
			arrow_array->release(arrow_array);
			delete arrow_array;
		}
		duckdb_destroy_arrow(&arrow_result);
	}

	// test prepare query arrow
	{
		REQUIRE(duckdb_prepare(tester.connection, "SELECT CAST($1 AS BIGINT)", &stmt) == DuckDBSuccess);
		REQUIRE(stmt != nullptr);
		REQUIRE(duckdb_bind_int64(stmt, 1, 42) == DuckDBSuccess);
		REQUIRE(duckdb_execute_prepared_arrow(stmt, &arrow_result) == DuckDBSuccess);

		ArrowSchema *arrow_schema = new ArrowSchema();
		REQUIRE(duckdb_query_arrow_schema(arrow_result, (duckdb_arrow_schema *)&arrow_schema) == DuckDBSuccess);
		REQUIRE(string(arrow_schema->format) == "+s");
		arrow_schema->release(arrow_schema);
		delete arrow_schema;

		ArrowArray *arrow_array = new ArrowArray();
		REQUIRE(duckdb_query_arrow_array(arrow_result, (duckdb_arrow_array *)&arrow_array) == DuckDBSuccess);
		REQUIRE(arrow_array->length == 1);
		arrow_array->release(arrow_array);
		delete arrow_array;

		duckdb_destroy_arrow(&arrow_result);
		duckdb_destroy_prepare(&stmt);
	}
}
