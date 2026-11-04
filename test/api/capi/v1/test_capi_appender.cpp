#include "capi_tester.hpp"
#include "duckdb.h"
#include "duckdb/function/table/system_functions.hpp"

#include <thread>
#include <random>

using namespace duckdb;

namespace {

struct CAPIAppender {
public:
	CAPIAppender(CAPITester &tester, const char *schema, const char *table) {
		auto status = duckdb_appender_create(tester.connection, schema, table, &appender);
		REQUIRE(status == DuckDBSuccess);
	}
	~CAPIAppender() {
		auto status = duckdb_appender_close(appender);
		REQUIRE(status == DuckDBSuccess);
		duckdb_appender_destroy(&appender);
	}
	operator duckdb_appender() const {
		return appender;
	}
	operator duckdb_appender *() {
		return &appender;
	}

public:
	duckdb_appender appender = nullptr;
};

} // namespace

void TestAppenderError(duckdb_appender &appender, const duckdb_error_type type, const string &expected) {
	auto error_data = duckdb_appender_error_data(appender);
	REQUIRE(error_data != nullptr);
	REQUIRE(duckdb_error_data_has_error(error_data));

	auto error_type = duckdb_error_data_error_type(error_data);
	REQUIRE(error_type == type);
	string error_msg = duckdb_error_data_message(error_data);
	REQUIRE(duckdb::StringUtil::Contains(error_msg, expected));

	duckdb_destroy_error_data(&error_data);
}

void AssertDecimalValueMatches(duckdb::unique_ptr<CAPIResult> &result, duckdb_decimal expected) {
	duckdb_decimal actual;

	actual = result->Fetch<duckdb_decimal>(0, 0);
	REQUIRE(actual.scale == expected.scale);
	REQUIRE(actual.width == expected.width);
	REQUIRE(actual.value.lower == expected.value.lower);
	REQUIRE(actual.value.upper == expected.value.upper);
}

template <class TYPE, duckdb_state APPEND_FUNC(duckdb_appender, TYPE)>
void TestAppendingSingleDecimalValue(TYPE value, duckdb_decimal expected, uint8_t width, uint8_t scale) {
	// Set the width and scale of the expected value
	expected.width = width;
	expected.scale = scale;

	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_state status;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query(StringUtil::Format("CREATE TABLE decimals(i DECIMAL(%d,%d))", width, scale));

	duckdb_appender appender;
	status = duckdb_appender_create(tester.connection, nullptr, "decimals", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_begin_row(appender);
	REQUIRE(status == DuckDBSuccess);

	status = APPEND_FUNC(appender, value);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_end_row(appender);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_appender_flush(appender);
	REQUIRE(status == DuckDBSuccess);

	result = tester.Query("SELECT * FROM decimals");
	AssertDecimalValueMatches(result, expected);

	duckdb_appender_close(appender);
	duckdb_appender_destroy(&appender);
}

TEST_CASE("Test appending into DECIMAL in C API", "[capi]") {
	duckdb_decimal expected;

	expected.value.lower = 1000;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<int32_t, &duckdb_append_int32>(1, expected, 4, 3);
	expected.value.lower = (uint64_t)18446744073709541617UL;
	expected.value.upper = -1;
	TestAppendingSingleDecimalValue<int16_t, &duckdb_append_int16>(-9999, expected, 4, 0);
	expected.value.lower = 9999;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<int16_t, &duckdb_append_int16>(9999, expected, 4, 0);
	expected.value.lower = 99999999;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<int32_t, &duckdb_append_int32>(99999999, expected, 8, 0);
	expected.value.lower = 1234;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<const char *, &duckdb_append_varchar>("1.234", expected, 4, 3);
	TestAppendingSingleDecimalValue<const char *, &duckdb_append_varchar>("123.4", expected, 4, 1);

	expected.value.lower = 3245234123123;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<const char *, &duckdb_append_varchar>("3245234.123123", expected, 19, 6);
	TestAppendingSingleDecimalValue<const char *, &duckdb_append_varchar>("3245234.123123", expected, 13, 6);
	// Precision loss
	expected.value.lower = 123124323;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<float, &duckdb_append_float>(12.3124324f, expected, 9, 7);

	// Precision loss
	expected.value.lower = 123452342343;
	expected.value.upper = 0;
	TestAppendingSingleDecimalValue<double, &duckdb_append_double>(12345234234.31243244234324, expected, 26, 1);
}

TEST_CASE("Test NULL struct Appender", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_state status;

	REQUIRE(tester.OpenDatabase(nullptr));
	tester.Query("CREATE TABLE test (simple_struct STRUCT(a INTEGER, b VARCHAR));");
	duckdb_appender appender;

	status = duckdb_appender_create(tester.connection, nullptr, "test", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	// create the column types of the data chunk
	duckdb::vector<duckdb_logical_type> child_types = {duckdb_create_logical_type(DUCKDB_TYPE_INTEGER),
	                                                   duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR)};
	duckdb::vector<const char *> names = {"a", "b"};
	auto struct_type = duckdb_create_struct_type(child_types.data(), names.data(), child_types.size());

	// create the data chunk
	duckdb_logical_type types[1] = {struct_type};
	auto data_chunk = duckdb_create_data_chunk(types, 1);
	REQUIRE(data_chunk);
	REQUIRE(duckdb_data_chunk_get_column_count(data_chunk) == 1);

	auto struct_vector = duckdb_data_chunk_get_vector(data_chunk, 0);
	auto first_child_vector = duckdb_struct_vector_get_child(struct_vector, 0);
	auto second_child_vector = duckdb_struct_vector_get_child(struct_vector, 1);

	// set two values
	auto first_child_ptr = static_cast<int32_t *>(duckdb_vector_get_data(first_child_vector));
	*first_child_ptr = 42;
	duckdb_vector_assign_string_element(second_child_vector, 0, "hello");

	// set the parent/STRUCT vector to NULL
	duckdb_vector_ensure_validity_writable(struct_vector);
	auto validity = duckdb_vector_get_validity(struct_vector);
	duckdb_validity_set_row_validity(validity, 1, false);

	// set the first child vector to NULL
	duckdb_vector_ensure_validity_writable(first_child_vector);
	auto first_validity = duckdb_vector_get_validity(first_child_vector);
	duckdb_validity_set_row_validity(first_validity, 1, false);

	// set the second child vector to NULL
	duckdb_vector_ensure_validity_writable(second_child_vector);
	auto second_validity = duckdb_vector_get_validity(second_child_vector);
	duckdb_validity_set_row_validity(second_validity, 1, false);

	duckdb_data_chunk_set_size(data_chunk, 2);
	REQUIRE(duckdb_append_data_chunk(appender, data_chunk) == DuckDBSuccess);

	// close flushes all remaining data
	status = duckdb_appender_close(appender);
	REQUIRE(status == DuckDBSuccess);

	// verify the result
	result = tester.Query("SELECT simple_struct::VARCHAR FROM test;");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "{'a': 42, 'b': hello}");
	REQUIRE(result->Fetch<string>(0, 1) == "");

	// destroy the data chunk and the appender
	duckdb_destroy_data_chunk(&data_chunk);
	status = duckdb_appender_destroy(&appender);
	REQUIRE(status == DuckDBSuccess);

	// destroy the logical types
	for (auto child_type : child_types) {
		duckdb_destroy_logical_type(&child_type);
	}
	duckdb_destroy_logical_type(&struct_type);
}

