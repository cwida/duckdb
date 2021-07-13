#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "test_helpers.hpp"
#include "duckdb/storage/storage_info.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test storage that exceeds a single block", "[storage]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	uint64_t integer_count = 3 * (Storage::BLOCK_SIZE / sizeof(int32_t));
	uint64_t expected_sum;
	Value sum;

	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert values
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a INTEGER, b INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (11, 22), (13, 22), (12, 21), (NULL, NULL)"));
		uint64_t table_size = 4;
		expected_sum = 11 + 12 + 13 + 22 + 22 + 21;
		// grow the table until it exceeds integer_count
		while (table_size < integer_count) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM test"));
			table_size *= 2;
			expected_sum *= 2;
		}
		sum = Value::BIGINT(expected_sum);
		// compute the sum
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	// reload the database from disk
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	// reload the database from disk, we do this again because checkpointing at startup causes this to follow a
	// different code path
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	DeleteDatabase(storage_database);
}

TEST_CASE("Test storage that exceeds a single block with different types", "[storage]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	uint64_t integer_count = 3 * (Storage::BLOCK_SIZE / sizeof(int32_t));
	Value sum;

	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert values
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a INTEGER, b BIGINT);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (11, 22), (13, 22), (12, 21), (NULL, NULL)"));
		uint64_t table_size = 4;
		// grow the table until it exceeds integer_count
		while (table_size < integer_count) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM test"));
			table_size *= 2;
		}
		// compute the sum
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE_NO_FAIL(*result);
		sum = result->GetValue(0, 0);
	}
	// reload the database from disk
	for (idx_t i = 0; i < 2; i++) {
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT SUM(a) + SUM(b) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {sum}));
	}
	DeleteDatabase(storage_database);
}

TEST_CASE("Test storing strings that exceed a single block", "[storage]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	uint64_t string_count = 3 * (Storage::BLOCK_SIZE / (sizeof(char) * 15));
	Value sum;

	Value count_per_group;
	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert values
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a VARCHAR);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES ('a'), ('bb'), ('ccc'), ('dddd'), ('eeeee')"));
		uint64_t table_size = 5;
		// grow the table until it exceeds integer_count
		while (table_size < string_count) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM test"));
			table_size *= 2;
		}
		count_per_group = Value::BIGINT(table_size / 5);
		// compute the sum
		result = con.Query("SELECT a, COUNT(*) FROM test GROUP BY a ORDER BY a");
		REQUIRE(CHECK_COLUMN(result, 0, {"a", "bb", "ccc", "dddd", "eeeee"}));
		REQUIRE(CHECK_COLUMN(result, 1,
		                     {count_per_group, count_per_group, count_per_group, count_per_group, count_per_group}));
	}
	// reload the database from disk
	for (idx_t i = 0; i < 2; i++) {
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT a, COUNT(*) FROM test GROUP BY a ORDER BY a");
		REQUIRE(CHECK_COLUMN(result, 0, {"a", "bb", "ccc", "dddd", "eeeee"}));
		REQUIRE(CHECK_COLUMN(result, 1,
		                     {count_per_group, count_per_group, count_per_group, count_per_group, count_per_group}));
	}
	// now perform an update of the database
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT count(a) FROM test WHERE a='a'");
		REQUIRE(CHECK_COLUMN(result, 0, {count_per_group}));
		result = con.Query("UPDATE test SET a='aaa' WHERE a='a'");
		REQUIRE(CHECK_COLUMN(result, 0, {count_per_group}));
	}
	// reload the database from disk again
	for (idx_t i = 0; i < 2; i++) {
		DuckDB db(storage_database, config.get());
		Connection con(db);

		result = con.Query("SELECT a, COUNT(*) FROM test GROUP BY a ORDER BY a");
		REQUIRE(CHECK_COLUMN(result, 0, {"aaa", "bb", "ccc", "dddd", "eeeee"}));
		REQUIRE(CHECK_COLUMN(result, 1,
		                     {count_per_group, count_per_group, count_per_group, count_per_group, count_per_group}));
	}

	DeleteDatabase(storage_database);
}

TEST_CASE("Test storing big strings", "[storage]") {
	unique_ptr<MaterializedQueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();

	uint64_t string_length = 64;
	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert the big string
		DuckDB db(storage_database, config.get());
		Connection con(db);
		string big_string = string(string_length, 'a');
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a VARCHAR, j BIGINT);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES ('" + big_string + "', 1)"));
		uint64_t iteration = 2;
		while (string_length < Storage::BLOCK_SIZE * 2) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT a||a||a||a||a||a||a||a||a||a, " + to_string(iteration) +
			                          " FROM test"));
			REQUIRE_NO_FAIL(con.Query("DELETE FROM test WHERE j=" + to_string(iteration - 1)));
			iteration++;
			string_length *= 10;
		}

		// check the length
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
	}
	// reload the database from disk
	for (idx_t i = 0; i < 2; i++) {
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
	}
	DeleteDatabase(storage_database);
}
