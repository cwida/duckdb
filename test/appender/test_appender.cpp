#include "catch.hpp"
#include "duckdb/main/appender.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <vector>

using namespace duckdb;

TEST_CASE("Basic appender tests", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// create a table to append to
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));

	// append a bunch of values
	{
		Appender appender(con, "integers");
		for (idx_t i = 0; i < 2000; i++) {
			appender.BeginRow();
			appender.Append<int32_t>(1);
			appender.EndRow();
		}
		appender.Close();
	}

	con.Query("BEGIN TRANSACTION");

	// check that the values have been added to the database
	result = con.Query("SELECT SUM(i) FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {2000}));

	// test a rollback of the appender
	{
		Appender appender2(con, "integers");
		// now append a bunch of values
		for (idx_t i = 0; i < 2000; i++) {
			appender2.BeginRow();
			appender2.Append<int32_t>(1);
			appender2.EndRow();
		}
		appender2.Close();
	}
	con.Query("ROLLBACK");

	// the data in the database should not be changed
	result = con.Query("SELECT SUM(i) FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {2000}));

	// test different types
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE vals(i TINYINT, j SMALLINT, k BIGINT, l VARCHAR, m DECIMAL)"));

	// now append a bunch of values
	{
		Appender appender(con, "vals");

		for (idx_t i = 0; i < 2000; i++) {
			appender.BeginRow();
			appender.Append<int8_t>(1);
			appender.Append<int16_t>(1);
			appender.Append<int64_t>(1);
			appender.Append<const char *>("hello");
			appender.Append<double>(3.33);
			appender.EndRow();
		}
	}

	// check that the values have been added to the database
	result = con.Query("SELECT l, SUM(k) FROM vals GROUP BY l");
	REQUIRE(CHECK_COLUMN(result, 0, {"hello"}));
	REQUIRE(CHECK_COLUMN(result, 1, {2000}));

	// now test various error conditions
	// too few values per row
	{
		Appender appender(con, "integers");
		appender.BeginRow();
		REQUIRE_THROWS(appender.EndRow());
	}
	// too many values per row
	{
		Appender appender(con, "integers");
		appender.BeginRow();
		appender.Append<Value>(Value::INTEGER(2000));
		REQUIRE_THROWS(appender.Append<Value>(Value::INTEGER(2000)));
	}
}

TEST_CASE("Test AppendRow", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// create a table to append to
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));

	// append a bunch of values
	{
		Appender appender(con, "integers");
		for (idx_t i = 0; i < 2000; i++) {
			appender.AppendRow(1);
		}
		appender.Close();
	}

	// check that the values have been added to the database
	result = con.Query("SELECT SUM(i) FROM integers");
	REQUIRE(CHECK_COLUMN(result, 0, {2000}));

	{
		Appender appender(con, "integers");
		// test wrong types in append row
		REQUIRE_THROWS(appender.AppendRow("hello"));
	}

	// test different types
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE vals(i TINYINT, j SMALLINT, k BIGINT, l VARCHAR, m DECIMAL)"));
	// now append a bunch of values
	{
		Appender appender(con, "vals");
		for (idx_t i = 0; i < 2000; i++) {
			appender.AppendRow(1, 1, 1, "hello", 3.33);
			// append null values
			appender.AppendRow(nullptr, nullptr, nullptr, nullptr, nullptr);
		}
	}

	result = con.Query("SELECT COUNT(*), COUNT(i), COUNT(j), COUNT(k), COUNT(l), COUNT(m) FROM vals");
	REQUIRE(CHECK_COLUMN(result, 0, {4000}));
	REQUIRE(CHECK_COLUMN(result, 1, {2000}));
	REQUIRE(CHECK_COLUMN(result, 2, {2000}));
	REQUIRE(CHECK_COLUMN(result, 3, {2000}));
	REQUIRE(CHECK_COLUMN(result, 4, {2000}));
	REQUIRE(CHECK_COLUMN(result, 5, {2000}));

	// check that the values have been added to the database
	result = con.Query("SELECT l, SUM(k) FROM vals WHERE i IS NOT NULL GROUP BY l");
	REQUIRE(CHECK_COLUMN(result, 0, {"hello"}));
	REQUIRE(CHECK_COLUMN(result, 1, {2000}));

	// test dates and times
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE dates(d DATE, t TIME, ts TIMESTAMP)"));
	// now append a bunch of values
	{
		Appender appender(con, "dates");
		appender.AppendRow(Value::DATE(1992, 1, 1), Value::TIME(1, 1, 1, 0), Value::TIMESTAMP(1992, 1, 1, 1, 1, 1, 0));
	}
	result = con.Query("SELECT * FROM dates");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::DATE(1992, 1, 1)}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::TIME(1, 1, 1, 0)}));
	REQUIRE(CHECK_COLUMN(result, 2, {Value::TIMESTAMP(1992, 1, 1, 1, 1, 1, 0)}));

	// test dates and times without value append
	REQUIRE_NO_FAIL(con.Query("DELETE FROM dates"));
	// now append a bunch of values
	{
		Appender appender(con, "dates");
		appender.AppendRow(Date::FromDate(1992, 1, 1), Time::FromTime(1, 1, 1, 0),
		                   Timestamp::FromDatetime(Date::FromDate(1992, 1, 1), Time::FromTime(1, 1, 1, 0)));
	}
	result = con.Query("SELECT * FROM dates");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::DATE(1992, 1, 1)}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::TIME(1, 1, 1, 0)}));
	REQUIRE(CHECK_COLUMN(result, 2, {Value::TIMESTAMP(1992, 1, 1, 1, 1, 1, 0)}));
}