TEST_CASE("Test appender statements in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_state status;

	REQUIRE(tester.OpenDatabase(nullptr));
	tester.Query("CREATE TABLE test (i INTEGER, d double, s string)");
	duckdb_appender appender;

	// Creating the table with an unknown table fails, but creates an appender object.
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "unknown_table", &appender) == DuckDBError);
	REQUIRE(appender != nullptr);
	TestAppenderError(appender, DUCKDB_ERROR_CATALOG, "could not be found");

	// Flushing, closing, or destroying the appender also fails due to its invalid table.
	REQUIRE(duckdb_appender_close(appender) == DuckDBError);
	TestAppenderError(appender, DUCKDB_ERROR_INVALID, "not a valid appender");

	// Any data is still destroyed, so there are no leaks, even if duckdb_appender_destroy returns DuckDBError.
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBError);
	REQUIRE(duckdb_appender_destroy(nullptr) == DuckDBError);

	// Appender creation also fails if not providing an appender object.
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "test", nullptr) == DuckDBError);

	// Now, create a valid appender.
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "test", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	// Start appending rows.
	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_int32(appender, 42) == DuckDBSuccess);
	REQUIRE(duckdb_append_double(appender, 4.2) == DuckDBSuccess);
	REQUIRE(duckdb_append_varchar(appender, "Hello, World") == DuckDBSuccess);

	// Exceed the column count.
	REQUIRE(duckdb_append_int32(appender, 42) == DuckDBError);
	TestAppenderError(appender, DUCKDB_ERROR_INVALID_INPUT, "Too many appends for chunk");

	// Finish and flush the row.
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);

	// Next row.
	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_int32(appender, 42) == DuckDBSuccess);
	REQUIRE(duckdb_append_double(appender, 4.2) == DuckDBSuccess);

	// Missing column.
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBError);
	REQUIRE(duckdb_appender_error(appender) != nullptr);
	TestAppenderError(appender, DUCKDB_ERROR_INVALID_INPUT, "Call to EndRow before all columns have been appended to");

	// Append the missing column.
	REQUIRE(duckdb_append_varchar(appender, "Hello, World") == DuckDBSuccess);

	// out of cols here
	status = duckdb_append_int32(appender, 42);
	REQUIRE(status == DuckDBError);

	REQUIRE(duckdb_appender_error(appender) != nullptr);

	status = duckdb_appender_end_row(appender);
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
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	status = duckdb_appender_flush(appender);
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
	                             "us USMALLINT, ui UINTEGER, ub UBIGINT, uf REAL, ud DOUBLE, txt VARCHAR, blb BLOB, dt "
	                             "DATE, tm TIME, ts TIMESTAMP, ival INTERVAL, h HUGEINT, uh UHUGEINT)"));
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

	status = duckdb_append_double(tappender, 0.5);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_varchar_length(tappender, "hello world", 5);
	REQUIRE(status == DuckDBSuccess);

	duckdb_date_struct date_struct;
	date_struct.year = 1992;
	date_struct.month = 9;
	date_struct.day = 3;

	char blob_data[] = "hello world this\0is my long string";
	idx_t blob_len = 34;
	status = duckdb_append_blob(tappender, blob_data, blob_len);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_date(tappender, duckdb_to_date(date_struct));
	REQUIRE(status == DuckDBSuccess);

	duckdb_time_struct time_struct;
	time_struct.hour = 12;
	time_struct.min = 22;
	time_struct.sec = 33;
	time_struct.micros = 1234;

	status = duckdb_append_time(tappender, duckdb_to_time(time_struct));
	REQUIRE(status == DuckDBSuccess);

	duckdb_timestamp_struct ts;
	ts.date = date_struct;
	ts.time = time_struct;

	status = duckdb_append_timestamp(tappender, duckdb_to_timestamp(ts));
	REQUIRE(status == DuckDBSuccess);

	duckdb_interval interval;
	interval.months = 3;
	interval.days = 0;
	interval.micros = 0;

	status = duckdb_append_interval(tappender, interval);
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_hugeint(tappender, duckdb_double_to_hugeint(27));
	REQUIRE(status == DuckDBSuccess);

	status = duckdb_append_uhugeint(tappender, duckdb_double_to_uhugeint(27));
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
	REQUIRE(result->Fetch<double>(9, 0) == 0.5);
	REQUIRE(result->Fetch<string>(10, 0) == "hello");

	auto blob = duckdb_value_blob(&result->InternalResult(), 11, 0);
	REQUIRE(blob.size == blob_len);
	REQUIRE(memcmp(blob.data, blob_data, blob_len) == 0);
	duckdb_free(blob.data);
	REQUIRE(duckdb_value_int32(&result->InternalResult(), 11, 0) == 0);

	auto date = result->Fetch<duckdb_date_struct>(12, 0);
	REQUIRE(date.year == 1992);
	REQUIRE(date.month == 9);
	REQUIRE(date.day == 3);

	auto time = result->Fetch<duckdb_time_struct>(13, 0);
	REQUIRE(time.hour == 12);
	REQUIRE(time.min == 22);
	REQUIRE(time.sec == 33);
	REQUIRE(time.micros == 1234);

	auto timestamp = result->Fetch<duckdb_timestamp_struct>(14, 0);
	REQUIRE(timestamp.date.year == 1992);
	REQUIRE(timestamp.date.month == 9);
	REQUIRE(timestamp.date.day == 3);
	REQUIRE(timestamp.time.hour == 12);
	REQUIRE(timestamp.time.min == 22);
	REQUIRE(timestamp.time.sec == 33);
	REQUIRE(timestamp.time.micros == 1234);

	interval = result->Fetch<duckdb_interval>(15, 0);
	REQUIRE(interval.months == 3);
	REQUIRE(interval.days == 0);
	REQUIRE(interval.micros == 0);

	auto hugeint = result->Fetch<duckdb_hugeint>(16, 0);
	REQUIRE(duckdb_hugeint_to_double(hugeint) == 27);

	auto uhugeint = result->Fetch<duckdb_uhugeint>(17, 0);
	REQUIRE(duckdb_uhugeint_to_double(uhugeint) == 27);

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
	REQUIRE(result->IsNull(12, 1));
	REQUIRE(result->IsNull(13, 1));
	REQUIRE(result->IsNull(14, 1));
	REQUIRE(result->IsNull(15, 1));
	REQUIRE(result->IsNull(16, 1));

	REQUIRE(result->Fetch<bool>(0, 1) == false);
	REQUIRE(result->Fetch<int8_t>(1, 1) == 0);
	REQUIRE(result->Fetch<int16_t>(2, 1) == 0);
	REQUIRE(result->Fetch<int64_t>(3, 1) == 0);
	REQUIRE(result->Fetch<uint8_t>(4, 1) == 0);
	REQUIRE(result->Fetch<uint16_t>(5, 1) == 0);
	REQUIRE(result->Fetch<uint32_t>(6, 1) == 0);
	REQUIRE(result->Fetch<uint64_t>(7, 1) == 0);
	REQUIRE(result->Fetch<float>(8, 1) == 0);
	REQUIRE(result->Fetch<double>(9, 1) == 0);
	REQUIRE(result->Fetch<string>(10, 1).empty());

	blob = duckdb_value_blob(&result->InternalResult(), 11, 1);
	REQUIRE(blob.size == 0);

	date = result->Fetch<duckdb_date_struct>(12, 1);
	REQUIRE(date.year == 1970);

	time = result->Fetch<duckdb_time_struct>(13, 1);
	REQUIRE(time.hour == 0);

	timestamp = result->Fetch<duckdb_timestamp_struct>(14, 1);
	REQUIRE(timestamp.date.year == 1970);
	REQUIRE(timestamp.time.hour == 0);

	interval = result->Fetch<duckdb_interval>(15, 1);
	REQUIRE(interval.months == 0);

	hugeint = result->Fetch<duckdb_hugeint>(16, 1);
	REQUIRE(duckdb_hugeint_to_double(hugeint) == 0);

	uhugeint = result->Fetch<duckdb_uhugeint>(17, 1);
	REQUIRE(duckdb_uhugeint_to_double(uhugeint) == 0);

	// double out of range for hugeint
	hugeint = duckdb_double_to_hugeint(1e300);
	REQUIRE(hugeint.lower == 0);
	REQUIRE(hugeint.upper == 0);

	hugeint = duckdb_double_to_hugeint(NAN);
	REQUIRE(hugeint.lower == 0);
	REQUIRE(hugeint.upper == 0);

	// double out of range for uhugeint
	uhugeint = duckdb_double_to_uhugeint(1e300);
	REQUIRE(uhugeint.lower == 0);
	REQUIRE(uhugeint.upper == 0);

	uhugeint = duckdb_double_to_uhugeint(NAN);
	REQUIRE(uhugeint.lower == 0);
	REQUIRE(uhugeint.upper == 0);
}

