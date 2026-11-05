#include "capi_tester.hpp"
#include "result_wait_helpers.hpp"
#include <atomic>
#include <random>
#include <thread>

using namespace duckdb;

TEST_CASE("Test prepared statements in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_result res;
	duckdb_prepared_statement stmt = nullptr;
	duckdb_state status;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	status = duckdb_prepare(tester.connection, "SELECT CAST($1 AS BIGINT)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	REQUIRE(duckdb_prepared_statement_column_count(stmt) == 1);
	REQUIRE(duckdb_prepared_statement_column_type(stmt, 0) == DUCKDB_TYPE_BIGINT);
	auto logical_type = duckdb_prepared_statement_column_logical_type(stmt, 0);
	REQUIRE(logical_type);
	REQUIRE(duckdb_get_type_id(logical_type) == DUCKDB_TYPE_BIGINT);
	duckdb_destroy_logical_type(&logical_type);

	status = duckdb_bind_boolean(stmt, 1, true);
	REQUIRE(status == DuckDBSuccess);

	// Parameter index 2 is out of bounds
	status = duckdb_bind_boolean(stmt, 2, true);
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

	duckdb_bind_hugeint(stmt, 1, duckdb_double_to_hugeint(64));
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_hugeint_to_double(duckdb_value_hugeint(&res, 0, 0)) == 64.0);
	duckdb_destroy_result(&res);

	duckdb_bind_uhugeint(stmt, 1, duckdb_double_to_uhugeint(64));
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_uhugeint_to_double(duckdb_value_uhugeint(&res, 0, 0)) == 64.0);
	duckdb_destroy_result(&res);

	// Fetching a DECIMAL from a non-DECIMAL result returns 0
	duckdb_decimal decimal = duckdb_double_to_decimal(634.3453, 7, 4);
	duckdb_bind_decimal(stmt, 1, decimal);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	duckdb_decimal result_decimal = duckdb_value_decimal(&res, 0, 0);
	REQUIRE(result_decimal.scale == 0);
	REQUIRE(result_decimal.width == 0);
	REQUIRE(result_decimal.value.upper == 0);
	REQUIRE(result_decimal.value.lower == 0);
	duckdb_destroy_result(&res);

	duckdb_bind_uint8(stmt, 1, 8);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_uint8(&res, 0, 0) == 8);
	duckdb_destroy_result(&res);

	duckdb_bind_uint16(stmt, 1, 8);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_uint16(&res, 0, 0) == 8);
	duckdb_destroy_result(&res);

	duckdb_bind_uint32(stmt, 1, 8);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_uint32(&res, 0, 0) == 8);
	duckdb_destroy_result(&res);

	duckdb_bind_uint64(stmt, 1, 8);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_uint64(&res, 0, 0) == 8);
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

	REQUIRE(duckdb_bind_float(stmt, 1, NAN) == DuckDBSuccess);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBError);
	duckdb_destroy_result(&res);

	REQUIRE(duckdb_bind_double(stmt, 1, NAN) == DuckDBSuccess);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBError);
	duckdb_destroy_result(&res);

	REQUIRE(duckdb_bind_varchar(stmt, 1, "\x80\x40\x41") == DuckDBError);
	duckdb_bind_varchar(stmt, 1, "44");
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 44);
	duckdb_destroy_result(&res);

	duckdb_bind_null(stmt, 1);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_nullmask_data(&res, 0)[0] == true);
	duckdb_destroy_result(&res);

	duckdb_destroy_prepare(&stmt);
	// again to make sure it does not crash
	duckdb_destroy_result(&res);
	duckdb_destroy_prepare(&stmt);

	status = duckdb_prepare(tester.connection, "SELECT CAST($1 AS VARCHAR)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	// invalid unicode
	REQUIRE(duckdb_bind_varchar_length(stmt, 1, "\x80", 1) == DuckDBError);
	// we can bind null values, though!
	REQUIRE(duckdb_bind_varchar_length(stmt, 1, "\x00\x40\x41", 3) == DuckDBSuccess);
	duckdb_bind_varchar_length(stmt, 1, "hello world", 5);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	auto value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "hello");
	REQUIRE(duckdb_value_int8(&res, 0, 0) == 0);
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_bind_blob(stmt, 1, "hello\0world", 11);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "hello\\x00world");
	REQUIRE(duckdb_value_int8(&res, 0, 0) == 0);
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_date_struct date_struct;
	date_struct.year = 1992;
	date_struct.month = 9;
	date_struct.day = 3;

	duckdb_bind_date(stmt, 1, duckdb_to_date(date_struct));
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "1992-09-03");
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_time_struct time_struct;
	time_struct.hour = 12;
	time_struct.min = 22;
	time_struct.sec = 33;
	time_struct.micros = 123400;

	duckdb_bind_time(stmt, 1, duckdb_to_time(time_struct));
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "12:22:33.1234");
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_timestamp_struct ts;
	ts.date = date_struct;
	ts.time = time_struct;

	duckdb_bind_timestamp(stmt, 1, duckdb_to_timestamp(ts));
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "1992-09-03 12:22:33.1234");
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_bind_timestamp_tz(stmt, 1, duckdb_to_timestamp(ts));
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(StringUtil::Contains(string(value), "1992-09"));
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_interval interval;
	interval.months = 3;
	interval.days = 0;
	interval.micros = 0;

	duckdb_bind_interval(stmt, 1, interval);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	value = duckdb_value_varchar(&res, 0, 0);
	REQUIRE(string(value) == "3 months");
	duckdb_free(value);
	duckdb_destroy_result(&res);

	duckdb_destroy_prepare(&stmt);

	status = duckdb_query(tester.connection, "CREATE TABLE a (i INTEGER)", NULL);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_prepare(tester.connection, "INSERT INTO a VALUES (?)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);
	REQUIRE(duckdb_nparams(nullptr) == 0);
	REQUIRE(duckdb_nparams(stmt) == 1);
	REQUIRE(duckdb_param_type(nullptr, 0) == DUCKDB_TYPE_INVALID);
	REQUIRE(duckdb_param_type(stmt, 0) == DUCKDB_TYPE_INVALID);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_INTEGER);
	REQUIRE(duckdb_param_type(stmt, 2) == DUCKDB_TYPE_INVALID);

	for (int32_t i = 1; i <= 1000; i++) {
		duckdb_bind_int32(stmt, 1, i);
		status = duckdb_execute_prepared(stmt, nullptr);
		REQUIRE(status == DuckDBSuccess);
	}
	duckdb_destroy_prepare(&stmt);

	status = duckdb_prepare(tester.connection, "SELECT SUM(i)*$1-$2 FROM a", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);
	// clear bindings
	duckdb_bind_int32(stmt, 1, 2);
	REQUIRE(duckdb_clear_bindings(stmt) == DuckDBSuccess);

	// bind again will succeed
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

	REQUIRE(duckdb_prepared_statement_column_count(stmt) == 1);
	REQUIRE(duckdb_prepared_statement_column_type(stmt, 0) == DUCKDB_TYPE_INTEGER);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBError);
	duckdb_destroy_result(&res);
	duckdb_destroy_prepare(&stmt);

	// test duckdb_malloc explicitly
	auto malloced_data = duckdb_malloc(100);
	memcpy(malloced_data, "hello\0", 6);
	REQUIRE(string((char *)malloced_data) == "hello");
	duckdb_free(malloced_data);

	status = duckdb_prepare(tester.connection, "SELECT sum(i) FROM a WHERE i > ?", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);
	REQUIRE(duckdb_nparams(stmt) == 1);
	REQUIRE(duckdb_param_type(nullptr, 0) == DUCKDB_TYPE_INVALID);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_INTEGER);

	REQUIRE(duckdb_prepared_statement_column_count(stmt) == 1);
	REQUIRE(duckdb_prepared_statement_column_type(stmt, 0) == DUCKDB_TYPE_HUGEINT);

	duckdb_destroy_prepare(&stmt);
}