TEST_CASE("Test appender with generated column", "[appender]") {
	DuckDB db(nullptr); // Create an in-memory DuckDB database
	Connection con(db); // Create a connection to the database

	SECTION("Insert into table with generated column first") {
		// Try to create a table with a generated column
		REQUIRE_NOTHROW(con.Query(R"(
			CREATE TABLE tbl (
				b VARCHAR GENERATED ALWAYS AS (a),
				a VARCHAR
			)
		)"));

		Appender appender(con, "tbl");
		REQUIRE_NOTHROW(appender.BeginRow());
		REQUIRE_NOTHROW(appender.Append("a"));

		// Column 'b' is generated from 'a', so it does not need to be explicitly appended
		// End the row
		REQUIRE_NOTHROW(appender.EndRow());

		// Close the appender
		REQUIRE_NOTHROW(appender.Close());

		// Query the table to verify that the row was inserted correctly
		auto result = con.Query("SELECT * FROM tbl");
		REQUIRE_NO_FAIL(*result);

		// Check that the column 'a' contains "a" and 'b' contains the generated value "a"
		REQUIRE(CHECK_COLUMN(result, 0, {Value("a")}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value("a")}));
	}

	SECTION("Insert into table with generated column second") {
		// Try to create a table with a generated column
		REQUIRE_NOTHROW(con.Query(R"(
			CREATE TABLE tbl (
				a VARCHAR,
				b VARCHAR GENERATED ALWAYS AS (a)
			)
		)"));

		Appender appender(con, "tbl");
		REQUIRE_NOTHROW(appender.BeginRow());
		REQUIRE_NOTHROW(appender.Append("a"));

		// Column 'b' is generated from 'a', so it does not need to be explicitly appended
		// End the row
		REQUIRE_NOTHROW(appender.EndRow());

		// Close the appender
		REQUIRE_NOTHROW(appender.Close());

		// Query the table to verify that the row was inserted correctly
		auto result = con.Query("SELECT * FROM tbl");
		REQUIRE_NO_FAIL(*result);

		// Check that the column 'a' contains "a" and 'b' contains the generated value "a"
		REQUIRE(CHECK_COLUMN(result, 0, {Value("a")}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value("a")}));
	}
}