TEST_CASE("Test append DEFAULT in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	SECTION("BASIC DEFAULT VALUE") {
		tester.Query("CREATE OR REPLACE TABLE test (a INTEGER, b INTEGER DEFAULT 5)");
		{
			CAPIAppender appender(tester, nullptr, "test");
			auto status = duckdb_appender_begin_row(appender);
			REQUIRE(status == DuckDBSuccess);
			duckdb_append_int32(appender, 42);
			// Even though the column has a DEFAULT, we still require explicitly appending a value/default
			status = duckdb_appender_end_row(appender);
			REQUIRE(status == DuckDBError);
			status = duckdb_appender_flush(appender);
			REQUIRE(status == DuckDBError);

			status = duckdb_append_default(appender);
			REQUIRE(status == DuckDBSuccess);
			status = duckdb_appender_end_row(appender);
			REQUIRE(status == DuckDBSuccess);
		}
		result = tester.Query("SELECT * FROM test");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<int32_t>(0, 0) == 42);
		REQUIRE(result->Fetch<int32_t>(1, 0) == 5);
	}

	SECTION("NON DEFAULT VALUE") {
		tester.Query("CREATE OR REPLACE TABLE test (a INTEGER, b INTEGER DEFAULT 5)");
		{
			CAPIAppender appender(tester, nullptr, "test");
			auto status = duckdb_appender_begin_row(appender);
			REQUIRE(status == DuckDBSuccess);

			// Append default to column without a default
			status = duckdb_append_default(appender);
			REQUIRE(status == DuckDBSuccess);

			status = duckdb_append_int32(appender, 42);
			REQUIRE(status == DuckDBSuccess);
			status = duckdb_appender_end_row(appender);
			REQUIRE(status == DuckDBSuccess);
		}
		result = tester.Query("SELECT * FROM test");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->IsNull(0, 0));
		REQUIRE(result->Fetch<int32_t>(1, 0) == 42);
	}
}

TEST_CASE("Test append timestamp in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE TABLE test (t timestamp)");
	duckdb_appender appender;

	auto status = duckdb_appender_create_ext(tester.connection, nullptr, nullptr, "test", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_varchar(appender, "2022-04-09 15:56:37.544") == DuckDBSuccess);
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);

	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_varchar(appender, "XXXXX") == DuckDBError);
	REQUIRE(duckdb_appender_error(appender) != nullptr);
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBError);

	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);

	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	result = tester.Query("SELECT * FROM test");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "2022-04-09 15:56:37.544");
	tester.Cleanup();
}

