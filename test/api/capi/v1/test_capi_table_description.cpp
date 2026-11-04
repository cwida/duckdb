#include "capi_tester.hpp"
#include "duckdb.h"

using namespace duckdb;

TEST_CASE("Test the table description in the C API", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	duckdb_table_description table_description = nullptr;
	tester.Query("SET threads=1;");

	// Test a non-existent table.
	auto status = duckdb_table_description_create(tester.connection, nullptr, "test", &table_description);
	REQUIRE(status == DuckDBError);
	duckdb_table_description_destroy(&table_description);

	status = duckdb_table_description_create_ext(tester.connection, "hello", "world", "test", &table_description);
	REQUIRE(status == DuckDBError);
	duckdb_table_description_destroy(&table_description);

	// Create an in-memory table and a table in an external file.
	tester.Query("CREATE TABLE test (i INTEGER, j INTEGER default 5)");
	auto test_dir = TestDirectoryPath();
	auto attach_query = "ATTACH '" + test_dir + "/ext_description.db'";
	tester.Query(attach_query);
	tester.Query("CREATE TABLE ext_description.test(my_column INTEGER)");

	// Test invalid catalog and schema.
	status =
	    duckdb_table_description_create_ext(tester.connection, "non-existent", nullptr, "test", &table_description);
	REQUIRE(status == DuckDBError);
	duckdb_table_description_destroy(&table_description);

	status = duckdb_table_description_create(tester.connection, "non-existent", "test", &table_description);
	REQUIRE(status == DuckDBError);
	duckdb_table_description_destroy(&table_description);

	status = duckdb_table_description_create(tester.connection, nullptr, "test", &table_description);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_table_description_error(table_description) == nullptr);

	bool has_default;
	SECTION("Passing nullptr to has_default") {
		REQUIRE(duckdb_column_has_default(table_description, 2, nullptr) == DuckDBError);
		REQUIRE(duckdb_column_has_default(nullptr, 2, &has_default) == DuckDBError);
	}
	SECTION("Out of range column for has_default") {
		REQUIRE(duckdb_column_has_default(table_description, 2, &has_default) == DuckDBError);
	}
	SECTION("In range column - not default") {
		REQUIRE(duckdb_column_has_default(table_description, 0, &has_default) == DuckDBSuccess);
		REQUIRE(has_default == false);
	}
	SECTION("In range column - default") {
		REQUIRE(duckdb_column_has_default(table_description, 1, &has_default) == DuckDBSuccess);
		REQUIRE(has_default == true);
	}
	duckdb_table_description_destroy(&table_description);

	// Let's get information about the external table.
	status =
	    duckdb_table_description_create_ext(tester.connection, "ext_description", nullptr, "test", &table_description);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_table_description_error(table_description) == nullptr);

	SECTION("Passing nullptr to get_column_count") {
		REQUIRE(duckdb_table_description_get_column_count(nullptr) == 0);
	}
	SECTION("Passing nullptr to get_name") {
		REQUIRE(duckdb_table_description_get_column_name(nullptr, 0) == nullptr);
	}
	SECTION("Passing nullptr to get_type") {
		REQUIRE(duckdb_table_description_get_column_type(nullptr, 0) == nullptr);
	}
	SECTION("Out of range column for get_name") {
		REQUIRE(duckdb_table_description_get_column_name(table_description, 1) == nullptr);
	}
	SECTION("Out of range column for get_type") {
		REQUIRE(duckdb_table_description_get_column_type(table_description, 1) == nullptr);
	}
	SECTION("get the column count") {
		auto column_count = duckdb_table_description_get_column_count(table_description);
		REQUIRE(column_count == 1);
	}
	SECTION("In range column - get the name") {
		auto column_name = duckdb_table_description_get_column_name(table_description, 0);
		string expected = "my_column";
		REQUIRE(!expected.compare(column_name));
		duckdb_free(column_name);
	}
	SECTION("In range column - get the type") {
		auto column_type = duckdb_table_description_get_column_type(table_description, 0);
		auto type_id = duckdb_get_type_id(column_type);
		REQUIRE(type_id == DUCKDB_TYPE_INTEGER);
		duckdb_destroy_logical_type(&column_type);
	}
	duckdb_table_description_destroy(&table_description);
}

TEST_CASE("Test getting the table names of a query in the C API", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	tester.Query("CREATE SCHEMA schema1");
	tester.Query("CREATE SCHEMA \"schema.2\"");
	tester.Query("CREATE TABLE schema1.\"table.1\"(i INT)");
	tester.Query("CREATE TABLE \"schema.2\".\"table.2\"(i INT)");

	string query = "SELECT * FROM schema1.\"table.1\", \"schema.2\".\"table.2\"";
	auto table_name_values = duckdb_get_table_names(tester.connection, query.c_str(), true);

	auto size = duckdb_get_list_size(table_name_values);
	REQUIRE(size == 2);

	duckdb::unordered_set<string> expected_names = {"schema1.\"table.1\"", "\"schema.2\".\"table.2\""};
	for (idx_t i = 0; i < size; i++) {
		auto name_value = duckdb_get_list_child(table_name_values, i);
		auto name = duckdb_get_varchar(name_value);
		REQUIRE(expected_names.count(name) == 1);
		duckdb_free(name);
		duckdb_destroy_value(&name_value);
	}

	duckdb_destroy_value(&table_name_values);
	tester.Cleanup();
}

TEST_CASE("Test the table description of a view keeps the open transaction in the C API", "[capi]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE u(k INTEGER)"));
	REQUIRE_NO_FAIL(tester.Query("CREATE VIEW vw AS SELECT 1 AS k"));

	REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO u VALUES (1)"));
	duckdb_table_description table_description = nullptr;
	auto status = duckdb_table_description_create(tester.connection, nullptr, "vw", &table_description);
	REQUIRE(status == DuckDBError);
	REQUIRE(string(duckdb_table_description_error(table_description)) ==
	        "No table with that schema+name could be located");
	duckdb_table_description_destroy(&table_description);
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO u VALUES (2)"));
	REQUIRE_NO_FAIL(tester.Query("COMMIT"));

	auto result = tester.Query("SELECT count(*), sum(k) FROM u");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 2);
	REQUIRE(result->Fetch<int64_t>(1, 0) == 3);
}