TEST_CASE("Test default value appender", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	SECTION("Insert DEFAULT into default column") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i iNTEGER, j INTEGER DEFAULT 5)"));
		{
			Appender appender(con, "integers");
			appender.BeginRow();
			appender.Append<int32_t>(2);
			appender.AppendDefault();
			REQUIRE_NOTHROW(appender.EndRow());
			REQUIRE_NOTHROW(appender.Close());
		}
		result = con.Query("SELECT * FROM integers");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(2)}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::INTEGER(5)}));
	}

	SECTION("Insert DEFAULT into non-default column") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i iNTEGER, j INTEGER DEFAULT 5)"));
		{
			Appender appender(con, "integers");
			appender.BeginRow();
			// 'i' does not have a DEFAULT value, so it gets NULL
			REQUIRE_NOTHROW(appender.AppendDefault());
			REQUIRE_NOTHROW(appender.AppendDefault());
			REQUIRE_NOTHROW(appender.EndRow());
			REQUIRE_NOTHROW(appender.Close());
		}
		result = con.Query("SELECT * FROM integers");
		REQUIRE(CHECK_COLUMN(result, 0, {Value(LogicalTypeId::INTEGER)}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::INTEGER(5)}));
	}

	SECTION("Insert DEFAULT into column that can't be NULL") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i integer NOT NULL)"));
		{
			Appender appender(con, "integers");
			appender.BeginRow();
			REQUIRE_NOTHROW(appender.AppendDefault());
			REQUIRE_NOTHROW(appender.EndRow());
			// NOT NULL constraint failed
			REQUIRE_THROWS(appender.Close());
		}
		result = con.Query("SELECT * FROM integers");
		auto chunk = result->Fetch();
		REQUIRE(chunk == nullptr);
	}

	SECTION("DEFAULT nextval('seq')") {
		REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE seq"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i iNTEGER, j INTEGER DEFAULT nextval('seq'))"));
		{
			Appender appender(con, "integers");
			appender.BeginRow();
			appender.Append<int32_t>(1);

			// NOT_IMPLEMENTED: Non-foldable default values are not supported currently
			REQUIRE_THROWS(appender.AppendDefault());
			REQUIRE_THROWS(appender.EndRow());
			REQUIRE_NOTHROW(appender.Close());
		}
		// result = con.Query("SELECT * FROM integers");
		// REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(1)}));
		// REQUIRE(CHECK_COLUMN(result, 1, {Value::INTEGER(1)}));
	}

	SECTION("DEFAULT random()") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i iNTEGER, j DOUBLE DEFAULT random())"));
		con.Query("select setseed(0.42)");
		{
			Appender appender(con, "integers");
			appender.BeginRow();
			appender.Append<int32_t>(1);
			// NOT_IMPLEMENTED: Non-foldable default values are not supported currently
			REQUIRE_THROWS(appender.AppendDefault());
			REQUIRE_THROWS(appender.EndRow());
			REQUIRE_NOTHROW(appender.Close());
		}
		// result = con.Query("SELECT * FROM integers");
		// REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(1)}));
		// REQUIRE(CHECK_COLUMN(result, 1, {Value::DOUBLE(0.4729174713138491)}));
	}

	SECTION("DEFAULT now()") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i iNTEGER, j TIMESTAMPTZ DEFAULT now())"));
		con.Query("BEGIN TRANSACTION");
		result = con.Query("select now()");
		auto &materialized_result = *result;
		auto current_time = materialized_result.Collection().GetValue(0, 0);
		{
			Appender appender(con, "integers");
			appender.BeginRow();
			appender.Append<int32_t>(1);
			REQUIRE_NOTHROW(appender.AppendDefault());
			REQUIRE_NOTHROW(appender.EndRow());
			REQUIRE_NOTHROW(appender.Close());
		}
		result = con.Query("SELECT * FROM integers");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(1)}));
		REQUIRE(CHECK_COLUMN(result, 1, {current_time}));
		con.Query("COMMIT");
	}
}