TEST_CASE("Test append duckdb_value values in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE TABLE test ("
	             "lt1 boolean,"
	             "lt2 tinyint,"
	             "lt3 smallint,"
	             "lt4 integer,"
	             "lt5 bigint,"
	             "lt6 utinyint,"
	             "lt7 usmallint,"
	             "lt8 uinteger,"
	             "lt9 ubigint,"
	             "lt10 float,"
	             "lt11 double,"
	             "lt12 timestamp,"
	             "lt13 date,"
	             "lt14 time,"
	             "lt15 interval,"
	             "lt16 hugeint,"
	             "lt32 uhugeint,"
	             "lt17 varchar,"
	             "lt18 blob,"
	             "lt19 decimal," // no duckdb_create_decimal (yet)
	             "lt20 timestamp_s,"
	             "lt21 timestamp_ms,"
	             "lt22 timestamp_ns,"
	             "lt23 enum('fly', 'swim', 'walk'),"
	             "lt24 integer[],"
	             "lt25 struct(a integer, b float),"
	             "lt26 map(integer, float)," // no duckdb_create_map_value (yet)
	             "lt33 integer[3],"
	             "lt27 uuid,"                      // no duckdb_create_uuid (yet)
	             "lt28 union(a integer, b float)," // no duckdb_create_union_value (yet)
	             "lt29 bit,"                       // no duckdb_create_bit (yet)
	             "lt30 timetz,"
	             "lt31 timestamptz,"
	             // lt34 any - not a valid type in SQL
	             "lt35 bignum,"  // no duckdb_create_bignum (yet)
	             "lt36 integer," // for sqlnull
	             "lt37 time_ns,"
	             ")");
	duckdb_appender appender;

	auto status = duckdb_appender_create_ext(tester.connection, nullptr, nullptr, "test", &appender);
	REQUIRE(duckdb_appender_error(appender) == nullptr);
	REQUIRE(status == DuckDBSuccess);

	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);

	bool boolean = true;
	auto bool_value = duckdb_create_bool(boolean);
	REQUIRE(duckdb_append_value(appender, bool_value) == DuckDBSuccess);
	duckdb_destroy_value(&bool_value);

	int8_t tinyint = 127;
	auto tinyint_value = duckdb_create_int8(tinyint);
	REQUIRE(duckdb_append_value(appender, tinyint_value) == DuckDBSuccess);
	duckdb_destroy_value(&tinyint_value);

	int16_t smallint = 32767;
	auto smallint_value = duckdb_create_int16(smallint);
	REQUIRE(duckdb_append_value(appender, smallint_value) == DuckDBSuccess);
	duckdb_destroy_value(&smallint_value);

	int32_t integer = 2147483647;
	auto integer_value = duckdb_create_int32(integer);
	REQUIRE(duckdb_append_value(appender, integer_value) == DuckDBSuccess);
	duckdb_destroy_value(&integer_value);

	int64_t bigint = 9223372036854775807;
	auto bigint_value = duckdb_create_int64(bigint);
	REQUIRE(duckdb_append_value(appender, bigint_value) == DuckDBSuccess);
	duckdb_destroy_value(&bigint_value);

	uint8_t utinyint = 255;
	auto utinyint_value = duckdb_create_uint8(utinyint);
	REQUIRE(duckdb_append_value(appender, utinyint_value) == DuckDBSuccess);
	duckdb_destroy_value(&utinyint_value);

	uint16_t usmallint = 65535;
	auto usmallint_value = duckdb_create_uint16(usmallint);
	REQUIRE(duckdb_append_value(appender, usmallint_value) == DuckDBSuccess);
	duckdb_destroy_value(&usmallint_value);

	uint32_t uinteger = 4294967295;
	auto uinteger_value = duckdb_create_uint32(uinteger);
	REQUIRE(duckdb_append_value(appender, uinteger_value) == DuckDBSuccess);
	duckdb_destroy_value(&uinteger_value);

	uint64_t ubigint = 18446744073709551615ull; // unsigned long long literal
	auto ubigint_value = duckdb_create_uint64(ubigint);
	REQUIRE(duckdb_append_value(appender, ubigint_value) == DuckDBSuccess);
	duckdb_destroy_value(&ubigint_value);

	float float_ = 3.4028235e+38;
	auto float_value = duckdb_create_float(float_);
	REQUIRE(duckdb_append_value(appender, float_value) == DuckDBSuccess);
	duckdb_destroy_value(&float_value);

	double double_ = 1.7976931348623157e+308;
	auto double_value = duckdb_create_double(double_);
	REQUIRE(duckdb_append_value(appender, double_value) == DuckDBSuccess);
	duckdb_destroy_value(&double_value);

	duckdb_timestamp timestamp {9223372036854775806};
	auto timestamp_value = duckdb_create_timestamp(timestamp);
	REQUIRE(duckdb_append_value(appender, timestamp_value) == DuckDBSuccess);
	duckdb_destroy_value(&timestamp_value);

	duckdb_date date {2147483646};
	auto date_value = duckdb_create_date(date);
	REQUIRE(duckdb_append_value(appender, date_value) == DuckDBSuccess);
	duckdb_destroy_value(&date_value);

	duckdb_time time {86400000000};
	auto time_value = duckdb_create_time(time);
	REQUIRE(duckdb_append_value(appender, time_value) == DuckDBSuccess);
	duckdb_destroy_value(&time_value);

	duckdb_interval interval {999, 999, 999999999};
	auto interval_value = duckdb_create_interval(interval);
	REQUIRE(duckdb_append_value(appender, interval_value) == DuckDBSuccess);
	duckdb_destroy_value(&interval_value);

	duckdb_hugeint hugeint {18446744073709551615ull, 9223372036854775807};
	auto hugeint_value = duckdb_create_hugeint(hugeint);
	REQUIRE(duckdb_append_value(appender, hugeint_value) == DuckDBSuccess);
	duckdb_destroy_value(&hugeint_value);

	duckdb_uhugeint uhugeint {18446744073709551615ull, 18446744073709551615ull};
	auto uhugeint_value = duckdb_create_uhugeint(uhugeint);
	REQUIRE(duckdb_append_value(appender, uhugeint_value) == DuckDBSuccess);
	duckdb_destroy_value(&uhugeint_value);

	const char *varchar = "🦆🦆🦆🦆🦆🦆";
	auto varchar_value = duckdb_create_varchar(varchar);
	REQUIRE(duckdb_append_value(appender, varchar_value) == DuckDBSuccess);
	duckdb_destroy_value(&varchar_value);

	const uint8_t blob_data[] {0, 1, 2};
	idx_t blob_size = 3;
	auto blob_value = duckdb_create_blob(blob_data, blob_size);
	REQUIRE(duckdb_append_value(appender, blob_value) == DuckDBSuccess);
	duckdb_destroy_value(&blob_value);

	// no duckdb_create_decimal (yet)
	auto null_decimal_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_decimal_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_decimal_value);

	duckdb_timestamp_s timestamp_s {9223372036854};
	auto timestamp_s_value = duckdb_create_timestamp_s(timestamp_s);
	REQUIRE(duckdb_append_value(appender, timestamp_s_value) == DuckDBSuccess);
	duckdb_destroy_value(&timestamp_s_value);

	duckdb_timestamp_ms timestamp_ms {9223372036854775};
	auto timestamp_ms_value = duckdb_create_timestamp_ms(timestamp_ms);
	REQUIRE(duckdb_append_value(appender, timestamp_ms_value) == DuckDBSuccess);
	duckdb_destroy_value(&timestamp_ms_value);

	duckdb_timestamp_ns timestamp_ns {9223372036854775806};
	auto timestamp_ns_value = duckdb_create_timestamp_ns(timestamp_ns);
	REQUIRE(duckdb_append_value(appender, timestamp_ns_value) == DuckDBSuccess);
	duckdb_destroy_value(&timestamp_ns_value);

	const char *enum_members[] {"fly", "swim", "walk"};
	auto enum_member_count = 3;
	auto enum_logical_type = duckdb_create_enum_type(enum_members, enum_member_count);
	uint64_t enum_numerical_value = 2;
	auto enum_value = duckdb_create_enum_value(enum_logical_type, enum_numerical_value);
	REQUIRE(duckdb_append_value(appender, enum_value) == DuckDBSuccess);
	duckdb_destroy_value(&enum_value);
	duckdb_destroy_logical_type(&enum_logical_type);

	auto list_item_logical_type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	idx_t list_item_count = 3;
	int32_t list_integers[3] {101, 102, 103};
	duckdb_value list_values[3];
	for (idx_t i = 0; i < list_item_count; i++) {
		list_values[i] = duckdb_create_int32(list_integers[i]);
	}
	auto list_value = duckdb_create_list_value(list_item_logical_type, list_values, list_item_count);
	REQUIRE(duckdb_append_value(appender, list_value) == DuckDBSuccess);
	duckdb_destroy_value(&list_value);
	for (idx_t i = 0; i < list_item_count; i++) {
		duckdb_destroy_value(&list_values[i]);
	}
	duckdb_destroy_logical_type(&list_item_logical_type);

	auto struct_member_count = 2;
	duckdb_logical_type struct_member_types[2];
	struct_member_types[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	struct_member_types[1] = duckdb_create_logical_type(DUCKDB_TYPE_FLOAT);
	const char *struct_member_names[] {"a", "b"};
	int32_t struct_integer = 42;
	float struct_float = 3.14159;
	duckdb_value struct_values[2];
	struct_values[0] = duckdb_create_int32(struct_integer);
	struct_values[1] = duckdb_create_float(struct_float);
	auto struct_logical_type = duckdb_create_struct_type(struct_member_types, struct_member_names, struct_member_count);
	auto struct_value = duckdb_create_struct_value(struct_logical_type, struct_values);
	REQUIRE(duckdb_append_value(appender, struct_value) == DuckDBSuccess);
	duckdb_destroy_value(&struct_value);
	duckdb_destroy_value(&struct_values[0]);
	duckdb_destroy_value(&struct_values[1]);
	duckdb_destroy_logical_type(&struct_logical_type);
	duckdb_destroy_logical_type(&struct_member_types[0]);
	duckdb_destroy_logical_type(&struct_member_types[1]);

	// no duckdb_create_map_value (yet)
	auto null_map_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_map_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_map_value);

	auto array_item_logical_type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	idx_t array_item_count = 3;
	int32_t array_integers[3] {201, 202, 203};
	duckdb_value array_values[3];
	for (idx_t i = 0; i < array_item_count; i++) {
		array_values[i] = duckdb_create_int32(array_integers[i]);
	}
	auto array_value = duckdb_create_array_value(array_item_logical_type, array_values, array_item_count);
	REQUIRE(duckdb_append_value(appender, array_value) == DuckDBSuccess);
	duckdb_destroy_value(&array_value);
	for (idx_t i = 0; i < array_item_count; i++) {
		duckdb_destroy_value(&array_values[i]);
	}
	duckdb_destroy_logical_type(&array_item_logical_type);

	// no duckdb_create_uuid (yet)
	auto null_uuid_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_uuid_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_uuid_value);

	// no duckdb_create_union_value (yet)
	auto null_union_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_union_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_union_value);

	// no duckdb_create_bit (yet)
	auto null_bit_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_bit_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_bit_value);

	duckdb_time_tz time_tz = duckdb_create_time_tz(86400000000, -57599);
	auto time_tz_value = duckdb_create_time_tz_value(time_tz);
	REQUIRE(duckdb_append_value(appender, time_tz_value) == DuckDBSuccess);
	duckdb_destroy_value(&time_tz_value);

	duckdb_timestamp timestamp_tz {9223372036854775806};
	auto timestamp_tz_value = duckdb_create_timestamp_tz(timestamp_tz);
	REQUIRE(duckdb_append_value(appender, timestamp_tz_value) == DuckDBSuccess);
	duckdb_destroy_value(&timestamp_tz_value);

	// no duckdb_create_bignum (yet)
	auto null_bignum_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_bignum_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_bignum_value);

	auto null_value = duckdb_create_null_value();
	REQUIRE(duckdb_append_value(appender, null_value) == DuckDBSuccess);
	duckdb_destroy_value(&null_value);

	duckdb_time_ns time_ns {86400123456789};
	auto time_ns_value = duckdb_create_time_ns(time_ns);
	REQUIRE(duckdb_append_value(appender, time_ns_value) == DuckDBSuccess);
	duckdb_destroy_value(&time_ns_value);

	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);

	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	result = tester.Query("SELECT * FROM test");
	REQUIRE_NO_FAIL(*result);

	auto chunk = result->NextChunk();
	REQUIRE(reinterpret_cast<bool *>(chunk->GetData(0))[0] == boolean);
	REQUIRE(reinterpret_cast<int8_t *>(chunk->GetData(1))[0] == tinyint);
	REQUIRE(reinterpret_cast<int16_t *>(chunk->GetData(2))[0] == smallint);
	REQUIRE(reinterpret_cast<int32_t *>(chunk->GetData(3))[0] == integer);
	REQUIRE(reinterpret_cast<int64_t *>(chunk->GetData(4))[0] == bigint);
	REQUIRE(reinterpret_cast<uint8_t *>(chunk->GetData(5))[0] == utinyint);
	REQUIRE(reinterpret_cast<uint16_t *>(chunk->GetData(6))[0] == usmallint);
	REQUIRE(reinterpret_cast<uint32_t *>(chunk->GetData(7))[0] == uinteger);
	REQUIRE(reinterpret_cast<uint64_t *>(chunk->GetData(8))[0] == ubigint);
	REQUIRE(reinterpret_cast<float *>(chunk->GetData(9))[0] == float_);
	REQUIRE(reinterpret_cast<double *>(chunk->GetData(10))[0] == double_);
	REQUIRE(reinterpret_cast<duckdb_timestamp *>(chunk->GetData(11))[0].micros == timestamp.micros);
	REQUIRE(reinterpret_cast<duckdb_date *>(chunk->GetData(12))[0].days == date.days);
	REQUIRE(reinterpret_cast<duckdb_time *>(chunk->GetData(13))[0].micros == time.micros);

	REQUIRE_THAT(reinterpret_cast<duckdb_interval *>(chunk->GetData(14))[0],
	             Catch::Predicate<duckdb_interval>([&](const duckdb_interval &input) {
		             return input.months == interval.months && input.days == interval.days &&
		                    input.micros == interval.micros;
	             }));
	REQUIRE_THAT(reinterpret_cast<duckdb_hugeint *>(chunk->GetData(15))[0],
	             Catch::Predicate<duckdb_hugeint>([&](const duckdb_hugeint &input) {
		             return input.upper == hugeint.upper && input.lower == hugeint.lower;
	             }));
	REQUIRE_THAT(reinterpret_cast<duckdb_uhugeint *>(chunk->GetData(16))[0],
	             Catch::Predicate<duckdb_uhugeint>([&](const duckdb_uhugeint &input) {
		             return input.upper == uhugeint.upper && input.lower == uhugeint.lower;
	             }));
	REQUIRE_THAT(reinterpret_cast<duckdb_string_t *>(chunk->GetData(17))[0],
	             Catch::Predicate<duckdb_string_t>([&](const duckdb_string_t &input) {
		             return !strncmp(duckdb_string_t_data(const_cast<duckdb_string_t *>(&input)), varchar,
		                             strlen(varchar)) &&
		                    duckdb_string_t_length(input) == strlen(varchar);
	             }));
	REQUIRE_THAT(reinterpret_cast<duckdb_string_t *>(chunk->GetData(18))[0],
	             Catch::Predicate<duckdb_string_t>([&](const duckdb_string_t &input) {
		             return !memcmp(duckdb_string_t_data(const_cast<duckdb_string_t *>(&input)), blob_data,
		                            blob_size) &&
		                    duckdb_string_t_length(input) == blob_size;
	             }));

	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(19), 0) == false); // no duckdb_create_decimal (yet)

	REQUIRE(reinterpret_cast<duckdb_timestamp_s *>(chunk->GetData(20))[0].seconds == timestamp_s.seconds);
	REQUIRE(reinterpret_cast<duckdb_timestamp_ms *>(chunk->GetData(21))[0].millis == timestamp_ms.millis);
	REQUIRE(reinterpret_cast<duckdb_timestamp_ns *>(chunk->GetData(22))[0].nanos == timestamp_ns.nanos);
	REQUIRE(reinterpret_cast<uint8_t *>(chunk->GetData(23))[0] == enum_numerical_value);

	REQUIRE(reinterpret_cast<uint64_t *>(chunk->GetData(24))[0] == 0);               // list 0 offset
	REQUIRE(reinterpret_cast<uint64_t *>(chunk->GetData(24))[1] == list_item_count); // list 0 length
	for (idx_t i = 0; i < list_item_count; i++) {
		REQUIRE(reinterpret_cast<int32_t *>(chunk->GetListChildData(24))[i] == list_integers[i]);
	}

	REQUIRE(reinterpret_cast<int32_t *>(chunk->GetStructChildData(25, 0))[0] == struct_integer);
	REQUIRE(reinterpret_cast<float *>(chunk->GetStructChildData(25, 1))[0] == struct_float);

	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(26), 0) == false); // no duckdb_create_map_value (yet)

	for (idx_t i = 0; i < array_item_count; i++) {
		REQUIRE(reinterpret_cast<int32_t *>(chunk->GetArrayChildData(27))[i] == array_integers[i]);
	}

	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(28), 0) == false); // no duckdb_create_union (yet)
	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(29), 0) == false); // no duckdb_create_union_value (yet)
	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(30), 0) == false); // no duckdb_create_bit (yet)

	REQUIRE(reinterpret_cast<duckdb_time_tz *>(chunk->GetData(31))[0].bits == time_tz.bits);
	REQUIRE(reinterpret_cast<duckdb_timestamp *>(chunk->GetData(32))[0].micros == timestamp_tz.micros);

	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(33), 0) == false); // no duckdb_create_bignum (yet)

	REQUIRE(duckdb_validity_row_is_valid(chunk->GetValidity(34), 0) == false); // sqlnull

	REQUIRE(reinterpret_cast<duckdb_time_ns *>(chunk->GetData(35))[0].nanos == time_ns.nanos);

	tester.Cleanup();
}