TEST_CASE("Test duckdb_prepared_statement return value APIs", "[capi]") {
	duckdb_database db;
	duckdb_connection conn;
	duckdb_prepared_statement stmt;
	REQUIRE(duckdb_open("", &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

	// Unambiguous return column types
	REQUIRE(duckdb_prepare(conn, "select $1::TEXT, $2::integer, $3::BOOLEAN, $4::FLOAT, $5::DOUBLE", &stmt) ==
	        DuckDBSuccess);

	REQUIRE(duckdb_prepared_statement_column_count(stmt) == 5);
	auto expected_types = {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_INTEGER, DUCKDB_TYPE_BOOLEAN, DUCKDB_TYPE_FLOAT,
	                       DUCKDB_TYPE_DOUBLE};

	for (idx_t i = 0; i < 5; i++) {
		REQUIRE(duckdb_prepared_statement_column_type(stmt, i) == *std::next(expected_types.begin(), i));
		auto logical_type = duckdb_prepared_statement_column_logical_type(stmt, i);
		REQUIRE(logical_type);
		REQUIRE(duckdb_get_type_id(logical_type) == *std::next(expected_types.begin(), i));
		duckdb_destroy_logical_type(&logical_type);
	}

	auto column_name = duckdb_prepared_statement_column_name(stmt, 0);
	std::string col_name_str = column_name;
	duckdb_free((void *)column_name);
	REQUIRE(col_name_str == "CAST($1 AS TEXT)");

	duckdb_destroy_prepare(&stmt);

	// Return columns contain ambiguous types
	REQUIRE(duckdb_prepare(conn, "select $1::TEXT, $2::integer, $3, $4::BOOLEAN, $5::FLOAT, $6::DOUBLE", &stmt) ==
	        DuckDBSuccess);

	REQUIRE(duckdb_prepared_statement_column_count(stmt) == 1);
	REQUIRE(duckdb_prepared_statement_column_type(stmt, 0) == DUCKDB_TYPE_INVALID);

	auto logical_type = duckdb_prepared_statement_column_logical_type(stmt, 0);
	REQUIRE(logical_type);
	REQUIRE(duckdb_get_type_id(logical_type) == DUCKDB_TYPE_INVALID);
	duckdb_destroy_logical_type(&logical_type);

	auto col_name_ptr = duckdb_prepared_statement_column_name(stmt, 0);
	col_name_str = col_name_ptr;
	duckdb_free((void *)col_name_ptr);
	REQUIRE(col_name_str == "unknown");
	REQUIRE(duckdb_prepared_statement_column_name(stmt, 1) == nullptr);
	REQUIRE(duckdb_prepared_statement_column_name(stmt, 5) == nullptr);

	duckdb_destroy_prepare(&stmt);
	duckdb_disconnect(&conn);
	duckdb_close(&db);
}

TEST_CASE("Test duckdb_param_type and duckdb_param_logical_type", "[capi]") {
	duckdb_database db;
	duckdb_connection conn;
	duckdb_prepared_statement stmt;

	REQUIRE(duckdb_open("", &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);
	REQUIRE(duckdb_prepare(conn, "select $1::integer, $2::integer", &stmt) == DuckDBSuccess);

	auto logical_type = duckdb_param_logical_type(stmt, 2);
	REQUIRE(logical_type);
	REQUIRE(duckdb_get_type_id(logical_type) == DUCKDB_TYPE_INTEGER);
	duckdb_destroy_logical_type(&logical_type);

	REQUIRE(duckdb_param_type(stmt, 2) == DUCKDB_TYPE_INTEGER);
	REQUIRE(duckdb_bind_null(stmt, 1) == DuckDBSuccess);
	REQUIRE(duckdb_bind_int32(stmt, 2, 10) == DuckDBSuccess);

	REQUIRE(!duckdb_param_logical_type(nullptr, 2));
	REQUIRE(duckdb_param_type(nullptr, 2) == DUCKDB_TYPE_INVALID);
	REQUIRE(!duckdb_param_logical_type(stmt, 2000));
	REQUIRE(duckdb_param_type(stmt, 2000) == DUCKDB_TYPE_INVALID);

	duckdb_result result;
	REQUIRE(duckdb_execute_prepared(stmt, &result) == DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 2) == DUCKDB_TYPE_INTEGER);
	duckdb_clear_bindings(stmt);
	duckdb_destroy_result(&result);

	duckdb_destroy_prepare(&stmt);
	duckdb_disconnect(&conn);
	duckdb_close(&db);
}

TEST_CASE("Test duckdb_param_type with nested casts", "[capi]") {
	duckdb_database db;
	duckdb_connection conn;
	duckdb_prepared_statement stmt;

	REQUIRE(duckdb_open("", &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

	// Single cast: parameter type is the inner cast target.
	REQUIRE(duckdb_prepare(conn, "SELECT CAST($1 AS INTEGER)", &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_INTEGER);
	duckdb_destroy_prepare(&stmt);

	// Nested cast: parameter type should still be the innermost cast target.
	REQUIRE(duckdb_prepare(conn, "SELECT CAST(CAST($1 AS INTEGER) AS VARCHAR)", &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_INTEGER);
	duckdb_destroy_prepare(&stmt);

	REQUIRE(duckdb_prepare(conn, "SELECT CAST(CAST($1 AS TIMESTAMPTZ) AS VARCHAR)", &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_TIMESTAMP_TZ);
	duckdb_destroy_prepare(&stmt);

	// Triple-nested cast: still pinned by the innermost target.
	REQUIRE(duckdb_prepare(conn, "SELECT CAST(CAST(CAST($1 AS INTEGER) AS BIGINT) AS VARCHAR)", &stmt) ==
	        DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_INTEGER);
	duckdb_destroy_prepare(&stmt);

	// Multiple independent casts on the same parameter remain ambiguous and invalidate.
	REQUIRE(duckdb_prepare(conn, "SELECT $1::INTEGER + $1::BIGINT", &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_INVALID);
	duckdb_destroy_prepare(&stmt);

	duckdb_disconnect(&conn);
	duckdb_close(&db);
}

TEST_CASE("Test prepared statements with named parameters in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_result res;
	duckdb_prepared_statement stmt = nullptr;
	duckdb_state status;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	status = duckdb_prepare(tester.connection, "SELECT CAST($my_val AS BIGINT)", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	idx_t parameter_index;
	// test invalid name
	status = duckdb_bind_parameter_index(stmt, &parameter_index, "invalid");
	REQUIRE(status == DuckDBError);

	status = duckdb_bind_parameter_index(stmt, &parameter_index, "my_val");
	REQUIRE(status == DuckDBSuccess);

	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_BIGINT);

	auto logical_type = duckdb_param_logical_type(stmt, 1);
	REQUIRE(logical_type);
	REQUIRE(duckdb_get_type_id(logical_type) == DUCKDB_TYPE_BIGINT);
	duckdb_destroy_logical_type(&logical_type);

	idx_t param_count = duckdb_nparams(stmt);
	duckdb::vector<string> names;
	for (idx_t i = 0; i < param_count; i++) {
		auto name = duckdb_parameter_name(stmt, i + 1);
		names.push_back(std::string(name));
		duckdb_free((void *)name);
	}

	REQUIRE(duckdb_parameter_name(stmt, 0) == (const char *)NULL);
	REQUIRE(duckdb_parameter_name(stmt, 2) == (const char *)NULL);

	REQUIRE(duckdb_prepared_statement_column_count(stmt) == 1);
	REQUIRE(duckdb_prepared_statement_column_type(stmt, 0) == DUCKDB_TYPE_BIGINT);

	duckdb::vector<string> expected_names = {"my_val"};
	REQUIRE(names.size() == expected_names.size());
	for (idx_t i = 0; i < expected_names.size(); i++) {
		auto &name = names[i];
		auto &expected_name = expected_names[i];
		REQUIRE(name == expected_name);
	}

	status = duckdb_bind_boolean(stmt, parameter_index, 1);
	REQUIRE(status == DuckDBSuccess);
	status = duckdb_bind_boolean(stmt, parameter_index + 1, 1);
	REQUIRE(status == DuckDBError);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 1);
	duckdb_destroy_result(&res);

	// Clear the bindings, don't rebind the parameter index
	status = duckdb_clear_bindings(stmt);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_bind_boolean(stmt, parameter_index, 1);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 1);
	duckdb_destroy_result(&res);

	duckdb_destroy_prepare(&stmt);
}

TEST_CASE("Maintain prepared statement types", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_result res;
	duckdb_prepared_statement stmt = nullptr;
	duckdb_state status;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	status = duckdb_prepare(tester.connection, "select cast(111 as short) * $1", &stmt);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(stmt != nullptr);

	status = duckdb_bind_int64(stmt, 1, 1665);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 184815);
	duckdb_destroy_result(&res);
	duckdb_destroy_prepare(&stmt);
}

TEST_CASE("Test duckdb_parameter_name", "[capi]") {
	CAPITester tester;
	duckdb_prepared_statement stmt;

	REQUIRE(tester.OpenDatabase(nullptr));

	SECTION("Contiguous positional parameters") {
		REQUIRE(duckdb_prepare(tester.connection, "SELECT $1, $2, $3", &stmt) == DuckDBSuccess);
		REQUIRE(duckdb_nparams(stmt) == 3);

		const char *name;
		name = duckdb_parameter_name(stmt, 1);
		REQUIRE(string(name) == "1");
		duckdb_free((void *)name);

		name = duckdb_parameter_name(stmt, 2);
		REQUIRE(string(name) == "2");
		duckdb_free((void *)name);

		name = duckdb_parameter_name(stmt, 3);
		REQUIRE(string(name) == "3");
		duckdb_free((void *)name);

		REQUIRE(duckdb_parameter_name(stmt, 4) == nullptr);

		duckdb_destroy_prepare(&stmt);
	}

	SECTION("Uncontiguous parameters") {
		REQUIRE(duckdb_prepare(tester.connection, "SELECT $1, $10", &stmt) == DuckDBSuccess);

		REQUIRE(duckdb_nparams(stmt) == 2);

		const char *name;
		name = duckdb_parameter_name(stmt, 1);
		REQUIRE(string(name) == "1");
		duckdb_free((void *)name);

		name = duckdb_parameter_name(stmt, 10);
		REQUIRE(string(name) == "10");
		duckdb_free((void *)name);

		// Non-existing indexes
		REQUIRE(duckdb_parameter_name(stmt, 2) == nullptr);
		REQUIRE(duckdb_parameter_name(stmt, 11) == nullptr);

		duckdb_destroy_prepare(&stmt);
	}

	SECTION("Named parameters") {
		REQUIRE(duckdb_prepare(tester.connection, "SELECT $foo, $bar", &stmt) == DuckDBSuccess);
		REQUIRE(duckdb_nparams(stmt) == 2);

		const char *name;
		// Named parameters are starting from 1
		name = duckdb_parameter_name(stmt, 1);
		REQUIRE(string(name) == "foo");
		duckdb_free((void *)name);

		name = duckdb_parameter_name(stmt, 2);
		REQUIRE(string(name) == "bar");
		duckdb_free((void *)name);

		duckdb_destroy_prepare(&stmt);
	}
}

TEST_CASE("Prepared streaming result", "[capi]") {
	CAPITester tester;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	SECTION("non streaming result") {
		REQUIRE(tester.Query("CREATE TABLE t2 (i INTEGER, j INTEGER);"));

		duckdb_prepared_statement stmt;
		REQUIRE(duckdb_prepare(tester.connection,
		                       "INSERT INTO t2  SELECT 2 AS i, 3 AS j  RETURNING *, i * j AS i_times_j",
		                       &stmt) == DuckDBSuccess);
		duckdb_result res;
		REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
		REQUIRE(!duckdb_result_is_streaming(res));
		duckdb_destroy_result(&res);
		duckdb_destroy_prepare(&stmt);
	}

	SECTION("streaming result") {
		duckdb_prepared_statement stmt;
		REQUIRE(duckdb_prepare(tester.connection, "FROM RANGE(0, 10)", &stmt) == DuckDBSuccess);

		duckdb_result res;
		REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
		REQUIRE(duckdb_result_is_streaming(res));

		duckdb_data_chunk chunk;
		idx_t index = 0;
		while (true) {
			chunk = duckdb_stream_fetch_chunk(res);
			if (!chunk) {
				break;
			}
			auto chunk_size = duckdb_data_chunk_get_size(chunk);
			REQUIRE(chunk_size > 0);

			auto vec = duckdb_data_chunk_get_vector(chunk, 0);
			auto column_type = duckdb_vector_get_column_type(vec);
			REQUIRE(duckdb_get_type_id(column_type) == DUCKDB_TYPE_BIGINT);
			duckdb_destroy_logical_type(&column_type);

			auto data = reinterpret_cast<int64_t *>(duckdb_vector_get_data(vec));
			for (idx_t i = 0; i < chunk_size; i++) {
				REQUIRE(data[i] == int64_t(index + i));
			}
			index += chunk_size;
			duckdb_destroy_data_chunk(&chunk);
		}

		REQUIRE(duckdb_stream_fetch_chunk(res) == nullptr);

		duckdb_destroy_result(&res);
		duckdb_destroy_prepare(&stmt);
	}

	SECTION("an open stream holds the connection until it ends") {
		duckdb_prepared_statement stmt;
		REQUIRE(duckdb_prepare(tester.connection, "FROM range(100000)", &stmt) == DuckDBSuccess);
		duckdb_result res;
		REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
		REQUIRE(duckdb_result_is_streaming(res));
		auto chunk = duckdb_stream_fetch_chunk(res);
		REQUIRE(chunk);
		duckdb_destroy_data_chunk(&chunk);

		auto refused = tester.Query("SELECT 84");
		REQUIRE(refused->HasError());
		REQUIRE(refused->ErrorType() == DUCKDB_ERROR_RESOURCE_IN_USE);
		REQUIRE(string(refused->ErrorMessage()).find("connection has an open result") != string::npos);

		// fetched to its end, the stream releases the connection
		while ((chunk = duckdb_stream_fetch_chunk(res))) {
			duckdb_destroy_data_chunk(&chunk);
		}
		REQUIRE(duckdb_result_error(&res) == nullptr);
		REQUIRE_NO_FAIL(tester.Query("SELECT 84"));
		duckdb_destroy_result(&res);

		// and so does destroying it unread
		REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
		REQUIRE(tester.Query("SELECT 84")->HasError());
		duckdb_destroy_result(&res);
		REQUIRE_NO_FAIL(tester.Query("SELECT 84"));
		duckdb_destroy_prepare(&stmt);
	}

	SECTION("streaming extracted statements") {
		duckdb_extracted_statements stmts;
		auto n_statements = duckdb_extract_statements(tester.connection, "Select 1; Select 2;", &stmts);
		REQUIRE(n_statements == 2);

		for (idx_t i = 0; i < n_statements; i++) {
			duckdb_prepared_statement stmt;
			REQUIRE(duckdb_prepare_extracted_statement(tester.connection, stmts, i, &stmt) == DuckDBSuccess);

			duckdb_result res;
			REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
			REQUIRE(duckdb_result_is_streaming(res));

			duckdb_data_chunk chunk;
			chunk = duckdb_stream_fetch_chunk(res);
			REQUIRE(chunk != nullptr);
			REQUIRE(duckdb_data_chunk_get_size(chunk) == 1);

			auto vec = duckdb_data_chunk_get_vector(chunk, 0);

			auto type = duckdb_vector_get_column_type(vec);
			REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_INTEGER);
			duckdb_destroy_logical_type(&type);

			auto data = (int32_t *)duckdb_vector_get_data(vec);
			REQUIRE(data[0] == (int32_t)(i + 1));

			REQUIRE(duckdb_stream_fetch_chunk(res) == nullptr);

			duckdb_destroy_data_chunk(&chunk);
			duckdb_destroy_result(&res);
			duckdb_destroy_prepare(&stmt);
		}

		duckdb_destroy_extracted(&stmts);
	}
}

TEST_CASE("Only a SELECT streams through the streaming entry points", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t(i INTEGER)"));
	REQUIRE_NO_FAIL(tester.Query("PREPARE select_42 AS SELECT 42"));
	REQUIRE_NO_FAIL(tester.Query("CREATE SEQUENCE seq"));

	struct Case {
		const char *sql;
		bool streams;
		int64_t rows;
	};
	// rows is checked on a retained result only, -1 skips the check
	const Case cases[] = {
	    {"SELECT 42", true, -1},
	    {"FROM range(3)", true, -1},
	    {"SHOW TABLES", true, -1},
	    {"DESCRIBE t", true, -1},
	    {"PRAGMA version", true, -1},
	    {"EXPLAIN SELECT 42", false, -1},
	    {"EXPLAIN ANALYZE SELECT 42", false, -1},
	    {"WITH ins AS (INSERT INTO t VALUES (4) RETURNING i) SELECT i FROM ins", true, -1},
	    {"SELECT nextval('seq')", true, -1},
	    {"EXECUTE select_42", false, 1},
	    {"INSERT INTO t VALUES (1), (2) RETURNING i", false, 2},
	    {"CALL pragma_version()", false, 1},
	    {"CHECKPOINT", false, -1},
	    {"INSERT INTO t VALUES (3)", false, -1},
	    {"CREATE OR REPLACE TABLE c AS SELECT 1 AS i", false, -1},
	};
	for (bool pending : {false, true}) {
		for (auto &entry : cases) {
			INFO((pending ? "duckdb_execute_pending: " : "duckdb_execute_prepared_streaming: ") << entry.sql);
			duckdb_prepared_statement stmt;
			REQUIRE(duckdb_prepare(tester.connection, entry.sql, &stmt) == DuckDBSuccess);
			duckdb_result res;
			if (pending) {
				duckdb_pending_result pending_res;
				REQUIRE(duckdb_pending_prepared_streaming(stmt, &pending_res) == DuckDBSuccess);
				REQUIRE(duckdb_execute_pending(pending_res, &res) == DuckDBSuccess);
				duckdb_destroy_pending(&pending_res);
			} else {
				REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
			}
			REQUIRE(duckdb_result_is_streaming(res) == entry.streams);
			if (!entry.streams && entry.rows >= 0) {
				REQUIRE(duckdb_row_count(&res) == idx_t(entry.rows));
			}
			// Destroyed unread: a streamed statement is abandoned, any other one has applied its effects already
			duckdb_destroy_result(&res);
			duckdb_destroy_prepare(&stmt);
		}
	}
	auto rows = tester.Query("SELECT count(*) FROM t");
	REQUIRE(rows->Fetch<int64_t>(0, 0) == 6);
}

TEST_CASE("A statement that is not streamed still returns its rows through the stream reader", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("PREPARE select_5000 AS SELECT range AS i FROM range(5000)"));

	struct Case {
		const char *sql;
		idx_t rows;
	};
	const Case cases[] = {
	    {"SELECT range FROM range(5000)", 5000},
	    {"EXECUTE select_5000", 5000},
	    {"EXPLAIN SELECT 42", 1},
	    {"CALL range(3000)", 3000},
	};
	for (bool pending : {false, true}) {
		for (auto &entry : cases) {
			INFO((pending ? "duckdb_execute_pending: " : "duckdb_execute_prepared_streaming: ") << entry.sql);
			duckdb_prepared_statement stmt;
			REQUIRE(duckdb_prepare(tester.connection, entry.sql, &stmt) == DuckDBSuccess);
			duckdb_result res;
			if (pending) {
				duckdb_pending_result pending_res;
				REQUIRE(duckdb_pending_prepared_streaming(stmt, &pending_res) == DuckDBSuccess);
				REQUIRE(duckdb_execute_pending(pending_res, &res) == DuckDBSuccess);
				duckdb_destroy_pending(&pending_res);
			} else {
				REQUIRE(duckdb_execute_prepared_streaming(stmt, &res) == DuckDBSuccess);
			}
			idx_t rows = 0;
			while (auto chunk = duckdb_stream_fetch_chunk(res)) {
				rows += duckdb_data_chunk_get_size(chunk);
				duckdb_destroy_data_chunk(&chunk);
			}
			REQUIRE(duckdb_result_error(&res) == nullptr);
			REQUIRE(rows == entry.rows);
			duckdb_destroy_result(&res);
			duckdb_destroy_prepare(&stmt);
		}
	}
}

TEST_CASE("A streaming execution without an out_result keeps what the statement wrote", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t(i BIGINT)"));
	const char *sql = "WITH ins AS (INSERT INTO t SELECT range FROM range(100000) RETURNING i) SELECT i FROM ins";

	bool in_transaction = false;
	SECTION("autocommit") {
	}
	SECTION("in a transaction") {
		in_transaction = true;
		REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
	}
	duckdb_prepared_statement stmt;
	REQUIRE(duckdb_prepare(tester.connection, sql, &stmt) == DuckDBSuccess);
	// No result reaches the caller, so the statement is completed instead of streamed and dropped unread
	REQUIRE(duckdb_execute_prepared_streaming(stmt, nullptr) == DuckDBSuccess);
	duckdb_destroy_prepare(&stmt);
	if (in_transaction) {
		REQUIRE_NO_FAIL(tester.Query("COMMIT"));
	}
	auto count = tester.Query("SELECT count(*) FROM t");
	REQUIRE(count->Fetch<int64_t>(0, 0) == 100000);
}

TEST_CASE("Test STRING LITERAL parameter type", "[capi]") {
	duckdb_database db;
	duckdb_connection conn;
	duckdb_prepared_statement stmt;

	REQUIRE(duckdb_open("", &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

	REQUIRE(duckdb_prepare(conn, "SELECT ?", &stmt) == DuckDBSuccess);
	REQUIRE(duckdb_bind_varchar(stmt, 1, "a") == DuckDBSuccess);
	REQUIRE(duckdb_param_type(stmt, 1) == DUCKDB_TYPE_STRING_LITERAL);
	duckdb_destroy_prepare(&stmt);

	duckdb_disconnect(&conn);
	duckdb_close(&db);
}

TEST_CASE("Test concurrent prepared statement execution race condition MRE", "[capi]") {
	// This test is a minimal reproducible example for the race condition described in #7187 (internal).
	duckdb_database db;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);

	duckdb_connection conn;
	REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

	std::atomic<idx_t> failures {0};
	std::atomic<idx_t> completed {0};

	constexpr idx_t NUM_THREADS = 4;
	constexpr idx_t ITERATIONS = 1000;

	duckdb::vector<std::thread> threads;
	for (idx_t t = 0; t < NUM_THREADS; t++) {
		threads.emplace_back([&]() {
			for (idx_t i = 0; i < ITERATIONS; i++) {
				duckdb_prepared_statement stmt = nullptr;
				if (duckdb_prepare(conn, "SELECT 1", &stmt) != DuckDBSuccess) {
					++failures;
					continue;
				}

				duckdb_result result;
				if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
					++failures;
					duckdb_destroy_prepare(&stmt);
					continue;
				}

				duckdb_destroy_result(&result);
				duckdb_destroy_prepare(&stmt);
				++completed;
			}
		});
	}

	for (auto &t : threads) {
		t.join();
	}

	// All executions should succeed
	REQUIRE(failures == 0);
	REQUIRE(completed == NUM_THREADS * ITERATIONS);

	duckdb_disconnect(&conn);
	duckdb_close(&db);
}

static constexpr const char *SALES_PIVOT = "PIVOT sales ON year USING sum(amount) ORDER BY city";

static void CreateSales(CAPITester &tester) {
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE sales(city VARCHAR, year INTEGER, amount INTEGER)"));
	REQUIRE_NO_FAIL(tester.Query(
	    "INSERT INTO sales VALUES ('ams', 2023, 10), ('ams', 2024, 20), ('rtm', 2023, 30), ('rtm', 2024, 40)"));
}

static void RequirePivotRows(CAPIResult &result) {
	REQUIRE(!result.HasError());
	REQUIRE(!result.IsStreaming());
	REQUIRE(duckdb_result_statement_type(result.InternalResult()) == DUCKDB_STATEMENT_TYPE_MULTI);
	REQUIRE(result.ColumnCount() == 3);
	REQUIRE(result.ColumnName(1) == "2023");
	REQUIRE(result.row_count() == 2);
	REQUIRE(result.Fetch<string>(0, 0) == "ams");
	REQUIRE(result.Fetch<int64_t>(1, 0) == 10);
	REQUIRE(result.Fetch<int64_t>(2, 1) == 40);
}

static void RequirePendingPivotRows(CAPIPending &pending) {
	Deadline deadline;
	duckdb_pending_state state;
	while (!duckdb_pending_execution_is_finished(state = pending.ExecuteTask())) {
		REQUIRE(!deadline.Passed());
	}
	REQUIRE(state == DUCKDB_PENDING_RESULT_READY);
	RequirePivotRows(*pending.Execute());
}

TEST_CASE("Preparing a dynamic PIVOT in the C API", "[capi][expanding_statements]") {
	CAPITester tester;
	CAPIPrepared prepared;
	REQUIRE(tester.OpenDatabase(nullptr));
	CreateSales(tester);

	REQUIRE(prepared.Prepare(tester, SALES_PIVOT));
	REQUIRE(duckdb_prepared_statement_type(prepared.prepared) == DUCKDB_STATEMENT_TYPE_MULTI);
	REQUIRE(duckdb_prepared_statement_column_count(prepared.prepared) == 1);
	REQUIRE(duckdb_prepared_statement_column_type(prepared.prepared, 0) == DUCKDB_TYPE_INVALID);
	REQUIRE(duckdb_nparams(prepared.prepared) == 0);

	SECTION("executed") {
		RequirePivotRows(*tester.QueryPrepared(prepared.prepared));
	}
	SECTION("executed for streaming") {
		duckdb_result result;
		auto success = duckdb_execute_prepared_streaming(prepared.prepared, &result) == DuckDBSuccess;
		CAPIResult streamed(result, success);
		RequirePivotRows(streamed);
	}
	SECTION("pending") {
		CAPIPending pending;
		REQUIRE(pending.Pending(prepared));
		RequirePendingPivotRows(pending);
	}
	SECTION("pending for streaming") {
		CAPIPending pending;
		REQUIRE(pending.PendingStreaming(prepared));
		RequirePendingPivotRows(pending);
	}
	SECTION("given a value") {
		REQUIRE(duckdb_bind_int32(prepared.prepared, 1, 42) == DuckDBError);
	}
}

TEST_CASE("Preparing an ALTER that expands in the C API", "[capi][expanding_statements]") {
	CAPITester tester;
	CAPIPrepared prepared;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t AS SELECT range AS i FROM range(10)"));

	REQUIRE(prepared.Prepare(tester, "ALTER TABLE t ADD COLUMN c DOUBLE DEFAULT random()"));
	REQUIRE(duckdb_prepared_statement_type(prepared.prepared) == DUCKDB_STATEMENT_TYPE_MULTI);
	REQUIRE(duckdb_prepared_statement_column_type(prepared.prepared, 0) == DUCKDB_TYPE_INVALID);
	// Preparing ran nothing
	REQUIRE(tester.Query("SELECT c FROM t")->HasError());

	auto result = tester.QueryPrepared(prepared.prepared);
	REQUIRE(!result->HasError());
	REQUIRE(duckdb_result_statement_type(result->InternalResult()) == DUCKDB_STATEMENT_TYPE_MULTI);
	REQUIRE(duckdb_result_return_type(result->InternalResult()) == DUCKDB_RESULT_TYPE_NOTHING);
	auto filled = tester.Query("SELECT count(*) FROM t WHERE c IS NOT NULL");
	REQUIRE(filled->Fetch<int64_t>(0, 0) == 10);
}

TEST_CASE("Preparing a statement that expands with a parameter fails in the C API", "[capi][expanding_statements]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	CreateSales(tester);

	duckdb_prepared_statement prepared = nullptr;
	REQUIRE(duckdb_prepare(tester.connection, "PIVOT sales ON year USING sum(amount + $1)", &prepared) == DuckDBError);
	REQUIRE(string(duckdb_prepare_error(prepared)).find("parameters are not supported") != string::npos);
	duckdb_destroy_prepare(&prepared);
	REQUIRE(tester.Query("SELECT count(*) FROM duckdb_types() WHERE type_name LIKE '__pivot_enum%'")
	            ->Fetch<int64_t>(0, 0) == 0);
}

TEST_CASE("Preparing an extracted dynamic PIVOT in the C API", "[capi][expanding_statements]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	CreateSales(tester);

	duckdb_extracted_statements extracted = nullptr;
	auto count = duckdb_extract_statements(tester.connection, SALES_PIVOT, &extracted);
	duckdb_prepared_statement prepared = nullptr;
	auto state =
	    count == 1 ? duckdb_prepare_extracted_statement(tester.connection, extracted, 0, &prepared) : DuckDBError;
	auto statement_type = duckdb_prepared_statement_type(prepared);
	duckdb::unique_ptr<CAPIResult> result;
	if (state == DuckDBSuccess) {
		result = tester.QueryPrepared(prepared);
	}
	duckdb_destroy_prepare(&prepared);
	duckdb_destroy_extracted(&extracted);

	REQUIRE(count == 1);
	REQUIRE(state == DuckDBSuccess);
	REQUIRE(statement_type == DUCKDB_STATEMENT_TYPE_MULTI);
	RequirePivotRows(*result);
}