TEST_CASE("Test incorrect usage of appender", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// create a table to append to
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER, j INTEGER)"));

	// append a bunch of values
	{
		Appender appender(con, "integers");
		appender.BeginRow();
		appender.Append<int32_t>(1);
		// call EndRow before all rows have been appended results in an exception
		REQUIRE_THROWS(appender.EndRow());
		// we can still close the appender
		REQUIRE_NOTHROW(appender.Close());
	}
	{
		Appender appender(con, "integers");
		// flushing results in the same error
		appender.BeginRow();
		appender.Append<int32_t>(1);
		REQUIRE_THROWS(appender.Flush());
		// we can still close the appender
		REQUIRE_NOTHROW(appender.Close());
	}
	{
		// we get the same exception when calling AppendRow with an incorrect number of arguments
		Appender appender(con, "integers");
		REQUIRE_THROWS(appender.AppendRow(1));
		// we can still close the appender
		REQUIRE_NOTHROW(appender.Close());
	}
	{
		// we can flush an empty appender
		Appender appender(con, "integers");
		REQUIRE_NOTHROW(appender.Flush());
		REQUIRE_NOTHROW(appender.Flush());
		REQUIRE_NOTHROW(appender.Flush());
	}
}

TEST_CASE("Test appending NaN and INF using appender", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE doubles(d DOUBLE, f REAL)"));

	// appending NAN or INF succeeds
	Appender appender(con, "doubles");
	appender.AppendRow(1e308 + 1e308, 1e38f * 1e38f);
	appender.AppendRow(NAN, NAN);
	appender.Close();

	result = con.Query("SELECT * FROM doubles");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::DOUBLE(1e308 + 1e308), Value::DOUBLE(NAN)}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::FLOAT(1e38f * 1e38f), Value::FLOAT(NAN)}));
}

TEST_CASE("Test appender with quotes", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA \"my_schema\""));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE \"my_schema\".\"my_table\"(\"i\" INTEGER)"));

	// append a bunch of values
	{
		Appender appender(con, "my_schema", "my_table");
		appender.AppendRow(1);
		appender.Close();
	}
	result = con.Query("SELECT * FROM my_schema.my_table");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}

TEST_CASE("Test appender with string lengths", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE my_table (s STRING)"));
	{
		Appender appender(con, "my_table");
		appender.BeginRow();
		appender.Append("asdf", 3);
		appender.EndRow();
		appender.Close();
	}
	result = con.Query("SELECT * FROM my_table");
	REQUIRE(CHECK_COLUMN(result, 0, {"asd"}));
}

TEST_CASE("Test various appender types", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE type_table(a BOOL, b UINT8, c UINT16, d UINT32, e UINT64, f FLOAT)"));
	{
		Appender appender(con, "type_table");
		appender.AppendRow(true, uint8_t(1), uint16_t(2), uint32_t(3), uint64_t(4), 5.0f);
	}
	result = con.Query("SELECT * FROM type_table");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(CHECK_COLUMN(result, 1, {1}));
	REQUIRE(CHECK_COLUMN(result, 2, {2}));
	REQUIRE(CHECK_COLUMN(result, 3, {3}));
	REQUIRE(CHECK_COLUMN(result, 4, {4}));
	REQUIRE(CHECK_COLUMN(result, 5, {5}));
	// too many rows
	{
		Appender appender(con, "type_table");
		REQUIRE_THROWS(appender.AppendRow(true, uint8_t(1), uint16_t(2), uint32_t(3), uint64_t(4), 5.0f, nullptr));
	}
	{
		Appender appender(con, "type_table");
		REQUIRE_THROWS(appender.AppendRow(true, 1, 2, 3, 4, 5, 1));
	}
}