TEST_CASE("Test append to different catalog in C API") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	auto test_dir = TestDirectoryPath();
	auto attach_query = "ATTACH '" + test_dir + "/append_to_other.db'";
	REQUIRE(tester.Query(attach_query)->success);

	auto result = tester.Query("CREATE OR REPLACE TABLE append_to_other.tbl(i INTEGER)");
	REQUIRE(result->success);

	duckdb_appender appender;
	auto status = duckdb_appender_create_ext(tester.connection, "append_to_other", "main", "tbl", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	for (idx_t i = 0; i < 200; i++) {
		REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
		REQUIRE(duckdb_append_int32(appender, 2) == DuckDBSuccess);
		REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);
	}
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);

	result = tester.Query("SELECT SUM(i)::BIGINT FROM append_to_other.tbl");
	REQUIRE(result->Fetch<int64_t>(0, 0) == 400);

	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);
	tester.Cleanup();
}

TEST_CASE("Test appending with an active column list in the C API") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30, l AS (random()))");
	duckdb_appender appender;

	auto status = duckdb_appender_create_ext(tester.connection, nullptr, nullptr, "tbl", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	REQUIRE(duckdb_appender_add_column(appender, "hello") == DuckDBError);
	TestAppenderError(appender, DUCKDB_ERROR_INVALID_INPUT, "the column must exist in the table");
	REQUIRE(duckdb_appender_add_column(appender, "j") == DuckDBSuccess);

	duckdb_logical_type types[1];
	types[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	auto data_chunk = duckdb_create_data_chunk(types, 1);

	auto col = duckdb_data_chunk_get_vector(data_chunk, 0);
	auto col_data = reinterpret_cast<int32_t *>(duckdb_vector_get_data(col));
	col_data[0] = 15;
	duckdb_data_chunk_set_size(data_chunk, 1);

	REQUIRE(duckdb_append_data_chunk(appender, data_chunk) == DuckDBSuccess);
	duckdb_destroy_data_chunk(&data_chunk);
	duckdb_destroy_logical_type(&types[0]);

	REQUIRE(duckdb_appender_clear_columns(appender) == DuckDBSuccess);

	duckdb_logical_type types_all[3];
	types_all[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	types_all[1] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	types_all[2] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	auto data_chunk_all = duckdb_create_data_chunk(types_all, 3);

	for (idx_t i = 0; i < 3; i++) {
		col = duckdb_data_chunk_get_vector(data_chunk_all, i);
		col_data = reinterpret_cast<int32_t *>(duckdb_vector_get_data(col));
		col_data[0] = 42;
	}
	duckdb_data_chunk_set_size(data_chunk_all, 1);

	REQUIRE(duckdb_append_data_chunk(appender, data_chunk_all) == DuckDBSuccess);
	duckdb_destroy_data_chunk(&data_chunk_all);
	duckdb_destroy_logical_type(&types_all[0]);
	duckdb_destroy_logical_type(&types_all[1]);
	duckdb_destroy_logical_type(&types_all[2]);

	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	result = tester.Query("SELECT i, j, k, l IS NOT NULL FROM tbl");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int32_t>(0, 0) == 4);
	REQUIRE(result->Fetch<int32_t>(1, 0) == 15);
	REQUIRE(result->Fetch<int32_t>(2, 0) == 30);
	REQUIRE(result->Fetch<bool>(3, 0) == true);
	REQUIRE(result->Fetch<int32_t>(0, 1) == 42);
	REQUIRE(result->Fetch<int32_t>(1, 1) == 42);
	REQUIRE(result->Fetch<int32_t>(2, 1) == 42);
	REQUIRE(result->Fetch<bool>(3, 1) == true);

	tester.Cleanup();
}

TEST_CASE("Test appending default value to data chunk in the C API") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30, l AS (random()))");
	duckdb_appender appender;

	auto status = duckdb_appender_create_ext(tester.connection, nullptr, nullptr, "tbl", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	duckdb_logical_type types[3];
	types[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	types[1] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	types[2] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	auto data_chunk = duckdb_create_data_chunk(types, 3);

	auto col1 = duckdb_data_chunk_get_vector(data_chunk, 0);
	auto col_data1 = reinterpret_cast<int32_t *>(duckdb_vector_get_data(col1));
	col_data1[0] = 15;
	col_data1[1] = 17;

	auto col2 = duckdb_data_chunk_get_vector(data_chunk, 1);
	auto col_data2 = reinterpret_cast<int32_t *>(duckdb_vector_get_data(col2));
	col_data2[0] = 16;
	col_data2[1] = 18;

	REQUIRE(duckdb_append_default_to_chunk(appender, data_chunk, 2, 0) == DuckDBSuccess);
	REQUIRE(duckdb_append_default_to_chunk(appender, data_chunk, 2, 1) == DuckDBSuccess);

	REQUIRE(duckdb_append_default_to_chunk(appender, data_chunk, 3, 1) == DuckDBError);
	REQUIRE(duckdb_append_default_to_chunk(appender, data_chunk, 2, duckdb_vector_size() + 1) == DuckDBError);

	duckdb_data_chunk_set_size(data_chunk, 2);

	REQUIRE(duckdb_append_data_chunk(appender, data_chunk) == DuckDBSuccess);
	duckdb_destroy_data_chunk(&data_chunk);
	duckdb_destroy_logical_type(&types[0]);
	duckdb_destroy_logical_type(&types[1]);
	duckdb_destroy_logical_type(&types[2]);

	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	result = tester.Query("SELECT i, j, k, l IS NOT NULL FROM tbl");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int32_t>(0, 0) == 15);
	REQUIRE(result->Fetch<int32_t>(1, 0) == 16);
	REQUIRE(result->Fetch<int32_t>(2, 0) == 30);
	REQUIRE(result->Fetch<bool>(3, 0) == true);
	REQUIRE(result->Fetch<int32_t>(0, 1) == 17);
	REQUIRE(result->Fetch<int32_t>(1, 1) == 18);
	REQUIRE(result->Fetch<int32_t>(2, 1) == 30);
	REQUIRE(result->Fetch<bool>(3, 1) == true);

	tester.Cleanup();
}

