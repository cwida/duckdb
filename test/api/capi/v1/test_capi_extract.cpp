#include "capi_tester.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/parser/statement/set_statement.hpp"

using namespace duckdb;

TEST_CASE("Test extract statements in C API", "[capi]") {
	CAPITester tester;
	duckdb_result res;
	duckdb_extracted_statements stmts = nullptr;
	duckdb_state status;
	const char *error;
	duckdb_prepared_statement prepared = nullptr;

	REQUIRE(tester.OpenDatabase(nullptr));

	idx_t size = duckdb_extract_statements(tester.connection,
	                                       "CREATE TABLE tbl (col INT); INSERT INTO tbl VALUES (1), (2), (3), (4); "
	                                       "SELECT COUNT(col) FROM tbl WHERE col > $1",
	                                       &stmts);

	REQUIRE(size == 3);
	REQUIRE(stmts != nullptr);

	for (idx_t i = 0; i + 1 < size; i++) {
		status = duckdb_prepare_extracted_statement(tester.connection, stmts, i, &prepared);
		REQUIRE(status == DuckDBSuccess);
		status = duckdb_execute_prepared(prepared, &res);
		REQUIRE(status == DuckDBSuccess);
		duckdb_destroy_prepare(&prepared);
		duckdb_destroy_result(&res);
	}

	duckdb_prepared_statement stmt = nullptr;
	status = duckdb_prepare_extracted_statement(tester.connection, stmts, size - 1, &stmt);
	REQUIRE(status == DuckDBSuccess);
	duckdb_bind_int32(stmt, 1, 1);
	status = duckdb_execute_prepared(stmt, &res);
	REQUIRE(status == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&res, 0, 0) == 3);
	duckdb_destroy_prepare(&stmt);
	duckdb_destroy_result(&res);
	duckdb_destroy_extracted(&stmts);

	//	 test empty statement is not an error
	size = duckdb_extract_statements(tester.connection, "", &stmts);
	REQUIRE(size == 0);
	error = duckdb_extract_statements_error(stmts);
	REQUIRE(error == nullptr);
	duckdb_destroy_extracted(&stmts);

	//	 test incorrect statement cannot be extracted
	size = duckdb_extract_statements(tester.connection, "This is not valid SQL", &stmts);
	REQUIRE(size == 0);
	error = duckdb_extract_statements_error(stmts);
	REQUIRE(error != nullptr);
	duckdb_destroy_extracted(&stmts);

	// test out of bounds
	size = duckdb_extract_statements(tester.connection, "SELECT CAST($1 AS BIGINT)", &stmts);
	REQUIRE(size == 1);
	status = duckdb_prepare_extracted_statement(tester.connection, stmts, 2, &prepared);
	REQUIRE(status == DuckDBError);
	duckdb_destroy_extracted(&stmts);
}

TEST_CASE("Test invalid PRAGMA in C API", "[capi]") {
	duckdb_database db;
	duckdb_connection con;
	const char *err_msg;

	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);

	// Extraction only parses, so the unknown PRAGMA function is reported when the statement is prepared
	duckdb_extracted_statements stmts;
	auto size = duckdb_extract_statements(con, "PRAGMA something;", &stmts);
	REQUIRE(size == 1);

	duckdb_prepared_statement prepared = nullptr;
	REQUIRE(duckdb_prepare_extracted_statement(con, stmts, 0, &prepared) == DuckDBError);
	err_msg = duckdb_prepare_error(prepared);
	REQUIRE(err_msg != nullptr);
	REQUIRE(string(err_msg).find("Catalog Error") != std::string::npos);

	duckdb_destroy_prepare(&prepared);
	duckdb_destroy_extracted(&stmts);
	duckdb_disconnect(&con);
	duckdb_close(&db);
}

TEST_CASE("Test extract statements for 'ALTER TABLE ... ADD COLUMN ... DEFAULT' in C API", "[capi]") {
	CAPITester tester;
	duckdb_extracted_statements stmts = nullptr;

	REQUIRE(tester.OpenDatabase(nullptr));

	idx_t size = duckdb_extract_statements(
	    tester.connection,
	    "ALTER TABLE my_table ADD COLUMN my_column TIMESTAMP WITH TIME ZONE DEFAULT non_existent_function();", &stmts);

	// The engine expands the ALTER when it is submitted, so extraction yields it as written
	REQUIRE(size == 1);
	REQUIRE(stmts != nullptr);
	auto wrapper = (ExtractStatementsWrapper *)stmts;

	REQUIRE(wrapper->statements[0]->query ==
	        "ALTER TABLE my_table ADD COLUMN my_column TIMESTAMP WITH TIME ZONE DEFAULT non_existent_function();");

	duckdb_destroy_extracted(&stmts);
}

TEST_CASE("Test a parser error while extracting statements invalidates the transaction in C API", "[capi]") {
	CAPITester tester;
	duckdb_extracted_statements stmts = nullptr;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(tester.Query("BEGIN"));
	REQUIRE_NO_FAIL(tester.Query("SET current_transaction_invalidation_policy='ALL_ERRORS_INVALIDATE_TRANSACTION'"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO audit VALUES (21)"));
	auto size = duckdb_extract_statements(tester.connection, "inser into audit_backup from audit", &stmts);
	REQUIRE(size == 0);
	REQUIRE(duckdb_extract_statements_error(stmts) != nullptr);
	duckdb_destroy_extracted(&stmts);
	REQUIRE_FAIL(tester.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(tester.Query("ABORT"));

	auto result = tester.Query("SELECT COUNT(*) FROM audit");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
}

TEST_CASE("Test a parser error while getting table names leaves the transaction usable in C API", "[capi]") {
	CAPITester tester;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE audit(i INT)"));

	REQUIRE_NO_FAIL(tester.Query("BEGIN"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO audit VALUES (21)"));
	REQUIRE(duckdb_get_table_names(tester.connection, "inser into audit_backup from audit", false) == nullptr);
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO audit VALUES (22)"));
	REQUIRE_NO_FAIL(tester.Query("COMMIT"));

	auto result = tester.Query("SELECT COUNT(*) FROM audit");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 2);
}