TEST_CASE("Test alter table in the middle of append", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	// create a table to append to
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER, j INTEGER)"));
	{
		// create the appender
		Appender appender(con, "integers");
		appender.AppendRow(1, 2);

		REQUIRE_NO_FAIL(con.Query("ALTER TABLE integers DROP COLUMN i"));
		REQUIRE_THROWS(appender.Close());
	}
}

TEST_CASE("Test appending to a different database file", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	auto test_dir = TestDirectoryPath();
	auto attach_query = "ATTACH '" + test_dir + "/append_to_other.db'";
	REQUIRE_NO_FAIL(con.Query(attach_query));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE append_to_other.tbl(i INTEGER)"));

	Appender appender(con, "append_to_other", "main", "tbl");
	for (idx_t i = 0; i < 200; i++) {
		appender.BeginRow();
		appender.Append<int32_t>(2);
		appender.EndRow();
	}
	appender.Close();

	result = con.Query("SELECT SUM(i) FROM append_to_other.tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {400}));
	bool failed;

	try {
		failed = false;
		Appender appender_invalid(con, "invalid_database", "main", "tbl");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("Catalog Error") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);

	try {
		failed = false;
		Appender appender_invalid(con, "append_to_other", "invalid_schema", "tbl");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("Catalog Error") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);

	// Attach as readonly.
	REQUIRE_NO_FAIL(con.Query("DETACH append_to_other"));
	REQUIRE_NO_FAIL(con.Query(attach_query + " (readonly)"));

	try {
		failed = false;
		Appender appender_readonly(con, "append_to_other", "main", "tbl");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("Cannot append to a readonly database") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);
}

TEST_CASE("Test appending to different database files", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	auto test_dir = TestDirectoryPath();
	auto attach_db1 = "ATTACH '" + test_dir + "/db1.db'";
	auto attach_db2 = "ATTACH '" + test_dir + "/db2.db'";
	REQUIRE_NO_FAIL(con.Query(attach_db1));
	REQUIRE_NO_FAIL(con.Query(attach_db2));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE db1.tbl(i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE db2.tbl(i INTEGER)"));

	REQUIRE_NO_FAIL(con.Query("START TRANSACTION"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO db1.tbl VALUES (1)"));

	Appender appender(con, "db2", "main", "tbl");
	appender.BeginRow();
	appender.Append<int32_t>(2);
	appender.EndRow();

	bool failed;
	try {
		failed = false;
		appender.Close();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("a single transaction can only write to a single attached database") !=
		        std::string::npos);
		failed = true;
	}
	REQUIRE(failed);
	REQUIRE_NO_FAIL(con.Query("COMMIT TRANSACTION"));
}

TEST_CASE("Test appending with an active default column", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(
	    con.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30, l AS (random()))"));
	Appender appender(con, "main", "tbl");
	appender.AddColumn("i");

	DataChunk chunk;
	const duckdb::vector<LogicalType> types = {LogicalType::INTEGER};
	chunk.Initialize(*con.context, types);

	chunk.data[0].Append(Value::INTEGER(42));
	chunk.data[0].Append(Value::INTEGER(43));
	chunk.SetChildCardinality(2);
	appender.AppendDataChunk(chunk);
	appender.Close();

	result = con.Query("SELECT i, j, k, l IS NOT NULL FROM tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {42, 43}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value(), Value()}));
	REQUIRE(CHECK_COLUMN(result, 2, {30, 30}));
	REQUIRE(CHECK_COLUMN(result, 3, {true, true}));
}

TEST_CASE("Test appending with two active normal columns", "[appender]") {
#if STANDARD_VECTOR_SIZE != 2048
	return;
#endif
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(
	    con.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30, l AS (2 * j), m INT)"));
	Appender appender(con, "main", "tbl");
	appender.AddColumn("j");
	appender.AddColumn("m");

	DataChunk chunk;
	const duckdb::vector<LogicalType> types = {LogicalType::INTEGER, LogicalType::INTEGER};
	chunk.Initialize(*con.context, types);

	for (idx_t i = 0; i < 4; i++) {
		for (idx_t j = 0; j < 2; j++) {
			auto &col = chunk.data[j];
			auto col_data = FlatVector::Writer<int32_t>(col, STANDARD_VECTOR_SIZE);

			auto offset = i * STANDARD_VECTOR_SIZE;
			for (idx_t k = 0; k < STANDARD_VECTOR_SIZE; k++) {
				col_data.WriteValue(static_cast<int32_t>(offset + k));
			}
		}
		chunk.SetChildCardinality(STANDARD_VECTOR_SIZE);
		appender.AppendDataChunk(chunk);
		chunk.Reset();
	}
	appender.Close();

	result = con.Query("SELECT SUM(i), SUM(j), SUM(k), SUM(l), SUM(m) FROM tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {32768}));
	REQUIRE(CHECK_COLUMN(result, 1, {33550336}));
	REQUIRE(CHECK_COLUMN(result, 2, {245760}));
	REQUIRE(CHECK_COLUMN(result, 3, {67100672}));
	REQUIRE(CHECK_COLUMN(result, 4, {33550336}));
}

TEST_CASE("Test changing the active column configuration", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30)"));
	Appender appender(con, "main", "tbl");

	// Create a data chunk with all three columns filled.

	DataChunk chunk_all_types;
	const duckdb::vector<LogicalType> all_types = {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER};
	chunk_all_types.Initialize(*con.context, all_types);

	chunk_all_types.data[0].Append(Value::INTEGER(42));
	chunk_all_types.data[1].Append(Value::INTEGER(111));
	chunk_all_types.data[2].Append(Value::INTEGER(50));

	chunk_all_types.SetChildCardinality(1);
	appender.AppendDataChunk(chunk_all_types);

	appender.AddColumn("j");
	appender.AddColumn("i");

	DataChunk chunk_j_i;
	const duckdb::vector<LogicalType> types_j_i = {LogicalType::INTEGER, LogicalType::INTEGER};
	chunk_j_i.Initialize(*con.context, types_j_i);

	chunk_j_i.data[0].Append(Value::INTEGER(111));
	chunk_j_i.data[1].Append(Value::INTEGER(42));

	chunk_j_i.SetChildCardinality(1);
	appender.AppendDataChunk(chunk_j_i);

	appender.ClearColumns();
	appender.AppendDataChunk(chunk_all_types);

	appender.AddColumn("k");

	DataChunk chunk_k;
	const duckdb::vector<LogicalType> types_k = {LogicalType::INTEGER};
	chunk_k.Initialize(*con.context, types_k);

	chunk_k.data[0].Append(Value::INTEGER(50));

	chunk_k.SetChildCardinality(1);
	appender.AppendDataChunk(chunk_k);
	appender.Close();

	result = con.Query("SELECT i, j, k FROM tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {42, 42, 42, 4}));
	REQUIRE(CHECK_COLUMN(result, 1, {111, 111, 111, Value()}));
	REQUIRE(CHECK_COLUMN(result, 2, {50, 30, 50, 50}));
}

TEST_CASE("Test edge cases for the active column configuration", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30, l AS (2 * j))"));
	Appender appender(con, "main", "tbl");

	appender.AddColumn("i");
	appender.AddColumn("j");

	bool failed;
	// Cannot add columns that do not exist.
	try {
		failed = false;
		appender.AddColumn("hello");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("the column must exist in the table") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);

	// Cannot add generated columns.
	try {
		failed = false;
		appender.AddColumn("l");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("cannot add a generated column to the appender") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);

	// Cannot add the same column twice.
	try {
		failed = false;
		appender.AddColumn("j");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("cannot add the same column twice") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);
	appender.Close();
}

TEST_CASE("Test appending rows with an active column list", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(
	    con.Query("CREATE OR REPLACE TABLE tbl (i INT DEFAULT 4, j INT, k INT DEFAULT 30, l AS (2 * j), m INT)"));
	Appender appender(con, "main", "tbl");
	appender.AddColumn("j");
	appender.AddColumn("m");

	appender.Append(42);
	appender.Append(43);
	appender.EndRow();

	appender.Append(Value());
	appender.Append(44);
	appender.EndRow();
	appender.Close();

	result = con.Query("SELECT i, j, k, l, m FROM tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {4, 4}));
	REQUIRE(CHECK_COLUMN(result, 1, {42, Value()}));
	REQUIRE(CHECK_COLUMN(result, 2, {30, 30}));
	REQUIRE(CHECK_COLUMN(result, 3, {84, Value()}));
	REQUIRE(CHECK_COLUMN(result, 4, {43, 44}));
}

TEST_CASE("Appender::Clear() clears the data", "[appender]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ints(i INTEGER)"));
	Appender appender(con, "main", "ints");

	// We will append rows to reach more than the maximum chunk size
	// (DEFAULT_FLUSH_COUNT will always be more than a chunk size),
	// so we will make sure that also the collection is being cleared.
	// We use the DEFAULT_FLUSH_COUNT so we won't flush before calling the `Clear`
	constexpr auto rows_to_append = BaseAppender::DEFAULT_FLUSH_COUNT - 10;

	for (idx_t i = 0; i < rows_to_append; i++) {
		appender.AppendRow(i);
	}

	// We're clearing, which means we're expecting to have only the `1` appended after the clear.
	appender.Clear();
	appender.AppendRow(1);
	appender.Close();

	duckdb::unique_ptr<QueryResult> result = con.Query("SELECT * FROM ints");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}

TEST_CASE("Interrupted QueryAppender flow: interrupt -> clear -> close finishes", "[appender]") {
	std::promise<void> execution_started;
	std::promise<void> resume_execution;
	auto started = execution_started.get_future();
	auto resume = resume_execution.get_future();
	atomic<idx_t> execution_count {0};
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ints(i INTEGER)"));

	ScalarFunction probe("interrupt_probe", {LogicalType::INTEGER}, LogicalType::INTEGER,
	                     [&](DataChunk &args, ExpressionState &, Vector &result) {
		                     if (execution_count.fetch_add(1) == 0) {
			                     execution_started.set_value();
			                     resume.wait();
		                     }
		                     result.Reference(args.data[0]);
	                     });
	probe.SetVolatile();
	CreateScalarFunctionInfo info(probe);
	con.context->RegisterFunction(info);

	duckdb::vector<LogicalType> types = {LogicalType::INTEGER};
	duckdb::vector<duckdb::Identifier> names = {"i"};
	QueryAppender app(con, "INSERT INTO ints SELECT interrupt_probe(i) FROM appended_data", types, names);
	app.AppendRow(1);

	struct ResumeAndWait {
		std::promise<void> &resume;
		std::future<void> &flush;

		~ResumeAndWait() {
			resume.set_value();
			flush.wait();
		}
	};

	auto flush = std::async(std::launch::async, [&]() { app.Flush(); });
	bool reached_execution;
	{
		ResumeAndWait cleanup {resume_execution, flush};
		// Query initialization clears interrupts, so wait until execution has begun.
		reached_execution = started.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
		// Also request cancellation on timeout before releasing and waiting for the worker.
		con.Interrupt();
	}
	REQUIRE(reached_execution);

	bool interrupted = false;
	try {
		flush.get();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		INFO(error.Message());
		REQUIRE(error.Type() == ExceptionType::INTERRUPT);
		interrupted = true;
	}
	REQUIRE(interrupted);
	REQUIRE(execution_count.load() == 1);

	idx_t rows_to_discard = 0;
	bool append_after_clear = false;
	SECTION("Clear after the interrupted flush") {
		rows_to_discard = 0;
	}
	SECTION("Clear also discards newly buffered rows") {
		rows_to_discard = STANDARD_VECTOR_SIZE + 1;
	}
	SECTION("Fresh rows can be appended after Clear") {
		rows_to_discard = STANDARD_VECTOR_SIZE + 1;
		append_after_clear = true;
	}
	// Flush resets its collection on error; also exercise Clear with both buffers populated.
	for (idx_t i = 0; i < rows_to_discard; i++) {
		app.AppendRow(2);
	}
	app.Clear();
	REQUIRE(execution_count.load() == 1);
	if (append_after_clear) {
		app.AppendRow(3);
	}

	REQUIRE_NOTHROW(app.Close());
	auto result = con.Query("SELECT i FROM ints");
	REQUIRE_NO_FAIL(*result);
	if (append_after_clear) {
		REQUIRE(execution_count.load() == 2);
		REQUIRE(CHECK_COLUMN(result, 0, {3}));
	} else {
		REQUIRE(execution_count.load() == 1);
		REQUIRE(result->RowCount() == 0);
	}
}

TEST_CASE("Test appender_allocator_flush_threshold", "[appender]") {
	duckdb::unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	const size_t blob_size = 100 * 1024;
	std::vector<uint8_t> data(blob_size, 'A');
	auto value = duckdb::Value::BLOB(data.data(), data.size());
	REQUIRE_NO_FAIL(con.Query("SET GLOBAL memory_limit='1GB'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE my_table (b BLOB)"));

	// Flush when call `EndRow`
	Appender appender_1(con, "my_table", 16 * 1024 * 1024);
	for (int i = 0; i < 10000; i++) {
		appender_1.BeginRow();
		appender_1.Append(value);
		appender_1.EndRow();
	}
	appender_1.Close();

	// Flush when call `FlushChunk`
	Appender appender_2(con, "my_table", 64 * 1024);
	for (int i = 0; i < 10000; i++) {
		appender_2.BeginRow();
		appender_2.Append(value);
		appender_2.EndRow();
	}
	appender_2.Close();
}

TEST_CASE("Test appender on a temp table in a read only database", "[appender]") {
	auto dbdir = TestCreatePath("appender_temp_readonly");
	DeleteDatabase(dbdir);

	// Create the persistent database file, then reopen it read only.
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE persistent(i INTEGER)"));
	}

	DBConfig readonly_config;
	readonly_config.options.access_mode = AccessMode::READ_ONLY;
	DuckDB db(dbdir, &readonly_config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t(i INTEGER)"));

	// The name resolves through the (writable) temp catalog, not the read only default database.
	auto info = con.TableInfo("t");
	REQUIRE(info);
	REQUIRE(info->qualified_name.Catalog() == "temp");
	REQUIRE(info->qualified_name.Schema() == "main");
	REQUIRE(info->qualified_name.Name() == "t");
	REQUIRE(info->readonly == false);

	Appender appender(con, "t");
	appender.BeginRow();
	appender.Append<int32_t>(42);
	appender.EndRow();
	appender.Close();

	auto result = con.Query("SELECT i FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
}

TEST_CASE("Test appender when the search path resolves into a read only database", "[appender]") {
	auto dbdir = TestCreatePath("appender_readonly_attach.db");
	DeleteDatabase(dbdir);

	// Create a persistent database file holding the target table.
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl(i INTEGER)"));
	}

	// Default (in-memory) database is writable; the table only exists in the read only attachment.
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + dbdir + "' AS ro_attached (READ_ONLY)"));
	// Keep memory.main as the default catalog so the name resolves into ro_attached via the search path.
	REQUIRE_NO_FAIL(con.Query("SET search_path='memory.main,ro_attached.main'"));

	auto info = con.TableInfo("tbl");
	REQUIRE(info);
	REQUIRE(info->qualified_name.Catalog() == "ro_attached");
	REQUIRE(info->readonly == true);

	bool failed = false;
	try {
		Appender appender(con, "tbl");
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Message().find("Cannot append to a readonly database") != std::string::npos);
		failed = true;
	}
	REQUIRE(failed);
}