TEST_CASE("Test UPSERT using the C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE TABLE tbl (i INT PRIMARY KEY, value VARCHAR)");
	tester.Query("INSERT INTO tbl VALUES (1, 'hello')");
	duckdb_appender appender;

	string query = "INSERT OR REPLACE INTO tbl SELECT i, val FROM my_appended_data";
	duckdb_logical_type types[2];
	types[0] = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
	types[1] = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

	const char *column_names[2];
	column_names[0] = "i";
	column_names[1] = "val";

	idx_t column_count = 2;

	auto status = duckdb_appender_create_query(tester.connection, query.c_str(), column_count, types,
	                                           "my_appended_data", column_names, &appender);
	duckdb_destroy_logical_type(&types[0]);
	duckdb_destroy_logical_type(&types[1]);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_int32(appender, 1) == DuckDBSuccess);
	REQUIRE(duckdb_append_varchar(appender, "hello world") == DuckDBSuccess);
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);

	REQUIRE(duckdb_appender_begin_row(appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_int32(appender, 2) == DuckDBSuccess);
	REQUIRE(duckdb_append_varchar(appender, "bye bye") == DuckDBSuccess);
	REQUIRE(duckdb_appender_end_row(appender) == DuckDBSuccess);

	REQUIRE(duckdb_appender_flush(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	result = tester.Query("SELECT * FROM tbl ORDER BY i");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int32_t>(0, 0) == 1);
	REQUIRE(result->Fetch<string>(1, 0) == "hello world");
	REQUIRE(result->Fetch<int32_t>(0, 1) == 2);
	REQUIRE(result->Fetch<string>(1, 1) == "bye bye");

	tester.Cleanup();
}

bool HasError(const duckdb_state state, atomic<bool> &success, const string &message) {
	if (state == DuckDBError) {
		success = false;
		Printer::Print(message);
		return true;
	}
	return false;
}

TEST_CASE("Test the appender with parallel appends and multiple data types in the C API", "[capi]") {
	auto test_types = TestAllTypesFun::GetTestTypes(false, false);

	string query = "CREATE TABLE IF NOT EXISTS test (";
	for (auto &type : test_types) {
		if (type.name == "union") {
			type.name = "union_col";
		}
		query += type.name + " " + type.type.ToString() + ", ";
	}
	query += ")";

	char *err_msg;

	// Open DB. The empty struct type requires storage version v2.0.0, and GEOGRAPHY requires v2.1.0.
	auto test_dir = TestDirectoryPath();
	auto path = test_dir + "/test.db";
	duckdb_config config;
	REQUIRE(duckdb_create_config(&config) == DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "storage_compatibility_version", "v2.1.0") == DuckDBSuccess);
	duckdb_database db;
	REQUIRE(duckdb_open_ext(path.c_str(), &db, config, &err_msg) == DuckDBSuccess);
	duckdb_destroy_config(&config);

	// Connect.
	duckdb_connection conn;
	REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);

	// Create the table.
	duckdb_result ret;
	REQUIRE(duckdb_query(conn, query.c_str(), &ret) == DuckDBSuccess);
	duckdb_destroy_result(&ret);

	atomic<bool> success {true};
	duckdb::vector<std::thread> threads;

	idx_t worker_count = 5;
	for (idx_t worker_id = 0; worker_id < worker_count; worker_id++) {
		threads.emplace_back([db, &success, test_types]() {
			// Create thread-local connection.
			duckdb_connection t_conn;
			if (HasError(duckdb_connect(db, &t_conn), success, "failed to create connection")) {
				return;
			}

			// Create appender.
			duckdb_appender t_app;
			if (HasError(duckdb_appender_create(t_conn, "main", "test", &t_app), success,
			             "failed to create appender")) {
				return;
			}

			// Start a transaction.
			duckdb_result t_ret;
			if (HasError(duckdb_query(t_conn, "BEGIN TRANSACTION", &t_ret), success, "failed to begin transaction")) {
				return;
			}
			duckdb_destroy_result(&t_ret);

			for (unsigned j = 0; j < STANDARD_VECTOR_SIZE + 10; j++) {
				if (!success) {
					return;
				}

				// Begin row.
				if (HasError(duckdb_appender_begin_row(t_app), success, "failed to begin append to row")) {
					return;
				}

				// Append all values.
				for (const auto &type : test_types) {
					auto value = type.min_value;
					duckdb_value val_ptr = reinterpret_cast<duckdb_value>(&value);
					if (HasError(duckdb_append_value(t_app, val_ptr), success, "failed to append value to row")) {
						return;
					};
				}

				// End row.
				if (HasError(duckdb_appender_end_row(t_app), success, "failed to append end row")) {
					return;
				}
			}

			// COMMIT and clean up.
			if (HasError(duckdb_query(t_conn, "COMMIT", &t_ret), success, "failed to commit transaction")) {
				return;
			}
			duckdb_destroy_result(&t_ret);
			if (HasError(duckdb_appender_destroy(&t_app), success, "failed to append destroy result")) {
				return;
			}
			duckdb_disconnect(&t_conn);
		});
	}

	for (auto &t : threads) {
		t.join();
	}

	duckdb_disconnect(&conn);
	duckdb_close(&db);
}

TEST_CASE("Test clear appender data in C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;
	duckdb_state status;

	REQUIRE(tester.OpenDatabase(nullptr));

	// create a table and insert initial data
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE integers(i INTEGER)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO integers VALUES (1)"));

	// create appender and append many rows
	duckdb_appender appender;
	status = duckdb_appender_create(tester.connection, nullptr, "integers", &appender);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_appender_error(appender) == nullptr);

	// We will append rows to reach more than the maximum chunk size
	// (DEFAULT_FLUSH_COUNT will always be more than a chunk size),
	// so we will make sure that also the collection is being cleared.
	// We use the DEFAULT_FLUSH_COUNT so we won't flush before calling the `Clear`
	constexpr auto rows_to_append = BaseAppender::DEFAULT_FLUSH_COUNT - 10;

	// append a bunch of values that should be cleared
	for (idx_t i = 0; i < rows_to_append; i++) {
		status = duckdb_appender_begin_row(appender);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_append_int32(appender, 999);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_appender_end_row(appender);
		REQUIRE(status == DuckDBSuccess);
	}

	// clear all buffered data without flushing
	status = duckdb_appender_clear(appender);
	REQUIRE(status == DuckDBSuccess);

	// close the appender (should not write the cleared data)
	status = duckdb_appender_close(appender);
	REQUIRE(status == DuckDBSuccess);

	// verify that only the initial data exists (the 2000 rows were cleared)
	result = tester.Query("SELECT SUM(i)::BIGINT FROM integers");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 1);

	// destroy the appender
	status = duckdb_appender_destroy(&appender);
	REQUIRE(status == DuckDBSuccess);

	// test that appender can be used after clear
	status = duckdb_appender_create(tester.connection, nullptr, "integers", &appender);
	REQUIRE(status == DuckDBSuccess);

	// append a few rows
	for (idx_t i = 0; i < 5; i++) {
		status = duckdb_appender_begin_row(appender);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_append_int32(appender, 42);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_appender_end_row(appender);
		REQUIRE(status == DuckDBSuccess);
	}

	// clear again
	status = duckdb_appender_clear(appender);
	REQUIRE(status == DuckDBSuccess);

	// append new data after clear
	for (idx_t i = 0; i < 3; i++) {
		status = duckdb_appender_begin_row(appender);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_append_int32(appender, 100);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_appender_end_row(appender);
		REQUIRE(status == DuckDBSuccess);
	}

	// flush this time to write the new data
	status = duckdb_appender_flush(appender);
	REQUIRE(status == DuckDBSuccess);

	// close
	status = duckdb_appender_close(appender);
	REQUIRE(status == DuckDBSuccess);

	// verify: initial 1 + new 3 rows = 301 total
	result = tester.Query("SELECT SUM(i)::BIGINT FROM integers");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 301);

	status = duckdb_appender_destroy(&appender);
	REQUIRE(status == DuckDBSuccess);

	tester.Cleanup();
}
