#include "arrow/arrow_test_helper.hpp"
#include "catch.hpp"
#include "duckdb/common/adbc/adbc.h"
#include "duckdb/common/adbc/adbc.hpp"
#include "duckdb/common/adbc/wrappers.hpp"
#include "duckdb/common/adbc/options.h"
#include "duckdb/common/arrow/nanoarrow/nanoarrow.hpp"
#include <iostream>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

namespace duckdb {

using duckdb_adbc::ConnectionRelease;
using duckdb_adbc::DatabaseInit;
using duckdb_adbc::DatabaseRelease;
using duckdb_adbc::InitializeADBCError;
using duckdb_adbc::StatementBindStream;
using duckdb_adbc::StatementExecuteQuery;
using duckdb_adbc::StatementSetOption;

bool SUCCESS(AdbcStatusCode status) {
	return status == ADBC_STATUS_OK;
}

const char *duckdb_lib = std::getenv("DUCKDB_INSTALL_LIB");

class ADBCTestDatabase {
public:
	explicit ADBCTestDatabase(const string &path_parameter = ":memory:") {
		InitializeADBCError(&adbc_error);
		if (path_parameter != ":memory:") {
			path = TestCreatePath(path_parameter);
		} else {
			path = path_parameter;
		}
		REQUIRE(duckdb_lib);
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", path.c_str(), &adbc_error)));

		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));
		// Create a separate connection for ingest operations to avoid streaming conflicts.
		// Using the same connection for both streaming query results and ingest would cause issues
		// because the streaming result holds the ClientContext lock.
		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection_ingest, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection_ingest, &adbc_database, &adbc_error)));
		arrow_stream.release = nullptr;
		arrow_stream_ingest.release = nullptr;
	}

	~ADBCTestDatabase() {
		if (arrow_stream.release) {
			arrow_stream.release(&arrow_stream);
			arrow_stream.release = nullptr;
		}
		if (arrow_stream_ingest.release) {
			arrow_stream_ingest.release(&arrow_stream_ingest);
			arrow_stream_ingest.release = nullptr;
		}
		// Release any existing error before calling Release functions
		InitializeADBCError(&adbc_error);
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection_ingest, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
		// Ensure error is released at the end
		InitializeADBCError(&adbc_error);
	}

	bool QueryAndCheck(const string &query) {
		QueryArrow(query);
		auto conn_wrapper = static_cast<DuckDBAdbcConnectionWrapper *>(adbc_connection.private_data);

		auto cconn = reinterpret_cast<Connection *>(conn_wrapper->connection);
		// Create a separate connection for arrow_scan to avoid deadlock.
		// When using streaming execution, the original connection's ClientContext is locked
		// while the stream is being consumed. Using the same connection for arrow_scan would
		// cause a deadlock because arrow_scan also needs to lock the ClientContext.
		Connection separate_conn(*cconn->context->db);
		return ArrowTestHelper::RunArrowComparison(separate_conn, query, arrow_stream);
	}

	unique_ptr<MaterializedQueryResult> Query(const string &query) {
		auto conn_wrapper = static_cast<DuckDBAdbcConnectionWrapper *>(adbc_connection.private_data);
		auto cconn = reinterpret_cast<Connection *>(conn_wrapper->connection);
		return cconn->Query(query);
	}

	ArrowArrayStream &QueryArrow(const string &query) {
		if (arrow_stream.release) {
			arrow_stream.release(&arrow_stream);
			arrow_stream.release = nullptr;
		}
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));
		// Release the statement
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
		return arrow_stream;
	}

	// Query using the ingest connection. Use this when creating data for temporary table ingestion
	// to avoid conflicts between the streaming result and DDL on the main connection.
	ArrowArrayStream &QueryArrowForIngest(const string &query) {
		if (arrow_stream_ingest.release) {
			arrow_stream_ingest.release(&arrow_stream_ingest);
			arrow_stream_ingest.release = nullptr;
		}
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream_ingest, &rows_affected, &adbc_error)));
		// Release the statement
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
		return arrow_stream_ingest;
	}

	void CreateTable(const string &table_name, ArrowArrayStream &input_data, string schema = "", bool temporary = false,
	                 string catalog = "") {
		REQUIRE(input_data.release);
		AdbcStatement adbc_statement;
		// Use separate connection for ingest to avoid streaming conflicts.
		// Exception: temporary tables are connection-specific, so we must use the main connection
		// for them to be visible to Query() calls on the same connection.
		auto &conn = temporary ? adbc_connection : adbc_connection_ingest;
		REQUIRE(SUCCESS(AdbcStatementNew(&conn, &adbc_statement, &adbc_error)));
		if (!catalog.empty()) {
			REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_CATALOG, catalog.c_str(),
			                                       &adbc_error)));
		}
		if (!schema.empty()) {
			REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, schema.c_str(),
			                                       &adbc_error)));
		}
		if (temporary) {
			REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TEMPORARY,
			                                       ADBC_OPTION_VALUE_ENABLED, &adbc_error)));
		}
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));

		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));
		// Release the statement
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
		if (input_data.release) {
			input_data.release(&input_data);
		}
		input_data.release = nullptr;
	}

	AdbcError adbc_error = {};
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;
	AdbcConnection adbc_connection_ingest;

	ArrowArrayStream arrow_stream;
	ArrowArrayStream arrow_stream_ingest;
	string path;
};

TEST_CASE("ADBC - Select 42", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	REQUIRE(db.QueryAndCheck("SELECT 42"));
}

TEST_CASE("ADBC - non-empty query without actual statements", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	REQUIRE(db.QueryAndCheck("--"));
}

TEST_CASE("ADBC - Cancel connection while consuming stream", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	AdbcStatement adbc_statement;
	ArrowArrayStream stream;
	stream.release = nullptr;

	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT i FROM range(100000000) t(i)", &db.adbc_error)));
	int64_t rows_affected = 0;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &stream, &rows_affected, &db.adbc_error)));
	// The stream must remain valid even after releasing the statement.
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));

	std::atomic<bool> got_chunk {false};
	std::atomic<bool> done {false};
	std::atomic<int> stream_status {0};
	std::string last_error;

	std::thread consumer([&]() {
		ArrowArray array;
		std::memset(&array, 0, sizeof(array));
		while (true) {
			int rc = stream.get_next(&stream, &array);
			if (rc != 0) {
				auto err = stream.get_last_error(&stream);
				if (err) {
					last_error = err;
				}
				stream_status.store(rc);
				break;
			}
			if (!array.release) {
				stream_status.store(0);
				break;
			}
			got_chunk.store(true);
			array.release(&array);
			std::memset(&array, 0, sizeof(array));
		}
		done.store(true);
	});

	// Wait until we started consuming at least one chunk.
	for (int i = 0; i < 200 && !got_chunk.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(got_chunk.load());

	// Cancel from another thread.
	REQUIRE(AdbcConnectionCancel(&db.adbc_connection, &db.adbc_error) == ADBC_STATUS_OK);

	// Wait for the consumer to observe the cancellation.
	for (int i = 0; i < 200 && !done.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	consumer.join();

	REQUIRE(stream_status.load() != 0);
	REQUIRE(last_error.find("Interrupted!") != std::string::npos);

	if (stream.release) {
		stream.release(&stream);
	}

	// After the stream is released, cancel on idle connection still succeeds.
	REQUIRE(AdbcConnectionCancel(&db.adbc_connection, &db.adbc_error) == ADBC_STATUS_OK);

	// Connection should be reusable after cancel.
	REQUIRE(db.QueryAndCheck("SELECT 1"));
}

TEST_CASE("ADBC - Cancel statement while consuming stream", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	AdbcStatement adbc_statement;
	ArrowArrayStream stream;
	stream.release = nullptr;

	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT i FROM range(100000000) t(i)", &db.adbc_error)));
	int64_t rows_affected = 0;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &stream, &rows_affected, &db.adbc_error)));

	std::atomic<bool> got_chunk {false};
	std::atomic<bool> done {false};
	std::atomic<int> stream_status {0};
	std::string last_error;

	std::thread consumer([&]() {
		ArrowArray array;
		std::memset(&array, 0, sizeof(array));
		while (true) {
			int rc = stream.get_next(&stream, &array);
			if (rc != 0) {
				auto err = stream.get_last_error(&stream);
				if (err) {
					last_error = err;
				}
				stream_status.store(rc);
				break;
			}
			if (!array.release) {
				stream_status.store(0);
				break;
			}
			got_chunk.store(true);
			array.release(&array);
			std::memset(&array, 0, sizeof(array));
		}
		done.store(true);
	});

	for (int i = 0; i < 200 && !got_chunk.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(got_chunk.load());

	REQUIRE(AdbcStatementCancel(&adbc_statement, &db.adbc_error) == ADBC_STATUS_OK);

	for (int i = 0; i < 200 && !done.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	consumer.join();

	REQUIRE(stream_status.load() != 0);
	REQUIRE(last_error.find("Interrupted!") != std::string::npos);

	if (stream.release) {
		stream.release(&stream);
	}
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));

	// After completion, cancelling again should fail.
	// Driver manager returns INVALID_STATE when statement->private_driver is nullptr (set by StatementRelease).
	REQUIRE(AdbcStatementCancel(&adbc_statement, &db.adbc_error) == ADBC_STATUS_INVALID_STATE);
	// Release the error set by the failed cancel
	InitializeADBCError(&db.adbc_error);

	// Connection should be reusable after cancel.
	REQUIRE(db.QueryAndCheck("SELECT 1"));
}

TEST_CASE("ADBC - Cancel unhappy paths", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	SECTION("Cancel on uninitialized connection") {
		AdbcError adbc_error = {};
		AdbcConnection adbc_connection = {};

		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
		// Connection is created but not initialized (Init not called)
		// Cancel should fail with INVALID_STATE
		REQUIRE(AdbcConnectionCancel(&adbc_connection, &adbc_error) == ADBC_STATUS_INVALID_STATE);
		if (adbc_error.release) {
			adbc_error.release(&adbc_error);
		}
		// Release the connection (no Init was called, so this should still work)
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	}

	SECTION("Cancel on released connection") {
		ADBCTestDatabase db;

		// Create and initialize a new connection
		AdbcConnection adbc_connection = {};
		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &db.adbc_database, &db.adbc_error)));

		// Release the connection
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &db.adbc_error)));

		// Cancel on released connection should fail
		REQUIRE(AdbcConnectionCancel(&adbc_connection, &db.adbc_error) == ADBC_STATUS_INVALID_STATE);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	SECTION("Cancel on idle statement (no active query)") {
		ADBCTestDatabase db;

		AdbcStatement adbc_statement = {};
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));

		// Cancel on a statement with no active query should succeed (no-op)
		REQUIRE(AdbcStatementCancel(&adbc_statement, &db.adbc_error) == ADBC_STATUS_OK);

		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));
	}

	SECTION("Cancel on released statement") {
		ADBCTestDatabase db;

		AdbcStatement adbc_statement = {};
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));

		// Cancel on released statement should fail
		// Driver manager returns INVALID_STATE when statement->private_driver is nullptr
		REQUIRE(AdbcStatementCancel(&adbc_statement, &db.adbc_error) == ADBC_STATUS_INVALID_STATE);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}
}

TEST_CASE("ADBC - Test ingestion", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");

	// Create Table 'my_table' from the Arrow Result
	db.CreateTable("my_table", input_data);

	REQUIRE(db.QueryAndCheck("SELECT * FROM my_table"));
}

TEST_CASE("ADBC - Test ingestion - Incorrect column count", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");

	// Create Table 'my_table' from the Arrow Result
	db.CreateTable("my_table", input_data);

	// Append data with extra column
	input_data = db.QueryArrow("SELECT 42 as value1, 29 as value2");
	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));
	REQUIRE(
	    SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table", &db.adbc_error)));
	// We need to use append mode
	REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND,
	                                       &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &db.adbc_error)));
	REQUIRE(!SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, nullptr, &db.adbc_error)));
	REQUIRE((std::strcmp(db.adbc_error.message, "incorrect column count in AppendDataChunk, expected 1, got 2") == 0));
	// Release error
	db.adbc_error.release(&db.adbc_error);
	InitializeADBCError(&db.adbc_error);
	// Release the statement
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));
	if (input_data.release) {
		input_data.release(&input_data);
	}
	input_data.release = nullptr;
}

TEST_CASE("ADBC - Test ingestion - Append to missing table", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	auto &input_data = db.QueryArrow("SELECT 42 as value");

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));
	REQUIRE(SUCCESS(
	    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "missing_table", &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND,
	                                       &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &db.adbc_error)));
	auto status = AdbcStatementExecuteQuery(&adbc_statement, nullptr, nullptr, &db.adbc_error);
	REQUIRE(!SUCCESS(status));
	REQUIRE(db.adbc_error.message);
	REQUIRE(StringUtil::Contains(db.adbc_error.message, "does not exist"));
	REQUIRE(StringUtil::Contains(db.adbc_error.message, "missing_table"));
	db.adbc_error.release(&db.adbc_error);
	InitializeADBCError(&db.adbc_error);
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));
	if (input_data.release) {
		input_data.release(&input_data);
	}
	input_data.release = nullptr;
}

TEST_CASE("ADBC - Test ingestion - Temporary Table", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Temporary and persistent tables with the same name should be distinct.
	// The temp table goes to 'temp' catalog, persistent table goes to 'memory' catalog.
	// Use QueryArrowForIngest to avoid streaming conflicts when creating temp tables.
	auto &input_temp = db.QueryArrowForIngest("SELECT 42 as value");
	db.CreateTable("my_table", input_temp, "", true);
	auto &input_persistent = db.QueryArrow("SELECT 84 as value");
	// Explicitly specify 'memory' catalog to create persistent table
	db.CreateTable("my_table", input_persistent, "main", false, "memory");

	{
		auto res_temp = db.Query("SELECT * FROM temp.my_table");
		REQUIRE(!res_temp->HasError());
		REQUIRE(res_temp->GetValue(0, 0).ToString() == "42");
	}
	{
		auto res_persistent = db.Query("SELECT * FROM memory.main.my_table");
		REQUIRE(!res_persistent->HasError());
		REQUIRE(res_persistent->GetValue(0, 0).ToString() == "84");
	}
}

TEST_CASE("ADBC - Test ingestion - Temporary Table - Persistent Without Catalog", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// create a temp table with data1 on adbc_connection
	auto &input_temp = db.QueryArrowForIngest("SELECT 1 AS idx, 'foo' AS value");
	db.CreateTable("my_table", input_temp, "", true);

	// create a persistent table with data2 on the SAME adbc_connection (no catalog/schema).
	// This triggers the bug: the appender resolves "my_table" to the temp table.
	{
		auto &input_persistent = db.QueryArrowForIngest("SELECT 2 AS idx, 'bar' AS value");
		AdbcStatement stmt = {};
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table", &db.adbc_error)));
		// No catalog, no schema, no temporary flag — should go to memory.main
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input_persistent, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
		if (input_persistent.release) {
			input_persistent.release(&input_persistent);
		}
	}

	// Temp table should still contain only data1 (idx=1)
	{
		auto res = db.Query("SELECT idx FROM temp.my_table ORDER BY idx");
		REQUIRE(!res->HasError());
		REQUIRE(res->RowCount() == 1);
		REQUIRE(res->GetValue(0, 0).ToString() == "1");
	}
	// Persistent table should contain data2 (idx=2)
	{
		auto res = db.Query("SELECT idx FROM memory.main.my_table ORDER BY idx");
		REQUIRE(!res->HasError());
		REQUIRE(res->RowCount() == 1);
		REQUIRE(res->GetValue(0, 0).ToString() == "2");
	}
}

TEST_CASE("ADBC - Test ingestion - Temporary Table - Schema Set", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	db.Query("CREATE SCHEMA my_schema;");
	// Create Arrow Result using ingest connection to avoid streaming conflicts.
	auto &input_data = db.QueryArrowForIngest("SELECT 42 as value");

	// Temporary ingestion ignores schema.
	db.CreateTable("my_table", input_data, "my_schema", true);

	// Note: QueryAndCheck uses a separate connection, but temporary tables are connection-specific.
	// Use Query instead to verify on the same connection.
	{
		auto res = db.Query("SELECT * FROM my_table");
		REQUIRE(!res->HasError());
		REQUIRE(res->GetValue(0, 0).ToString() == "42");
	}
	{
		auto res = db.Query("SELECT * FROM my_schema.my_table");
		REQUIRE(res->HasError());
	}
}

TEST_CASE("ADBC - Test ingestion - Target Catalog", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Attach a separate database to act as the target catalog.
	auto attached_path = TestCreatePath("adbc_ingest_target_catalog.db");
	db.Query("ATTACH '" + attached_path + "' AS my_catalog;");

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");

	// Ingest into catalog-qualified name.
	db.CreateTable("my_table", input_data, "", false, "my_catalog");

	// Catalog-only defaults schema to main; use 3-part name to avoid catalog/schema ambiguity.
	REQUIRE(db.QueryAndCheck("SELECT * FROM my_catalog.main.my_table"));
}

TEST_CASE("ADBC - Test ingestion - Target Catalog and Schema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	auto attached_path = TestCreatePath("adbc_ingest_target_catalog_schema.db");
	db.Query("ATTACH '" + attached_path + "' AS my_catalog;");
	db.Query("CREATE SCHEMA my_catalog.my_schema;");

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");

	// Ingest into catalog.schema.table
	db.CreateTable("my_table", input_data, "my_schema", false, "my_catalog");

	REQUIRE(db.QueryAndCheck("SELECT * FROM my_catalog.my_schema.my_table"));
}

TEST_CASE("ADBC - Test ingestion - Temporary Table - Catalog Set", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create Arrow Result using ingest connection to avoid streaming conflicts.
	auto &input_data = db.QueryArrowForIngest("SELECT 42 as value");

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Case 1: set catalog then temporary
	{
		AdbcStatement stmt;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_CATALOG, "my_catalog", &adbc_error)));
		// Enabling temporary ingestion clears the catalog.
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_ENABLED, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input_data, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &adbc_error)));
	}
	// Note: QueryAndCheck uses a separate connection, but temporary tables are connection-specific.
	{
		auto res = db.Query("SELECT * FROM my_table");
		REQUIRE(!res->HasError());
		REQUIRE(res->GetValue(0, 0).ToString() == "42");
	}
	// Release input stream (BindStream may transfer ownership on success).
	if (input_data.release) {
		input_data.release(&input_data);
	}
	input_data.release = nullptr;

	// Case 2: set temporary then catalog
	{
		auto &input_data_2 = db.QueryArrowForIngest("SELECT 42 as value");
		AdbcStatement stmt;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_ENABLED, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table_2", &adbc_error)));
		// Setting a catalog while temporary is enabled is invalid; error is expected at execution time.
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_CATALOG, "my_catalog", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input_data_2, &adbc_error)));
		REQUIRE(!SUCCESS(AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &adbc_error)));
		REQUIRE((std::strcmp(adbc_error.message, "Temporary option is not supported with catalog") == 0));
		adbc_error.release(&adbc_error);
		InitializeADBCError(&adbc_error);
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &adbc_error)));
		// Execution failed, so the driver should not have consumed the input stream.
		if (input_data_2.release) {
			input_data_2.release(&input_data_2);
		}
		input_data_2.release = nullptr;
	}
}

TEST_CASE("ADBC - Test ingestion - Temporary Table - Schema After Temporary", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	db.Query("CREATE SCHEMA my_schema;");
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Case 1: set schema then temporary
	{
		auto &input_data = db.QueryArrowForIngest("SELECT 42 as value");
		AdbcStatement stmt;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "my_schema", &adbc_error)));
		// Enabling temporary ingestion clears the schema.
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_ENABLED, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input_data, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &adbc_error)));
		// Note: QueryAndCheck uses a separate connection, but temporary tables are connection-specific.
		{
			auto res = db.Query("SELECT * FROM my_table");
			REQUIRE(!res->HasError());
			REQUIRE(res->GetValue(0, 0).ToString() == "42");
		}
		{
			auto res = db.Query("SELECT * FROM my_schema.my_table");
			REQUIRE(res->HasError());
		}
		// Release input stream (BindStream may transfer ownership on success).
		if (input_data.release) {
			input_data.release(&input_data);
		}
		input_data.release = nullptr;
	}

	// Case 2: set temporary then schema
	{
		auto &input_data = db.QueryArrowForIngest("SELECT 42 as value");
		AdbcStatement stmt;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_ENABLED, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table", &adbc_error)));
		// Setting a schema while temporary is enabled should fail at execution time.
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "my_schema", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input_data, &adbc_error)));
		REQUIRE(!SUCCESS(AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &adbc_error)));
		REQUIRE((std::strcmp(adbc_error.message, "Temporary option is not supported with schema") == 0));
		adbc_error.release(&adbc_error);
		InitializeADBCError(&adbc_error);
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &adbc_error)));
		// Execution failed, so the driver should not have consumed the input stream.
		if (input_data.release) {
			input_data.release(&input_data);
		}
		input_data.release = nullptr;
	}
}

TEST_CASE("ADBC - Test ingestion - Quoted Table and Schema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	db.Query("CREATE SCHEMA \"my_schema\";");
	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");

	// Create table with name requiring quoting
	db.CreateTable("my_table", input_data, "my_schema");

	// Validate that we can get its schema
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	ArrowSchema arrow_schema;
	REQUIRE(SUCCESS(AdbcConnectionGetTableSchema(&db.adbc_connection, nullptr, "my_schema", "my_table", &arrow_schema,
	                                             &adbc_error)));
	REQUIRE((arrow_schema.n_children == 1));
	arrow_schema.release(&arrow_schema);
}

TEST_CASE("ADBC - Test Ingestion - Funky identifiers", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	std::vector<std::string> column_names = {"column.with.dots", "column\"with\"quotes", "column with whitespace"};
	std::string table_name = "my.table\"is 😵";
	std::string schema_name = "my schema\"is.😬";
	// Create a schema and allocate space for a single child
	ArrowSchema arrow_schema;
	duckdb_nanoarrow::ArrowSchemaInit(&arrow_schema, duckdb_nanoarrow::ArrowType::NANOARROW_TYPE_STRUCT);
	duckdb_nanoarrow::ArrowSchemaAllocateChildren(&arrow_schema, static_cast<int64_t>(column_names.size()));

	// Create the child schemas and build the select query to get test data
	std::string query = "SELECT ";
	for (size_t i = 0; i < column_names.size(); i++) {
		duckdb_nanoarrow::ArrowSchemaInit(arrow_schema.children[i], duckdb_nanoarrow::ArrowType::NANOARROW_TYPE_INT32);
		duckdb_nanoarrow::ArrowSchemaSetName(arrow_schema.children[i], column_names.at(i).c_str());
		if (i > 0) {
			query += ",";
		}
		query += std::to_string(i);
	}

	// Create input data
	auto &input_data = db.QueryArrow(query);
	ArrowArray prepared_array;
	input_data.get_next(&input_data, &prepared_array);

	// Create the schema
	db.Query("CREATE SCHEMA " + KeywordHelper::WriteOptionallyQuoted(schema_name));

	// Create ADBC statement that will create a table called "test"
	AdbcStatement adbc_stmt;
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_stmt, &adbc_error)));
	REQUIRE(SUCCESS(
	    AdbcStatementSetOption(&adbc_stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_CREATE, &adbc_error)));
	REQUIRE(SUCCESS(
	    AdbcStatementSetOption(&adbc_stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, schema_name.c_str(), &adbc_error)));
	REQUIRE(
	    SUCCESS(AdbcStatementSetOption(&adbc_stmt, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementBind(&adbc_stmt, &prepared_array, &arrow_schema, &adbc_error)));

	// Now execute: this should create the table and put the test data in
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_stmt, nullptr, nullptr, &adbc_error)));

	// Release everything
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_stmt, &adbc_error)));
	if (arrow_schema.release) {
		arrow_schema.release(&arrow_schema);
	}
	if (adbc_error.release) {
		adbc_error.release(&adbc_error);
	}
	if (input_data.release) {
		input_data.release(&input_data);
	}
	if (prepared_array.release) {
		prepared_array.release(&prepared_array);
	}

	// Check we can query
	auto schema_table =
	    KeywordHelper::WriteOptionallyQuoted(schema_name) + "." + KeywordHelper::WriteOptionallyQuoted(table_name);
	auto res = db.Query("select * from " + schema_table);
	for (size_t i = 0; i < column_names.size(); i++) {
		REQUIRE((res->ColumnName(i) == column_names.at(i)));
		REQUIRE((res->GetValue(i, 0) == i));
	}
}

TEST_CASE("ADBC - Test ingestion - Lineitem", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT * FROM read_csv_auto(\'data/csv/lineitem-carriage.csv\')");

	// Create Table 'my_table' from the Arrow Result
	db.CreateTable("lineitem", input_data);

	REQUIRE(db.QueryAndCheck("SELECT l_partkey, l_comment FROM lineitem WHERE l_orderkey=1 ORDER BY l_linenumber"));
}

TEST_CASE("ADBC - Pivot", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	auto &input_data = db.QueryArrow("SELECT * FROM read_csv_auto(\'data/csv/flights.csv\')");

	db.CreateTable("flights", input_data);

	REQUIRE(db.QueryAndCheck("PIVOT flights ON UniqueCarrier USING COUNT(1) GROUP BY OriginCityName;"));
}

TEST_CASE("Test Null Error/Database", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	AdbcDatabase adbc_database;
	// NULL error
	auto adbc_status = DatabaseInit(&adbc_database, nullptr);
	REQUIRE((adbc_status == ADBC_STATUS_INVALID_ARGUMENT));
	// NULL database
	adbc_status = DatabaseInit(nullptr, &adbc_error);
	REQUIRE((adbc_status == ADBC_STATUS_INVALID_ARGUMENT));
	REQUIRE((std::strcmp(adbc_error.message, "ADBC Database has an invalid pointer") == 0));

	// We must Release the error (Malloc-ed string)
	adbc_error.release(&adbc_error);

	// Null Error and Database
	adbc_status = DatabaseInit(nullptr, nullptr);
	REQUIRE((adbc_status == ADBC_STATUS_INVALID_ARGUMENT));
}

TEST_CASE("Test Invalid Path", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	AdbcDatabase adbc_database;

	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", "/this/path/is/imaginary/hopefully/", &adbc_error)));

	REQUIRE(!SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	if (!adbc_error.message) {
		REQUIRE(false);
	} else {
		REQUIRE(std::strstr(adbc_error.message, "Cannot open file"));
	}
	adbc_error.release(&adbc_error);
	InitializeADBCError(&adbc_error);
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	adbc_error.release(&adbc_error);
}

TEST_CASE("ADBC - Statement reuse", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;
	AdbcStatement adbc_statement;
	ArrowArrayStream arrow_stream;

	int64_t rows_affected;

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	// Insert some data
	// Create Arrow Result
	ADBCTestDatabase db;
	auto &input_data = db.QueryArrow("SELECT 42 as value");
	string table_name = "my_table";

	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	REQUIRE(
	    SUCCESS(StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data, &adbc_error)));
	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));

	string query = "DELETE FROM my_table";
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	if (arrow_stream.release) {
		arrow_stream.release(&arrow_stream);
	}

	auto &input_data_2 = db.QueryArrow("SELECT 42 as value");

	StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &adbc_error);

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data_2, &adbc_error)));
	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	if (arrow_stream.release) {
		arrow_stream.release(&arrow_stream);
	}

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
}

TEST_CASE("Error Release", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;
	AdbcStatement adbc_statement;
	ArrowArrayStream arrow_stream;
	ArrowArray arrow_array;

	int64_t rows_affected;

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	// Run Query so we can start trying to mess around with it
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT 42", &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	// Let's release the stream
	arrow_stream.release(&arrow_stream);

	// Per Arrow C Data Interface spec, after release all function pointers are set to NULL
	REQUIRE(!arrow_stream.release);
	REQUIRE(!arrow_stream.get_next);
	REQUIRE(!arrow_stream.get_schema);
	REQUIRE(!arrow_stream.get_last_error);
	REQUIRE(!arrow_stream.private_data);

	// Release ADBC Statement
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

	// Not possible to get Arrow stream with released statement
	REQUIRE((AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error) ==
	         ADBC_STATUS_INVALID_STATE));

	// We can release a statement and consume the stream afterward if we have called GetStream beforehand
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT 42", &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

	auto arrow_status = arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE((arrow_array.length == 1));
	REQUIRE((arrow_status == 0));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// We can't run a query without a query
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	AdbcStatementSetSqlQuery(&adbc_statement, nullptr, &adbc_error);
	REQUIRE((std::strcmp(adbc_error.message, "Missing query") == 0));

	// Release the connection
	REQUIRE(SUCCESS(ConnectionRelease(&adbc_connection, &adbc_error)));
	// Release the error
	adbc_error.release(&adbc_error);

	// We can't run a query after releasing a connection
	REQUIRE(!SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "Invalid connection object") == 0));
	// Release the error
	adbc_error.release(&adbc_error);

	// We can release it multiple times
	REQUIRE(SUCCESS(ConnectionRelease(&adbc_connection, &adbc_error)));

	// We can't Init with a released connection
	REQUIRE(!SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "Must call AdbcConnectionNew first") == 0));
	// Release the error
	adbc_error.release(&adbc_error);

	// Shut down the database
	REQUIRE(SUCCESS(DatabaseRelease(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));

	REQUIRE(!SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "Invalid database") == 0));

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	// Release the error
	adbc_error.release(&adbc_error);
}

TEST_CASE("Test Not-Implemented Partition Functions", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	auto status = AdbcConnectionReadPartition(&adbc_connection, nullptr, 0, nullptr, &adbc_error);
	REQUIRE((status == ADBC_STATUS_NOT_IMPLEMENTED));
	REQUIRE((std::strcmp(adbc_error.message, "Read Partitions are not supported in DuckDB") == 0));
	adbc_error.release(&adbc_error);

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	status = AdbcStatementExecutePartitions(&adbc_statement, nullptr, nullptr, nullptr, &adbc_error);
	REQUIRE((status == ADBC_STATUS_NOT_IMPLEMENTED));
	REQUIRE((std::strcmp(adbc_error.message, "Execute Partitions are not supported in DuckDB") == 0));
	adbc_error.release(&adbc_error);
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
}

// Reads entry `idx` of a utf8 ArrowArray (buffers: validity, offsets, data).
static string GetArrowString(ArrowArray *array, idx_t idx) {
	auto offsets = static_cast<const int32_t *>(array->buffers[1]);
	auto data = static_cast<const char *>(array->buffers[2]);
	return string(data + offsets[idx], static_cast<size_t>(offsets[idx + 1] - offsets[idx]));
}

TEST_CASE("Test ADBC ConnectionGetInfo", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	AdbcStatusCode status = ADBC_STATUS_OK;
	ArrowArrayStream out_stream;
	out_stream.release = nullptr;

	// ==== UNHAPPY PATH ====

	static uint32_t test_info_codes[] = {ADBC_INFO_DRIVER_NAME, ADBC_INFO_DRIVER_ARROW_VERSION,
	                                     ADBC_INFO_DRIVER_VERSION};
	static constexpr size_t TEST_INFO_CODE_LENGTH = sizeof(test_info_codes) / sizeof(uint32_t);

	// Invalid connection. With private_driver == nullptr this never reaches
	// duckdb_adbc::ConnectionGetInfo: the driver manager itself (see
	// test/api/adbc/driver_manager.cpp AdbcConnectionGetInfo) rejects the call
	// before dispatching to the driver.
	AdbcConnection bogus_connection;
	bogus_connection.private_data = nullptr;
	bogus_connection.private_driver = nullptr;
	out_stream.release = nullptr;
	status = AdbcConnectionGetInfo(&bogus_connection, test_info_codes, TEST_INFO_CODE_LENGTH, &out_stream, &adbc_error);
	REQUIRE(status == ADBC_STATUS_INVALID_STATE);
	REQUIRE(out_stream.release == nullptr);

	// No stream
	status = AdbcConnectionGetInfo(&adbc_connection, test_info_codes, TEST_INFO_CODE_LENGTH, nullptr, &adbc_error);
	REQUIRE(status == ADBC_STATUS_INVALID_ARGUMENT);

	// Connection allocated but never initialized (AdbcConnectionNew without a
	// matching AdbcConnectionInit) must fail, not silently succeed.
	{
		AdbcConnection uninitialized_connection;
		REQUIRE(SUCCESS(AdbcConnectionNew(&uninitialized_connection, &adbc_error)));
		out_stream.release = nullptr;
		status = AdbcConnectionGetInfo(&uninitialized_connection, test_info_codes, TEST_INFO_CODE_LENGTH, &out_stream,
		                               &adbc_error);
		REQUIRE(status == ADBC_STATUS_INVALID_STATE);
		REQUIRE(out_stream.release == nullptr);
		REQUIRE(SUCCESS(AdbcConnectionRelease(&uninitialized_connection, &adbc_error)));
	}

	// ==== HAPPY PATH ====
	// The info_value column is a dense union, which DuckDB's arrow_scan
	// cannot consume; the buffers are validated directly instead.

	// error is an optional out parameter per adbc.h; a null error must not
	// cause the call to fail.
	out_stream.release = nullptr;
	status = AdbcConnectionGetInfo(&adbc_connection, test_info_codes, TEST_INFO_CODE_LENGTH, &out_stream, nullptr);
	REQUIRE(status == ADBC_STATUS_OK);
	REQUIRE(out_stream.release != nullptr);
	out_stream.release(&out_stream);

	// This returns all known info codes.
	out_stream.release = nullptr;
	status = AdbcConnectionGetInfo(&adbc_connection, nullptr, 42, &out_stream, &adbc_error);
	REQUIRE((status == ADBC_STATUS_OK));
	REQUIRE((out_stream.release != nullptr));
	{
		ArrowSchema schema;
		schema.release = nullptr;
		REQUIRE(out_stream.get_schema(&out_stream, &schema) == 0);
		REQUIRE(string(schema.format) == "+s");
		REQUIRE(schema.n_children == 2);
		REQUIRE(string(schema.children[0]->name) == "info_name");
		REQUIRE(string(schema.children[0]->format) == "I");
		REQUIRE((schema.children[0]->flags & ARROW_FLAG_NULLABLE) == 0);

		auto info_value_schema = schema.children[1];
		REQUIRE(string(info_value_schema->name) == "info_value");
		// The value column must be a *dense* union per the ADBC spec, with
		// all six members defined by adbc.h regardless of which ones DuckDB
		// actually populates.
		REQUIRE(string(info_value_schema->format) == "+ud:0,1,2,3,4,5");
		// adbc.h:1380 does not mark `info_value` "not null" (unlike e.g.
		// `statistic_value`, adbc.h:1662), so it must stay nullable.
		REQUIRE((info_value_schema->flags & ARROW_FLAG_NULLABLE) != 0);
		REQUIRE(info_value_schema->n_children == 6);
		REQUIRE(string(info_value_schema->children[0]->name) == "string_value");
		REQUIRE(string(info_value_schema->children[1]->name) == "bool_value");
		REQUIRE(string(info_value_schema->children[2]->name) == "int64_value");
		REQUIRE(string(info_value_schema->children[3]->name) == "int32_bitmask");
		REQUIRE(string(info_value_schema->children[4]->name) == "string_list");
		REQUIRE(string(info_value_schema->children[5]->name) == "int32_to_int32_list_map");
		schema.release(&schema);

		ArrowArray batch;
		batch.release = nullptr;
		REQUIRE(out_stream.get_next(&out_stream, &batch) == 0);
		REQUIRE(batch.release);

		auto names = static_cast<const uint32_t *>(batch.children[0]->buffers[1]);
		auto value_array = batch.children[1];
		// Dense unions have exactly two buffers (type ids + offsets) and no
		// validity buffer of their own.
		REQUIRE(value_array->n_buffers == 2);
		REQUIRE(value_array->n_children == 6);
		auto type_ids = static_cast<const int8_t *>(value_array->buffers[0]);
		auto value_offsets = static_cast<const int32_t *>(value_array->buffers[1]);
		auto int64_values = static_cast<const int64_t *>(value_array->children[2]->buffers[1]);

		// The four members DuckDB never populates must still be structurally
		// well-formed empty children, not just zero-length placeholders: each
		// child's buffer count must match its Arrow layout, and the nested
		// list/map children must themselves be well-formed empty arrays.

		// bool_value: fixed-width primitive -> validity + data buffers.
		auto bool_value = value_array->children[1];
		REQUIRE(bool_value->length == 0);
		REQUIRE(bool_value->null_count == 0);
		REQUIRE(bool_value->n_buffers == 2);
		REQUIRE(bool_value->n_children == 0);

		// int32_bitmask: fixed-width primitive -> validity + data buffers.
		auto int32_bitmask = value_array->children[3];
		REQUIRE(int32_bitmask->length == 0);
		REQUIRE(int32_bitmask->null_count == 0);
		REQUIRE(int32_bitmask->n_buffers == 2);
		REQUIRE(int32_bitmask->n_children == 0);

		// string_list: list<utf8> -> validity + offsets buffers, one utf8 child.
		// A list array's offsets buffer must always have (length + 1) entries,
		// even when length is 0, so a single zeroed offset is required.
		auto string_list = value_array->children[4];
		REQUIRE(string_list->length == 0);
		REQUIRE(string_list->null_count == 0);
		REQUIRE(string_list->n_buffers == 2);
		REQUIRE(string_list->n_children == 1);
		auto string_list_offsets = static_cast<const int32_t *>(string_list->buffers[1]);
		REQUIRE(string_list_offsets[0] == 0);
		auto string_list_item = string_list->children[0];
		REQUIRE(string_list_item->length == 0);
		REQUIRE(string_list_item->null_count == 0);
		// utf8 -> validity + offsets + data buffers.
		REQUIRE(string_list_item->n_buffers == 3);
		REQUIRE(string_list_item->n_children == 0);

		// int32_to_int32_list_map: map<int32, list<int32>>, encoded per the
		// Arrow map layout as list<struct<key: int32, value: list<int32>>>.
		auto map_value = value_array->children[5];
		REQUIRE(map_value->length == 0);
		REQUIRE(map_value->null_count == 0);
		REQUIRE(map_value->n_buffers == 2); // validity + offsets, like any list.
		REQUIRE(map_value->n_children == 1);
		auto map_offsets = static_cast<const int32_t *>(map_value->buffers[1]);
		REQUIRE(map_offsets[0] == 0);
		auto map_entries = map_value->children[0];
		REQUIRE(map_entries->length == 0);
		REQUIRE(map_entries->null_count == 0);
		REQUIRE(map_entries->n_buffers == 1); // struct -> validity only, no data.
		REQUIRE(map_entries->n_children == 2);
		auto map_key = map_entries->children[0];
		REQUIRE(map_key->length == 0);
		REQUIRE(map_key->null_count == 0);
		REQUIRE(map_key->n_buffers == 2); // int32 -> validity + data.
		REQUIRE(map_key->n_children == 0);
		auto map_entry_value = map_entries->children[1];
		REQUIRE(map_entry_value->length == 0);
		REQUIRE(map_entry_value->null_count == 0);
		REQUIRE(map_entry_value->n_buffers == 2); // list<int32> -> validity + offsets.
		REQUIRE(map_entry_value->n_children == 1);
		auto map_value_offsets = static_cast<const int32_t *>(map_entry_value->buffers[1]);
		REQUIRE(map_value_offsets[0] == 0);
		auto map_value_item = map_entry_value->children[0];
		REQUIRE(map_value_item->length == 0);
		REQUIRE(map_value_item->null_count == 0);
		REQUIRE(map_value_item->n_buffers == 2); // int32 -> validity + data.
		REQUIRE(map_value_item->n_children == 0);

		bool found_adbc_version = false;
		bool found_vendor_name = false;
		// Each dense union member has its own independent offsets sequence;
		// verify both the string_value and int64_value tracks are 0,1,2,...
		// within their own child array as rows of that type_id are seen.
		idx_t expected_string_offset = 0;
		idx_t expected_int64_offset = 0;
		for (idx_t row = 0; row < static_cast<idx_t>(batch.length); row++) {
			switch (type_ids[row]) {
			case 0:
				REQUIRE(static_cast<idx_t>(value_offsets[row]) == expected_string_offset);
				if (names[row] == ADBC_INFO_VENDOR_NAME) {
					found_vendor_name = true;
					REQUIRE(GetArrowString(value_array->children[0], value_offsets[row]) == "duckdb");
				}
				expected_string_offset++;
				break;
			case 2:
				REQUIRE(static_cast<idx_t>(value_offsets[row]) == expected_int64_offset);
				if (names[row] == ADBC_INFO_DRIVER_ADBC_VERSION) {
					found_adbc_version = true;
					REQUIRE(int64_values[value_offsets[row]] == ADBC_VERSION_1_1_0);
				}
				expected_int64_offset++;
				break;
			default:
				FAIL("Unexpected GetInfo union type id");
			}
		}
		REQUIRE(found_adbc_version);
		REQUIRE(found_vendor_name);
		batch.release(&batch);
	}
	out_stream.release(&out_stream);

	{
		// Requesting only unrecognized info codes must yield an empty batch,
		// not an error and not NULL rows.
		static uint32_t unknown_code[] = {4242};
		ArrowArrayStream unknown_stream;
		unknown_stream.release = nullptr;
		status = AdbcConnectionGetInfo(&adbc_connection, unknown_code, 1, &unknown_stream, &adbc_error);
		REQUIRE((status == ADBC_STATUS_OK));
		REQUIRE((unknown_stream.release != nullptr));

		ArrowSchema schema;
		schema.release = nullptr;
		REQUIRE(unknown_stream.get_schema(&unknown_stream, &schema) == 0);
		schema.release(&schema);

		ArrowArray batch;
		batch.release = nullptr;
		REQUIRE(unknown_stream.get_next(&unknown_stream, &batch) == 0);
		REQUIRE(batch.release);
		REQUIRE(batch.length == 0);
		batch.release(&batch);
		unknown_stream.release(&unknown_stream);
	}

	{
		// Validate ADBC_INFO_DRIVER_ADBC_VERSION is returned as int64 (dense
		// union type id 2).
		static uint32_t version_code[] = {ADBC_INFO_DRIVER_ADBC_VERSION};
		ArrowArrayStream version_stream;
		version_stream.release = nullptr;
		status = AdbcConnectionGetInfo(&adbc_connection, version_code, 1, &version_stream, &adbc_error);
		REQUIRE((status == ADBC_STATUS_OK));
		REQUIRE((version_stream.release != nullptr));

		ArrowSchema schema;
		schema.release = nullptr;
		REQUIRE(version_stream.get_schema(&version_stream, &schema) == 0);
		schema.release(&schema);

		ArrowArray batch;
		batch.release = nullptr;
		REQUIRE(version_stream.get_next(&version_stream, &batch) == 0);
		REQUIRE(batch.release);
		REQUIRE(batch.length == 1);

		auto names = static_cast<const uint32_t *>(batch.children[0]->buffers[1]);
		REQUIRE(names[0] == ADBC_INFO_DRIVER_ADBC_VERSION);

		auto value_array = batch.children[1];
		auto type_ids = static_cast<const int8_t *>(value_array->buffers[0]);
		auto value_offsets = static_cast<const int32_t *>(value_array->buffers[1]);
		REQUIRE(type_ids[0] == 2);
		auto int64_values = static_cast<const int64_t *>(value_array->children[2]->buffers[1]);
		REQUIRE(int64_values[value_offsets[0]] == ADBC_VERSION_1_1_0);

		batch.release(&batch);
		version_stream.release(&version_stream);
	}

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	adbc_error.release(&adbc_error);
}

TEST_CASE("Test ADBC ConnectionGetStatisticNames", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db("test_statistic_names");

	ArrowArrayStream stream;
	stream.release = nullptr;

	// ==== UNHAPPY PATH ====
	AdbcConnection bogus_connection;
	bogus_connection.private_data = nullptr;
	bogus_connection.private_driver = nullptr;
	REQUIRE(AdbcConnectionGetStatisticNames(&bogus_connection, &stream, &db.adbc_error) != ADBC_STATUS_OK);
	REQUIRE(AdbcConnectionGetStatisticNames(&db.adbc_connection, nullptr, &db.adbc_error) != ADBC_STATUS_OK);
	if (db.adbc_error.release) {
		db.adbc_error.release(&db.adbc_error);
	}

	// ==== HAPPY PATH ====
	// DuckDB has no driver-specific statistics, so the result is always empty.
	REQUIRE(SUCCESS(AdbcConnectionGetStatisticNames(&db.adbc_connection, &stream, &db.adbc_error)));
	REQUIRE(stream.release);

	ArrowSchema schema;
	schema.release = nullptr;
	REQUIRE(stream.get_schema(&stream, &schema) == 0);
	REQUIRE(string(schema.format) == "+s");
	REQUIRE(schema.n_children == 2);
	REQUIRE(string(schema.children[0]->name) == "statistic_name");
	REQUIRE(string(schema.children[0]->format) == "u");
	REQUIRE((schema.children[0]->flags & ARROW_FLAG_NULLABLE) == 0);
	REQUIRE(string(schema.children[1]->name) == "statistic_key");
	REQUIRE(string(schema.children[1]->format) == "s");
	REQUIRE((schema.children[1]->flags & ARROW_FLAG_NULLABLE) == 0);
	schema.release(&schema);

	ArrowArray batch;
	batch.release = nullptr;
	REQUIRE(stream.get_next(&stream, &batch) == 0);
	REQUIRE(batch.release);
	REQUIRE(batch.length == 0);
	REQUIRE(batch.n_children == 2);
	batch.release(&batch);
	stream.release(&stream);
}

TEST_CASE("Test ADBC ConnectionGetStatistics", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db("test_get_statistics");

	auto res = db.Query("CREATE TABLE stats_table_one (i INTEGER)");
	REQUIRE(!res->HasError());
	res = db.Query("INSERT INTO stats_table_one SELECT * FROM range(3)");
	REQUIRE(!res->HasError());
	res = db.Query("CREATE TABLE stats_table_two (i INTEGER)");
	REQUIRE(!res->HasError());
	res = db.Query("INSERT INTO stats_table_two SELECT * FROM range(5)");
	REQUIRE(!res->HasError());
	// Views have no statistics and must not appear in the result.
	res = db.Query("CREATE VIEW stats_view_one AS SELECT * FROM stats_table_one");
	REQUIRE(!res->HasError());

	ArrowArrayStream stream;
	stream.release = nullptr;

	// ==== UNHAPPY PATH ====
	AdbcConnection bogus_connection;
	bogus_connection.private_data = nullptr;
	bogus_connection.private_driver = nullptr;
	REQUIRE(AdbcConnectionGetStatistics(&bogus_connection, nullptr, nullptr, nullptr, 1, &stream, &db.adbc_error) !=
	        ADBC_STATUS_OK);
	REQUIRE(AdbcConnectionGetStatistics(&db.adbc_connection, nullptr, nullptr, nullptr, 1, nullptr, &db.adbc_error) !=
	        ADBC_STATUS_OK);
	if (db.adbc_error.release) {
		db.adbc_error.release(&db.adbc_error);
	}

	// ==== HAPPY PATH ====
	// The statistic_value column is a dense union, which DuckDB's arrow_scan
	// cannot consume; the buffers are validated directly instead.
	REQUIRE(SUCCESS(AdbcConnectionGetStatistics(&db.adbc_connection, nullptr, nullptr, "stats_table_%", 1, &stream,
	                                            &db.adbc_error)));
	REQUIRE(stream.release);

	ArrowSchema schema;
	schema.release = nullptr;
	REQUIRE(stream.get_schema(&stream, &schema) == 0);
	REQUIRE(string(schema.format) == "+s");
	REQUIRE(schema.n_children == 2);
	REQUIRE(string(schema.children[0]->name) == "catalog_name");
	REQUIRE(string(schema.children[1]->name) == "catalog_db_schemas");
	REQUIRE((schema.children[1]->flags & ARROW_FLAG_NULLABLE) == 0);
	auto db_schema_schema = schema.children[1]->children[0];
	REQUIRE(string(db_schema_schema->children[0]->name) == "db_schema_name");
	REQUIRE(string(db_schema_schema->children[1]->name) == "db_schema_statistics");
	auto statistics_schema = db_schema_schema->children[1]->children[0];
	REQUIRE(statistics_schema->n_children == 5);
	REQUIRE(string(statistics_schema->children[0]->name) == "table_name");
	REQUIRE((statistics_schema->children[0]->flags & ARROW_FLAG_NULLABLE) == 0);
	REQUIRE(string(statistics_schema->children[1]->name) == "column_name");
	REQUIRE(string(statistics_schema->children[2]->name) == "statistic_key");
	REQUIRE(string(statistics_schema->children[3]->name) == "statistic_value");
	// The value column must be a *dense* union per the ADBC spec.
	REQUIRE(string(statistics_schema->children[3]->format) == "+ud:0,1,2,3");
	REQUIRE(statistics_schema->children[3]->n_children == 4);
	REQUIRE(string(statistics_schema->children[3]->children[1]->name) == "uint64");
	REQUIRE(string(statistics_schema->children[3]->children[2]->name) == "float64");
	REQUIRE(string(statistics_schema->children[4]->name) == "statistic_is_approximate");
	schema.release(&schema);

	ArrowArray batch;
	batch.release = nullptr;
	REQUIRE(stream.get_next(&stream, &batch) == 0);
	REQUIRE(batch.release);

	// One catalog, named after the database file.
	REQUIRE(batch.length == 1);
	REQUIRE(GetArrowString(batch.children[0], 0) == "test_get_statistics");

	// One schema in the catalog: "main".
	auto schemas_list = batch.children[1];
	auto schema_offsets = static_cast<const int32_t *>(schemas_list->buffers[1]);
	REQUIRE(schema_offsets[0] == 0);
	REQUIRE(schema_offsets[1] == 1);
	auto schema_struct = schemas_list->children[0];
	REQUIRE(schema_struct->length == 1);
	REQUIRE(GetArrowString(schema_struct->children[0], 0) == "main");

	// Two statistics (one ROW_COUNT per table), view excluded.
	auto stats_list = schema_struct->children[1];
	auto stats_offsets = static_cast<const int32_t *>(stats_list->buffers[1]);
	REQUIRE(stats_offsets[0] == 0);
	REQUIRE(stats_offsets[1] == 2);
	auto stats_struct = stats_list->children[0];
	REQUIRE(stats_struct->length == 2);
	REQUIRE(GetArrowString(stats_struct->children[0], 0) == "stats_table_one");
	REQUIRE(GetArrowString(stats_struct->children[0], 1) == "stats_table_two");

	// column_name is NULL (table-level statistics).
	auto column_name_array = stats_struct->children[1];
	REQUIRE(column_name_array->null_count == 2);
	auto column_name_validity = static_cast<const uint8_t *>(column_name_array->buffers[0]);
	REQUIRE((column_name_validity[0] & 0x3) == 0);

	auto keys = static_cast<const int16_t *>(stats_struct->children[2]->buffers[1]);
	REQUIRE(keys[0] == ADBC_STATISTIC_ROW_COUNT_KEY);
	REQUIRE(keys[1] == ADBC_STATISTIC_ROW_COUNT_KEY);

	// Approximate row counts are stored in the float64 union member (type id 2)
	// per the ADBC spec for ROW_COUNT.
	auto value_array = stats_struct->children[3];
	auto type_ids = static_cast<const int8_t *>(value_array->buffers[0]);
	auto value_offsets = static_cast<const int32_t *>(value_array->buffers[1]);
	REQUIRE(type_ids[0] == 2);
	REQUIRE(type_ids[1] == 2);
	REQUIRE(value_offsets[0] == 0);
	REQUIRE(value_offsets[1] == 1);
	auto row_counts = static_cast<const double *>(value_array->children[2]->buffers[1]);
	REQUIRE(row_counts[0] == 3.0);
	REQUIRE(row_counts[1] == 5.0);

	// Estimates are always flagged as approximate.
	auto approximate_bits = static_cast<const uint8_t *>(stats_struct->children[4]->buffers[1]);
	REQUIRE((approximate_bits[0] & 0x3) == 0x3);

	batch.release(&batch);
	stream.release(&stream);

	// ==== FILTERING ====
	// An exact table name matches a single table.
	stream.release = nullptr;
	REQUIRE(SUCCESS(AdbcConnectionGetStatistics(&db.adbc_connection, nullptr, "main", "stats_table_one", 0, &stream,
	                                            &db.adbc_error)));
	REQUIRE(stream.get_next(&stream, &batch) == 0);
	REQUIRE(batch.length == 1);
	{
		auto stats = batch.children[1]->children[0]->children[1]->children[0];
		REQUIRE(stats->length == 1);
		REQUIRE(GetArrowString(stats->children[0], 0) == "stats_table_one");
	}
	batch.release(&batch);
	stream.release(&stream);

	// A pattern with no matches produces an empty result.
	stream.release = nullptr;
	REQUIRE(SUCCESS(AdbcConnectionGetStatistics(&db.adbc_connection, nullptr, nullptr, "no_such_table_%", 1, &stream,
	                                            &db.adbc_error)));
	REQUIRE(stream.get_next(&stream, &batch) == 0);
	REQUIRE(batch.length == 0);
	batch.release(&batch);
	stream.release(&stream);
}

TEST_CASE("Test ADBC Statement Bind (unhappy)", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	string query = "select ?, ?, ?";

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

	ADBCTestDatabase db;
	auto &input_data = db.QueryArrow("SELECT 42, true, 'this is a string'");
	ArrowArray prepared_array;
	ArrowSchema prepared_schema;
	input_data.get_next(&input_data, &prepared_array);
	input_data.get_schema(&input_data, &prepared_schema);

	AdbcStatusCode status = ADBC_STATUS_OK;
	// No error passed in
	// This is not an error, this only means we can't provide a message
	status = AdbcStatementBind(&adbc_statement, &prepared_array, &prepared_schema, nullptr);
	REQUIRE((status == ADBC_STATUS_OK));

	// No valid statement
	AdbcStatement bogus_statement;
	bogus_statement.private_data = nullptr;
	bogus_statement.private_driver = nullptr;
	status = AdbcStatementBind(&bogus_statement, &prepared_array, &prepared_schema, &adbc_error);
	REQUIRE((status != ADBC_STATUS_OK));

	// No array
	status = AdbcStatementBind(&adbc_statement, nullptr, &prepared_schema, &adbc_error);
	REQUIRE((status != ADBC_STATUS_OK));

	// No schema
	status = AdbcStatementBind(&adbc_statement, &prepared_array, nullptr, &adbc_error);
	REQUIRE((status != ADBC_STATUS_OK));

	// ---- Get Parameter Schema ----

	// No error passed in
	ArrowSchema result;
	result.release = nullptr;
	status = AdbcStatementGetParameterSchema(&adbc_statement, &result, nullptr);
	result.release(&result);
	result.release = nullptr;
	REQUIRE((status == ADBC_STATUS_OK));

	// No valid statement
	status = AdbcStatementGetParameterSchema(&bogus_statement, &result, &adbc_error);
	REQUIRE((status != ADBC_STATUS_OK));

	// No result
	status = AdbcStatementGetParameterSchema(&adbc_statement, nullptr, &adbc_error);
	REQUIRE((status != ADBC_STATUS_OK));

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

	adbc_error.release(&adbc_error);
}

TEST_CASE("Test ADBC Statement Bind", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	ADBCTestDatabase db;

	// Create prepared parameter array
	auto &input_data = db.QueryArrow("SELECT 42, true, 'this is a string'");
	string query = "select ?, ?, ?";

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

	ArrowSchema expected_schema;
	expected_schema.release = nullptr;
	REQUIRE(SUCCESS(AdbcStatementGetParameterSchema(&adbc_statement, &expected_schema, &adbc_error)));
	REQUIRE((expected_schema.n_children == 3));
	for (int64_t i = 0; i < expected_schema.n_children; i++) {
		auto child = expected_schema.children[i];
		std::string child_name = child->name;
		std::string expected_name = StringUtil::Format("%d", i);
		REQUIRE((child_name == expected_name));
	}
	expected_schema.release(&expected_schema);

	ArrowArray prepared_array;
	ArrowSchema prepared_schema;
	input_data.get_next(&input_data, &prepared_array);
	input_data.get_schema(&input_data, &prepared_schema);
	REQUIRE(SUCCESS(AdbcStatementBind(&adbc_statement, &prepared_array, &prepared_schema, &adbc_error)));
	REQUIRE((prepared_array.release == nullptr));
	REQUIRE((prepared_schema.release == nullptr));

	int64_t rows_affected;
	ArrowArrayStream arrow_stream;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	input_data.release(&input_data);

	ArrowArray result_array;
	arrow_stream.get_next(&arrow_stream, &result_array);
	REQUIRE((reinterpret_cast<const int32_t *>(result_array.children[0]->buffers[1])[0] == 42));

	result_array.release(&result_array);
	arrow_stream.release(&arrow_stream);
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	adbc_error.release(&adbc_error);
}

TEST_CASE("Test ADBC Statement re-execute after bound parameters are consumed", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	ADBCTestDatabase db;
	auto &input_data = db.QueryArrow("SELECT 8::BIGINT");

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT CAST(? AS BIGINT)", &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

	ArrowArray prepared_array;
	ArrowSchema prepared_schema;
	input_data.get_next(&input_data, &prepared_array);
	input_data.get_schema(&input_data, &prepared_schema);
	REQUIRE(SUCCESS(AdbcStatementBind(&adbc_statement, &prepared_array, &prepared_schema, &adbc_error)));
	input_data.release(&input_data);

	// The first execution consumes the bound batch
	ArrowArrayStream arrow_stream;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, nullptr, &adbc_error)));
	ArrowArray result_array;
	REQUIRE(arrow_stream.get_next(&arrow_stream, &result_array) == 0);
	REQUIRE((reinterpret_cast<const int64_t *>(result_array.children[0]->buffers[1])[0] == 8));
	result_array.release(&result_array);
	arrow_stream.release(&arrow_stream);

	// Executing again without a fresh bind must fail cleanly instead of handing out a stream over an empty result
	ArrowArrayStream second_stream;
	second_stream.release = nullptr;
	auto status = AdbcStatementExecuteQuery(&adbc_statement, &second_stream, nullptr, &adbc_error);
	REQUIRE((status == ADBC_STATUS_INVALID_ARGUMENT));
	REQUIRE((second_stream.release == nullptr));
	REQUIRE((string(adbc_error.message).find("non-empty chunk") != string::npos));
	adbc_error.release(&adbc_error);
	InitializeADBCError(&adbc_error);

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	if (adbc_error.release) {
		adbc_error.release(&adbc_error);
	}
}

TEST_CASE("Test ADBC Statement with Zero Parameters", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Create connection
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	// Test 1: Query with no parameters (CREATE TABLE)
	{
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetSqlQuery(&adbc_statement, "CREATE TABLE test_zero_params (id INTEGER)", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

		// Get parameter schema - should have 0 children
		ArrowSchema param_schema;
		param_schema.release = nullptr;
		REQUIRE(SUCCESS(AdbcStatementGetParameterSchema(&adbc_statement, &param_schema, &adbc_error)));
		REQUIRE(param_schema.n_children == 0);
		REQUIRE(param_schema.format != nullptr);
		REQUIRE(strcmp(param_schema.format, "+s") == 0); // Should be a struct
		param_schema.release(&param_schema);

		// Execute the statement without binding any parameters
		ArrowArrayStream arrow_stream;
		arrow_stream.release = nullptr;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, nullptr, &adbc_error)));
		arrow_stream.release(&arrow_stream);

		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	// Test 2: Query with no parameters (SELECT constant)
	{
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT 42", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

		// Get parameter schema - should have 0 children
		ArrowSchema param_schema;
		param_schema.release = nullptr;
		REQUIRE(SUCCESS(AdbcStatementGetParameterSchema(&adbc_statement, &param_schema, &adbc_error)));
		REQUIRE(param_schema.n_children == 0);
		param_schema.release(&param_schema);

		// Execute and verify result
		ArrowArrayStream arrow_stream;
		arrow_stream.release = nullptr;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, nullptr, &adbc_error)));

		ArrowSchema result_schema;
		arrow_stream.get_schema(&arrow_stream, &result_schema);
		REQUIRE(result_schema.n_children == 1); // One column in result
		result_schema.release(&result_schema);

		arrow_stream.release(&arrow_stream);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	adbc_error.release(&adbc_error);
}

TEST_CASE("Test ADBC Transactions", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	ADBCTestDatabase db;

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");
	string table_name = "test";
	string query = "select count(*) from test";

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcConnection adbc_connection_2;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	ArrowArrayStream arrow_stream;
	ArrowArray arrow_array;

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection_2, &adbc_database, &adbc_error)));

	// Let's first insert with Auto-Commit On
	AdbcStatement adbc_statement;

	AdbcStatement adbc_statement_2;

	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	REQUIRE(
	    SUCCESS(StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data, &adbc_error)));

	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_2, &adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement_2, query.c_str(), &adbc_error)));
	int64_t rows_affected;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement_2, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 1)));
	// Release the boys
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// Now lets insert with Auto-Commit Off
	REQUIRE(SUCCESS(AdbcConnectionSetOption(&adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
	                                        ADBC_OPTION_VALUE_DISABLED, &adbc_error)));
	auto &input_data_2 = db.QueryArrow("SELECT 42;");
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	REQUIRE(
	    SUCCESS(StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));

	REQUIRE(SUCCESS(
	    StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &adbc_error)));

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data_2, &adbc_error)));

	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_2, &adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement_2, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement_2, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 1)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// If we check from con1, we should have 2
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 2)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// Now if we do a commit on the first connection this should be 2 on the second connection
	REQUIRE(SUCCESS(AdbcConnectionCommit(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_2, &adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement_2, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement_2, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 2)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// Lets do a rollback
	auto &input_data_3 = db.QueryArrow("SELECT 42;");
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	REQUIRE(
	    SUCCESS(StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));

	REQUIRE(SUCCESS(
	    StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &adbc_error)));

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data_3, &adbc_error)));

	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));

	// If we check from con1, we should have 3
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 3)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// If we check from con2 we should 2
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_2, &adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement_2, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement_2, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 2)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// If we rollback con1, we should now have two again on con1
	REQUIRE(SUCCESS(AdbcConnectionRollback(&adbc_connection, &adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 2)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	// Let's change the Auto commit config mid-transaction
	auto &input_data_4 = db.QueryArrow("SELECT 42;");
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	REQUIRE(
	    SUCCESS(StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(
	    StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &adbc_error)));

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data_4, &adbc_error)));

	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionSetOption(&adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
	                                        ADBC_OPTION_VALUE_ENABLED, &adbc_error)));

	// Now Both con1 and con2 should have 3
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_2, &adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement_2, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement_2, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 3)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 3)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	auto &input_data_5 = db.QueryArrow("SELECT 42;");
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	REQUIRE(
	    SUCCESS(StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, table_name.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(
	    StatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &adbc_error)));

	REQUIRE(SUCCESS(StatementBindStream(&adbc_statement, &input_data_5, &adbc_error)));

	REQUIRE(SUCCESS(StatementExecuteQuery(&adbc_statement, nullptr, nullptr, &adbc_error)));

	// Auto-Commit is on, so this should just be commited
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection_2, &adbc_statement_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement_2, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement_2, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 4)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &arrow_stream, &rows_affected, &adbc_error)));

	arrow_stream.get_next(&arrow_stream, &arrow_array);
	REQUIRE(((reinterpret_cast<const int64_t *>(arrow_array.children[0]->buffers[1])[0] == 4)));
	arrow_array.release(&arrow_array);
	arrow_stream.release(&arrow_stream);

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement_2, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection_2, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
}

TEST_CASE("Test ADBC Transaction Errors", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	// Can't commit if there is no transaction
	REQUIRE(!SUCCESS(AdbcConnectionCommit(&adbc_connection, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "No active transaction, cannot commit") == 0));
	adbc_error.release(&adbc_error);

	// Can't rollback if there is no transaction
	REQUIRE(!SUCCESS(AdbcConnectionRollback(&adbc_connection, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "No active transaction, cannot rollback") == 0));
	adbc_error.release(&adbc_error);

	// Try to set Commit option to random gunk
	REQUIRE(SUCCESS(!AdbcConnectionSetOption(&adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, "bla", &adbc_error)));

	REQUIRE((std::strcmp(adbc_error.message, "Invalid connection option value adbc.connection.autocommit=bla") == 0));
	adbc_error.release(&adbc_error);

	// Let's disable the autocommit
	REQUIRE(SUCCESS(AdbcConnectionSetOption(&adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
	                                        ADBC_OPTION_VALUE_DISABLED, &adbc_error)));

	// We should succeed on committing and rolling back.
	REQUIRE(SUCCESS(AdbcConnectionCommit(&adbc_connection, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRollback(&adbc_connection, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
}

TEST_CASE("Test ADBC ConnectionGetTableSchema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	ArrowSchema arrow_schema;
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	// Test successful schema return
	REQUIRE(SUCCESS(
	    AdbcConnectionGetTableSchema(&adbc_connection, nullptr, "main", "duckdb_indexes", &arrow_schema, &adbc_error)));
	REQUIRE((arrow_schema.n_children == 14));
	arrow_schema.release(&arrow_schema);

	// Test Catalog Name
	REQUIRE(!SUCCESS(
	    AdbcConnectionGetTableSchema(&adbc_connection, "bla", "main", "duckdb_indexes", &arrow_schema, &adbc_error)));
	REQUIRE(StringUtil::Contains(adbc_error.message, "Catalog \"bla\" does not exist"));
	adbc_error.release(&adbc_error);

	REQUIRE(SUCCESS(AdbcConnectionGetTableSchema(&adbc_connection, "system", "main", "duckdb_indexes", &arrow_schema,
	                                             &adbc_error)));
	REQUIRE((arrow_schema.n_children == 14));
	arrow_schema.release(&arrow_schema);

	// Empty schema should be fine
	REQUIRE(SUCCESS(
	    AdbcConnectionGetTableSchema(&adbc_connection, nullptr, "", "duckdb_indexes", &arrow_schema, &adbc_error)));
	REQUIRE((arrow_schema.n_children == 14));
	arrow_schema.release(&arrow_schema);

	// Test null and empty table name
	REQUIRE(!SUCCESS(AdbcConnectionGetTableSchema(&adbc_connection, nullptr, "", nullptr, &arrow_schema, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "AdbcConnectionGetTableSchema: must provide table_name") == 0));
	adbc_error.release(&adbc_error);

	REQUIRE(!SUCCESS(AdbcConnectionGetTableSchema(&adbc_connection, nullptr, "", "", &arrow_schema, &adbc_error)));
	REQUIRE((std::strcmp(adbc_error.message, "AdbcConnectionGetTableSchema: must provide table_name") == 0));
	adbc_error.release(&adbc_error);

	// Test invalid schema

	REQUIRE(!SUCCESS(
	    AdbcConnectionGetTableSchema(&adbc_connection, nullptr, "b", "duckdb_indexes", &arrow_schema, &adbc_error)));
	REQUIRE((std::strstr(adbc_error.message, "Catalog Error") != nullptr));
	adbc_error.release(&adbc_error);

	// Test invalid table
	REQUIRE(!SUCCESS(
	    AdbcConnectionGetTableSchema(&adbc_connection, nullptr, "", "duckdb_indexeeees", &arrow_schema, &adbc_error)));
	REQUIRE((std::strstr(adbc_error.message, "Catalog Error") != nullptr));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	adbc_error.release(&adbc_error);
}

TEST_CASE("Test ADBC Prepared Statement - Prepare nop", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	AdbcStatement adbc_statement;

	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));

	// Statement Prepare is a nop for us, so it should just work, although it just does some error checking.
	REQUIRE(SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

	REQUIRE(!SUCCESS(AdbcStatementPrepare(&adbc_statement, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
}

TEST_CASE("Test AdbcConnectionGetTableTypes", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	ADBCTestDatabase db("AdbcConnectionGetTableTypes.db");

	// Create Arrow Result
	auto &input_data = db.QueryArrow("SELECT 42 as value");
	// Create Table 'my_table' from the Arrow Result
	db.CreateTable("my_table", input_data);
	ArrowArrayStream arrow_stream;
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	REQUIRE(SUCCESS(AdbcConnectionGetTableTypes(&db.adbc_connection, &arrow_stream, &adbc_error)));
	db.CreateTable("result", arrow_stream);

	auto res = db.Query("Select * from result");
	REQUIRE((res->ColumnCount() == 1));
	REQUIRE((res->GetValue(0, 0).ToString() == "BASE TABLE"));
	adbc_error.release(&adbc_error);
}

TEST_CASE("ADBC - Empty sql (unhappy)", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	string query;
	SECTION("Empty") {
		query = "";
	}
	SECTION("Whitespace") {
		query = " \n";
	}

	// Create connection - database and whatnot
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
	auto status = AdbcStatementSetSqlQuery(&adbc_statement, query.c_str(), &adbc_error);

	REQUIRE((status == ADBC_STATUS_INVALID_ARGUMENT));

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

	adbc_error.release(&adbc_error);
}

TEST_CASE("Test Segfault Option Set", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	AdbcDatabase adbc_database;
	AdbcConnection adbc_connection;

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));

	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionSetOption(&adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
	                                        ADBC_OPTION_VALUE_DISABLED, &adbc_error)));

	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	auto conn_wrapper = static_cast<DuckDBAdbcConnectionWrapper *>(adbc_connection.private_data);
	auto cconn = reinterpret_cast<Connection *>(conn_wrapper->connection);

	REQUIRE(!cconn->IsAutoCommit());

	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	adbc_error.release(&adbc_error);
}

TEST_CASE("Test AdbcConnectionGetObjects", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	// Let's first try what works
	// 1. Test ADBC_OBJECT_DEPTH_CATALOGS
	{
		ADBCTestDatabase db("test_catalog_depth");
		// Create Arrow Result
		auto &input_data = db.QueryArrow("SELECT 42 as value");
		// Create Table 'my_table' from the Arrow Result
		db.CreateTable("my_table", input_data);

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_CATALOGS, nullptr, nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(0, 0).ToString() == "system"));
		REQUIRE((res->GetValue(0, 1).ToString() == "temp"));
		REQUIRE((res->GetValue(0, 2).ToString() == "test_catalog_depth"));
		REQUIRE((res->GetValue(1, 0).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 1).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 2).ToString() == "[]"));
		db.Query("Drop table result;");

		// Test Filters
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_CATALOGS, "bla", nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 0));
		db.Query("Drop table result;");
	}
	// 2. Test ADBC_OBJECT_DEPTH_DB_SCHEMAS
	{
		ADBCTestDatabase db("ADBC_OBJECT_DEPTH_DB_SCHEMAS.db");
		// Create Arrow Result
		auto &input_data = db.QueryArrow("SELECT 42 as value");
		// Create Table 'my_table' from the Arrow Result
		db.CreateTable("my_table", input_data);

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;

		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_DB_SCHEMAS, nullptr, nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(0, 0).ToString() == "ADBC_OBJECT_DEPTH_DB_SCHEMAS"));
		REQUIRE((res->GetValue(0, 1).ToString() == "system"));
		REQUIRE((res->GetValue(0, 2).ToString() == "temp"));
		string expected = R"([
		    {
		        'db_schema_name': information_schema,
		        'db_schema_tables': []
		    },
		    {
		        'db_schema_name': main,
		        'db_schema_tables': []
		    },
		    {
		        'db_schema_name': pg_catalog,
		        'db_schema_tables': []
		    }
		])";
		REQUIRE(res->GetValue(1, 0).ToString() == "[{'db_schema_name': main, 'db_schema_tables': []}]");
		REQUIRE((StringUtil::Replace(res->GetValue(1, 1).ToString(), " ", "") ==
		         StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected, "\n", ""), "\t", ""), " ", "")));
		REQUIRE(res->GetValue(1, 2).ToString() == "[{'db_schema_name': main, 'db_schema_tables': []}]");
		db.Query("Drop table result;");

		// Test Filters
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_DB_SCHEMAS, "bla", nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 0));
		db.Query("Drop table result;");

		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_DB_SCHEMAS, nullptr, "bla",
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(1, 0).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 1).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 2).ToString() == "[]"));
		db.Query("Drop table result;");
	}
	// 3. Test ADBC_OBJECT_DEPTH_TABLES
	{
		ADBCTestDatabase db("test_table_depth");
		// Create Arrow Result
		auto &input_data = db.QueryArrow("SELECT 42 as value");
		// Create Table 'my_table' from the Arrow Result
		db.CreateTable("my_table", input_data);
		// Create View 'my_view'
		db.Query("Create view my_view as from my_table;");

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query(R"(
			SELECT
				catalog_name,
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_name: catalog_db_schema.db_schema_name,
							db_schema_tables: list_sort(catalog_db_schema.db_schema_tables),
						}
					)
				)
			FROM result
			ORDER BY catalog_name ASC
		)");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(0, 0).ToString() == "system"));
		REQUIRE((res->GetValue(0, 1).ToString() == "temp"));
		REQUIRE((res->GetValue(0, 2).ToString() == "test_table_depth"));
		string expected_result_1 = R"({
                'db_schema_name': information_schema,
                'db_schema_tables': []
            })";
		string expected_result_2 = R"({
                'db_schema_name': main,
                'db_schema_tables': []
            })";
		string expected_result_3 = R"({
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_table,
                        'table_type': BASE TABLE,
                        'table_columns': [],
                        'table_constraints': []
                    },
                    {
                        'table_name': my_view,
                        'table_type': VIEW,
                        'table_columns': [],
                        'table_constraints': []
                    }
                ]
            })";
		string expected_1_clean = StringUtil::Replace(
		    StringUtil::Replace(StringUtil::Replace(expected_result_1, "\n", ""), "\t", ""), " ", "");
		string expected_2_clean = StringUtil::Replace(
		    StringUtil::Replace(StringUtil::Replace(expected_result_2, "\n", ""), "\t", ""), " ", "");
		string expected_3_clean = StringUtil::Replace(
		    StringUtil::Replace(StringUtil::Replace(expected_result_3, "\n", ""), "\t", ""), " ", "");
		REQUIRE((StringUtil::Replace(res->GetValue(1, 0).ToString(), " ", "").find(expected_1_clean) != string::npos));
		REQUIRE((StringUtil::Replace(res->GetValue(1, 1).ToString(), " ", "").find(expected_2_clean) != string::npos));
		REQUIRE((StringUtil::Replace(res->GetValue(1, 2).ToString(), " ", "").find(expected_3_clean) != string::npos));
		db.Query("Drop table result;");

		// Test Filters

		// catalog
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, "bla", nullptr, nullptr,
		                                         nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 0));
		db.Query("Drop table result;");

		// db_schema
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, "bla", nullptr,
		                                         nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(1, 0).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 1).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 2).ToString() == "[]"));
		db.Query("Drop table result;");

		// table_name
		string select_catalog_db_schemas = R"(
			SELECT
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_name: catalog_db_schema.db_schema_name,
							db_schema_tables: list_sort(catalog_db_schema.db_schema_tables),
						}
					)
				) as catalog_db_schemas
			FROM result
			WHERE catalog_name == 'test_table_depth'
        )";
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr, "bla",
		                                         nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query(select_catalog_db_schemas);
		REQUIRE((res->ColumnCount() == 1));
		REQUIRE((res->RowCount() == 1));
		string expected = "[{'db_schema_name': main, 'db_schema_tables': []}]";
		REQUIRE((res->GetValue(0, 0).ToString() == expected));
		db.Query("Drop table result;");

		{
			// table_type: Empty table_type returns all tables and views
			std::vector<const char *> table_type = {nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			REQUIRE(
			    (StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected_3_clean) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: ["BASE TABLE", "VIEW"] returns all tables and views
			std::vector<const char *> table_type = {"BASE TABLE", "VIEW", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			REQUIRE(
			    (StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected_3_clean) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: BASE TABLE returns all tables
			std::vector<const char *> table_type = {"BASE TABLE", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			string expected_result = R"([{
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_table,
                        'table_type': BASE TABLE,
                        'table_columns': [],
                        'table_constraints': []
                    }
                ]
            }])";
			string expected_clean = StringUtil::Replace(
			    StringUtil::Replace(StringUtil::Replace(expected_result, "\n", ""), "\t", ""), " ", "");
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "") == expected_clean));
			db.Query("Drop table result;");
		}

		{
			// table_type: VIEW returns all views
			std::vector<const char *> table_type = {"VIEW", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			string expected_result = R"([{
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_view,
                        'table_type': VIEW,
                        'table_columns': [],
                        'table_constraints': []
                    }
                ]
            }])";
			string expected_clean = StringUtil::Replace(
			    StringUtil::Replace(StringUtil::Replace(expected_result, "\n", ""), "\t", ""), " ", "");
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "") == expected_clean));
			db.Query("Drop table result;");
		}
	}
	// 4. Test ADBC_OBJECT_DEPTH_COLUMNS
	{
		ADBCTestDatabase db("test_column_depth");
		// Create Arrow Result
		auto &input_data = db.QueryArrow("SELECT 42 as value");
		// Create Table 'my_table' from the Arrow Result
		db.CreateTable("my_table", input_data);
		// Create View 'my_view'
		db.Query("Create view my_view as from my_table;");

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query(R"(
			SELECT
				catalog_name,
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_name: catalog_db_schema.db_schema_name,
							db_schema_tables: list_sort(catalog_db_schema.db_schema_tables),
						}
					)
				)
			FROM result
			ORDER BY catalog_name ASC
		)");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(0, 0).ToString() == "system"));
		REQUIRE((res->GetValue(0, 1).ToString() == "temp"));
		REQUIRE((res->GetValue(0, 2).ToString() == "test_column_depth"));
		string expected_1 = R"(
            {
                'db_schema_name': information_schema,
                'db_schema_tables': []
            })";
		string expected_2 = R"(
            {
                'db_schema_name': main,
                'db_schema_tables': []
            })";
		string expected_3 = R"(
            {
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_table,
                        'table_type': BASE TABLE,
                        'table_columns': [
                            {
                                'column_name': value,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': []
                    },
                    {
                        'table_name': my_view,
                        'table_type': VIEW,
                        'table_columns': [
                            {
                                'column_name': value,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': []
                    }
                ]
            })";
		string expected[3];
		expected[0] =
		    StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected_1, "\n", ""), "\t", ""), " ", "");
		expected[1] =
		    StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected_2, "\n", ""), "\t", ""), " ", "");
		expected[2] =
		    StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected_3, "\n", ""), "\t", ""), " ", "");
		REQUIRE((StringUtil::Replace(res->GetValue(1, 0).ToString(), " ", "").find(expected[0]) != string::npos));
		REQUIRE((StringUtil::Replace(res->GetValue(1, 1).ToString(), " ", "").find(expected[1]) != string::npos));
		REQUIRE((StringUtil::Replace(res->GetValue(1, 2).ToString(), " ", "").find(expected[2]) != string::npos));
		db.Query("Drop table result;");

		// Test Filters

		// catalog
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, "bla", nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 0));
		db.Query("Drop table result;");

		// db_schema
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, "bla",
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(1, 0).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 1).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 2).ToString() == "[]"));
		db.Query("Drop table result;");

		// table_name
		string select_catalog_db_schemas = R"(
			SELECT
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_name: catalog_db_schema.db_schema_name,
							db_schema_tables: list_sort(catalog_db_schema.db_schema_tables),
						}
					)
				) as catalog_db_schemas
			FROM result
			WHERE catalog_name == 'test_column_depth'
        )";
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
		                                         "bla", nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query(select_catalog_db_schemas);
		REQUIRE((res->ColumnCount() == 1));
		REQUIRE((res->RowCount() == 1));
		REQUIRE((res->GetValue(0, 0).ToString() == "[{'db_schema_name': main, 'db_schema_tables': []}]"));
		db.Query("Drop table result;");

		// column_name
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
		                                         nullptr, nullptr, "bla", &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query(select_catalog_db_schemas);
		auto expected_value = "[{'db_schema_name': main, "
		                      "'db_schema_tables': [{'table_name': my_table, 'table_type': BASE TABLE, "
		                      "'table_columns': [], 'table_constraints': []}, "
		                      "{'table_name': my_view, 'table_type': VIEW, "
		                      "'table_columns': [], 'table_constraints': []}]}]";
		REQUIRE((res->GetValue(0, 0).ToString() == expected_value));
		db.Query("Drop table result;");

		{
			// table_type: Empty table_type returns all tables and views
			std::vector<const char *> table_type = {nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected[2]) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: ["BASE TABLE", "VIEW"] returns all tables and views
			std::vector<const char *> table_type = {"BASE TABLE", "VIEW", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected[2]) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: BASE TABLE returns all tables
			std::vector<const char *> table_type = {"BASE TABLE", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			string expected_result = R"({
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_table,
                        'table_type': BASE TABLE,
                        'table_columns': [
                            {
                                'column_name': value,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': []
                    }
                ]
            })";
			string expected_clean = StringUtil::Replace(
			    StringUtil::Replace(StringUtil::Replace(expected_result, "\n", ""), "\t", ""), " ", "");
			REQUIRE(
			    (StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected_clean) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: VIEW returns all views
			std::vector<const char *> table_type = {"VIEW", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			string expected_result = R"({
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_view,
                        'table_type': VIEW,
                        'table_columns': [
                            {
                                'column_name': value,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': []
                    }
                ]
            })";
			string expected_clean = StringUtil::Replace(
			    StringUtil::Replace(StringUtil::Replace(expected_result, "\n", ""), "\t", ""), " ", "");
			REQUIRE(
			    (StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected_clean) != string::npos));
			db.Query("Drop table result;");
		}
	}
	// 4-2. Test ADBC_OBJECT_DEPTH_COLUMNS: xdbc_* metadata for diverse column types
	// Covers: remarks (comment), column default, DECIMAL precision/scale, DATE/TIME/TIMESTAMP
	// datetime subtypes and column sizes, TIME_NS and TIME WITH TIME ZONE sql type codes,
	// VARCHAR/BLOB char_octet_length, and array sql type code.
	{
		ADBCTestDatabase db("type_meta_test");
		db.Query(R"(
			CREATE TABLE type_test (
				c_bool BOOLEAN,
				c_bigint BIGINT,
				c_decimal DECIMAL(10,3),
				c_varchar VARCHAR,
				c_blob BLOB,
				c_date DATE,
				c_time TIME,
				c_time_tz TIMETZ,
				c_time_ns TIME_NS,
				c_ts TIMESTAMP,
				c_ts_tz TIMESTAMPTZ,
				c_arr INTEGER[],
				c_with_default INTEGER DEFAULT 42
			)
		)");
		db.Query("COMMENT ON COLUMN type_test.c_bool IS 'my comment'");

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, "type_meta_test",
		                                         nullptr, "type_test", nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);

		// Flatten the nested GetObjects result to one row per column
		auto res = db.Query(R"(
			SELECT col.column_name, col.remarks, col.xdbc_type_name, col.xdbc_column_size,
			       col.xdbc_decimal_digits, col.xdbc_num_prec_radix, col.xdbc_sql_data_type,
			       col.xdbc_datetime_sub, col.xdbc_char_octet_length, col.xdbc_column_def
			FROM (
				SELECT UNNEST(
					list_filter(
						list_filter(catalog_db_schemas, lambda s: s.db_schema_name = 'main')[1].db_schema_tables,
						lambda t: t.table_name = 'type_test'
					)[1].table_columns
				) AS col
				FROM result
				WHERE catalog_name = 'type_meta_test'
			) sub
			ORDER BY col.ordinal_position
		)");

		// col indices: 0=column_name, 1=remarks, 2=xdbc_type_name, 3=xdbc_column_size,
		//              4=xdbc_decimal_digits, 5=xdbc_num_prec_radix, 6=xdbc_sql_data_type,
		//              7=xdbc_datetime_sub, 8=xdbc_char_octet_length, 9=xdbc_column_def
		// row indices: 0=c_bool,1=c_bigint,2=c_decimal,3=c_varchar,4=c_blob,
		//              5=c_date,6=c_time,7=c_time_tz,8=c_time_ns,9=c_ts,10=c_ts_tz,11=c_arr,12=c_with_default
		REQUIRE((res->RowCount() == 13));

		// remarks: only c_bool has a comment
		REQUIRE((res->GetValue(1, 0).ToString() == "my comment"));
		REQUIRE((res->GetValue(1, 1).IsNull()));

		// xdbc_column_def: only c_with_default has a default value
		REQUIRE((res->GetValue(9, 12).ToString() == "42"));
		REQUIRE((res->GetValue(9, 0).IsNull()));

		// xdbc_sql_data_type covers every branch in the CASE expression
		REQUIRE((res->GetValue(6, 0).ToString() == "16"));    // BOOLEAN → 16
		REQUIRE((res->GetValue(6, 1).ToString() == "-5"));    // BIGINT → -5
		REQUIRE((res->GetValue(6, 2).ToString() == "3"));     // DECIMAL → 3
		REQUIRE((res->GetValue(6, 3).ToString() == "12"));    // VARCHAR → 12
		REQUIRE((res->GetValue(6, 4).ToString() == "2004"));  // BLOB → 2004
		REQUIRE((res->GetValue(6, 5).ToString() == "91"));    // DATE → 91
		REQUIRE((res->GetValue(6, 6).ToString() == "92"));    // TIME → 92
		REQUIRE((res->GetValue(6, 7).ToString() == "2013"));  // TIME WITH TIME ZONE → 2013
		REQUIRE((res->GetValue(6, 8).ToString() == "92"));    // TIME_NS → 92
		REQUIRE((res->GetValue(6, 9).ToString() == "93"));    // TIMESTAMP → 93
		REQUIRE((res->GetValue(6, 10).ToString() == "2014")); // TIMESTAMP WITH TIME ZONE → 2014
		REQUIRE((res->GetValue(6, 11).ToString() == "2003")); // INTEGER[] → 2003
		REQUIRE((res->GetValue(6, 12).ToString() == "4"));    // INTEGER → 4

		// xdbc_column_size: DATE=10, TIME variants=15, TIMESTAMP variants=26
		REQUIRE((res->GetValue(3, 0).IsNull()));            // BOOLEAN: no numeric precision
		REQUIRE((res->GetValue(3, 1).ToString() == "64"));  // BIGINT: 64
		REQUIRE((res->GetValue(3, 2).ToString() == "10"));  // DECIMAL(10,3): precision
		REQUIRE((res->GetValue(3, 5).ToString() == "10"));  // DATE: hardcoded 10
		REQUIRE((res->GetValue(3, 6).ToString() == "15"));  // TIME: hardcoded 15
		REQUIRE((res->GetValue(3, 7).ToString() == "15"));  // TIME WITH TIME ZONE: 15
		REQUIRE((res->GetValue(3, 8).ToString() == "15"));  // TIME_NS: 15
		REQUIRE((res->GetValue(3, 9).ToString() == "26"));  // TIMESTAMP: hardcoded 26
		REQUIRE((res->GetValue(3, 10).ToString() == "26")); // TIMESTAMP WITH TIME ZONE: 26

		// xdbc_datetime_sub: DATE=1, TIME variants=2, TIMESTAMP variants=3
		REQUIRE((res->GetValue(7, 5).ToString() == "1"));  // DATE → 1
		REQUIRE((res->GetValue(7, 6).ToString() == "2"));  // TIME → 2
		REQUIRE((res->GetValue(7, 7).ToString() == "2"));  // TIME WITH TIME ZONE → 2
		REQUIRE((res->GetValue(7, 8).ToString() == "2"));  // TIME_NS → 2
		REQUIRE((res->GetValue(7, 9).ToString() == "3"));  // TIMESTAMP → 3
		REQUIRE((res->GetValue(7, 10).ToString() == "3")); // TIMESTAMP WITH TIME ZONE → 3
		REQUIRE((res->GetValue(7, 11).IsNull()));          // INTEGER[]: NULL

		// DECIMAL: scale and radix
		REQUIRE((res->GetValue(4, 2).ToString() == "3"));  // xdbc_decimal_digits = scale = 3
		REQUIRE((res->GetValue(5, 2).ToString() == "10")); // xdbc_num_prec_radix = 10

		// xdbc_char_octet_length: NULL for unbounded VARCHAR/BLOB
		REQUIRE((res->GetValue(8, 3).IsNull())); // VARCHAR (unbounded)
		REQUIRE((res->GetValue(8, 4).IsNull())); // BLOB (unbounded)
		REQUIRE((res->GetValue(8, 0).IsNull())); // BOOLEAN: not a char type

		db.Query("Drop table result;");
	}
	// 5. Test ADBC_OBJECT_DEPTH_ALL
	{
		ADBCTestDatabase db("test_all_depth");
		// Create Table 'my_table' with primary key
		db.Query("Create table my_table (a int, b int, primary key (a, b))");
		// Create View 'my_view'
		db.Query("Create view my_view as from my_table;");

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr, nullptr,
		                                         nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query(R"(
			SELECT
				catalog_name,
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_name: catalog_db_schema.db_schema_name,
							db_schema_tables: list_sort(catalog_db_schema.db_schema_tables),
						}
					)
				)
			FROM result
			ORDER BY catalog_name ASC
		)");
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(0, 0).ToString() == "system"));
		REQUIRE((res->GetValue(0, 1).ToString() == "temp"));
		REQUIRE((res->GetValue(0, 2).ToString() == "test_all_depth"));
		string expected_1 = R"(
            {
                'db_schema_name': information_schema,
                'db_schema_tables': []
            })";
		string expected_2 = R"(
            {
                'db_schema_name': main,
                'db_schema_tables': []
            })";
		string expected_3 = R"(
            {
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_table,
                        'table_type': BASE TABLE,
                        'table_columns': [
                            {
                                'column_name': a,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 0,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': NO,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            },
                            {
                                'column_name': b,
                                'ordinal_position': 2,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 0,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': NO,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': [
                            {
                                'constraint_name': my_table_a_b_pkey,
                                'constraint_type': PRIMARY KEY,
                                'constraint_column_names': [a, b],
                                'constraint_column_usage': []
                            }
                        ]
                    },
                    {
                        'table_name': my_view,
                        'table_type': VIEW,
                        'table_columns': [
                            {
                                'column_name': a,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            },
                            {
                                'column_name': b,
                                'ordinal_position': 2,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': []
                    }
                ]
            })";
		string expected_1_clean =
		    StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected_1, "\n", ""), "\t", ""), " ", "");
		string expected_2_clean =
		    StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected_2, "\n", ""), "\t", ""), " ", "");
		string expected_3_clean =
		    StringUtil::Replace(StringUtil::Replace(StringUtil::Replace(expected_3, "\n", ""), "\t", ""), " ", "");
		REQUIRE((StringUtil::Replace(res->GetValue(1, 0).ToString(), " ", "").find(expected_1_clean) != string::npos));
		REQUIRE((StringUtil::Replace(res->GetValue(1, 1).ToString(), " ", "").find(expected_2_clean) != string::npos));
		REQUIRE((StringUtil::Replace(res->GetValue(1, 2).ToString(), " ", "").find(expected_3_clean) != string::npos));
		db.Query("Drop table result;");

		// Test Filters

		// catalog
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, "bla", nullptr,
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 0));
		db.Query("Drop table result;");

		// db_schema
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, "bla",
		                                         nullptr, nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query("Select * from result order by catalog_name asc");
		REQUIRE((res->ColumnCount() == 2));
		REQUIRE((res->RowCount() == 3));
		REQUIRE((res->GetValue(1, 0).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 1).ToString() == "[]"));
		REQUIRE((res->GetValue(1, 2).ToString() == "[]"));
		db.Query("Drop table result;");

		// table_name
		string select_catalog_db_schemas = R"(
			SELECT
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_name: catalog_db_schema.db_schema_name,
							db_schema_tables: list_sort(catalog_db_schema.db_schema_tables),
						}
					)
				) as catalog_db_schemas
			FROM result
			WHERE catalog_name == 'test_all_depth'
        )";
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
		                                         "bla", nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query(select_catalog_db_schemas);
		REQUIRE((res->RowCount() == 1));
		string expected = "[{'db_schema_name': main, 'db_schema_tables': []}]";
		REQUIRE((res->GetValue(0, 0).ToString() == expected));
		db.Query("Drop table result;");

		// column_name
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
		                                         nullptr, nullptr, "bla", &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		res = db.Query(select_catalog_db_schemas);
		REQUIRE((res->RowCount() == 1));
		// clang-format off
		expected = "["
			"{"
				"'db_schema_name': main, "
				"'db_schema_tables': ["
					"{"
						"'table_name': my_table, "
						"'table_type': BASE TABLE, "
						"'table_columns': [], "
						"'table_constraints': []"
					"}, "
					"{"
						"'table_name': my_view, "
						"'table_type': VIEW, "
						"'table_columns': [], "
						"'table_constraints': []"
					"}"
				"]"
			"}"
		"]";
		// clang-format on
		REQUIRE((res->GetValue(0, 0).ToString() == expected));
		db.Query("Drop table result;");

		{
			// table_type: Empty table_type returns all tables and views
			std::vector<const char *> table_type = {nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected[2]) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: ["BASE TABLE", "VIEW"] returns all tables and views
			std::vector<const char *> table_type = {"BASE TABLE", "VIEW", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "").find(expected[2]) != string::npos));
			db.Query("Drop table result;");
		}

		{
			// table_type: BASE TABLE returns all tables
			std::vector<const char *> table_type = {"BASE TABLE", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			string expected_result = R"([{
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_table,
                        'table_type': BASE TABLE,
                        'table_columns': [
                            {
                                'column_name': a,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 0,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': NO,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            },
                            {
                                'column_name': b,
                                'ordinal_position': 2,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 0,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': NO,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': [
                            {
                                'constraint_name': my_table_a_b_pkey,
                                'constraint_type': PRIMARY KEY,
                                'constraint_column_names': [a, b],
                                'constraint_column_usage': []
                            }
                        ]
                    }
                ]
            }])";
			string expected_clean = StringUtil::Replace(
			    StringUtil::Replace(StringUtil::Replace(expected_result, "\n", ""), "\t", ""), " ", "");
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "") == expected_clean));
			db.Query("Drop table result;");
		}

		{
			// table_type: VIEW returns all views
			std::vector<const char *> table_type = {"VIEW", nullptr};
			REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
			                                         nullptr, table_type.data(), nullptr, &arrow_stream, &adbc_error)));
			db.CreateTable("result", arrow_stream);
			res = db.Query(select_catalog_db_schemas);
			REQUIRE((res->ColumnCount() == 1));
			REQUIRE((res->RowCount() == 1));
			string expected_result = R"([{
                'db_schema_name': main,
                'db_schema_tables': [
                    {
                        'table_name': my_view,
                        'table_type': VIEW,
                        'table_columns': [
                            {
                                'column_name': a,
                                'ordinal_position': 1,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            },
                            {
                                'column_name': b,
                                'ordinal_position': 2,
                                'remarks': NULL,
                                'xdbc_data_type': NULL,
                                'xdbc_type_name': INTEGER,
                                'xdbc_column_size': 32,
                                'xdbc_decimal_digits': 0,
                                'xdbc_num_prec_radix': 2,
                                'xdbc_nullable': 1,
                                'xdbc_column_def': NULL,
                                'xdbc_sql_data_type': 4,
                                'xdbc_datetime_sub': NULL,
                                'xdbc_char_octet_length': NULL,
                                'xdbc_is_nullable': YES,
                                'xdbc_scope_catalog': NULL,
                                'xdbc_scope_schema': NULL,
                                'xdbc_scope_table': NULL,
                                'xdbc_is_autoincrement': NULL,
                                'xdbc_is_generatedcolumn': NULL
                            }
                        ],
                        'table_constraints': []
                    }
                ]
            }])";
			string expected_clean = StringUtil::Replace(
			    StringUtil::Replace(StringUtil::Replace(expected_result, "\n", ""), "\t", ""), " ", "");
			REQUIRE((StringUtil::Replace(res->GetValue(0, 0).ToString(), " ", "") == expected_clean));
			db.Query("Drop table result;");
		}
	}
	// 6. Test constraints
	//
	// Available constraints: https://duckdb.org/docs/stable/sql/constraints.html
	{
		ADBCTestDatabase db("test_constraints");
		// Create table 'foreign_table'
		db.Query("CREATE TABLE foreign_table (id INTEGER PRIMARY KEY)");
		// Create table 'my_table' with constraints
		db.Query(R"(
			CREATE TABLE my_table (
				primary_key INTEGER PRIMARY KEY,
				not_null INTEGER NOT NULL,
				custom_check INTEGER CHECK (custom_check > 0),
				unique_single INTEGER UNIQUE,
				unique_multi1 INTEGER,
				unique_multi2 INTEGER,
				UNIQUE (unique_multi2, unique_multi1),
				foreign_id INTEGER REFERENCES foreign_table(id)
			)
		)");

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_COLUMNS, nullptr, nullptr,
		                                         "my_table", nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		string select_catalog_db_schemas = R"(
			SELECT
				list_sort(
					list_transform(
						catalog_db_schemas,
						lambda catalog_db_schema: {
							db_schema_tables: list_sort(
								list_transform(
									catalog_db_schema.db_schema_tables,
									lambda db_schema_table: {
										table_name: db_schema_table.table_name,
										table_constraints: list_sort(db_schema_table.table_constraints)
									}
								)
							)
						}
					)
				) as catalog_db_schemas
			FROM result
			WHERE catalog_name == 'test_constraints'
		)";
		auto res = db.Query(select_catalog_db_schemas);
		REQUIRE((res->RowCount() == 1));
		// clang-format off
		std::string expected = "["
			"{"
				"'db_schema_tables': ["
					"{"
						"'table_name': my_table, "
						"'table_constraints': [{"
							"'constraint_name': my_table_custom_check_check, "
							"'constraint_type': CHECK, "
							"'constraint_column_names': [custom_check], "
							"'constraint_column_usage': []"
						"}, {"
							"'constraint_name': my_table_foreign_id_id_fkey, "
							"'constraint_type': FOREIGN KEY, "
							"'constraint_column_names': [foreign_id], "
							"'constraint_column_usage': [{"
								"'fk_catalog': test_constraints, "
								"'fk_db_schema': main, "
								"'fk_table': foreign_table, "
								"'fk_column_name': id"
							"}]"
						"}, {"
							"'constraint_name': my_table_primary_key_pkey, "
							"'constraint_type': PRIMARY KEY, "
							"'constraint_column_names': [primary_key], "
							"'constraint_column_usage': []"
						"}, {"
							"'constraint_name': my_table_unique_multi2_unique_multi1_key, "
							"'constraint_type': UNIQUE, "
							"'constraint_column_names': [unique_multi2, unique_multi1], "
							"'constraint_column_usage': []"
						"}, {"
							"'constraint_name': my_table_unique_single_key, "
							"'constraint_type': UNIQUE, "
							"'constraint_column_names': [unique_single], "
							"'constraint_column_usage': []"
						"}]"
					"}"
				"]"
			"}"
		"]";
		// clang-format on
		REQUIRE((res->GetValue(0, 0).ToString() == expected));
		db.Query("DROP TABLE result;");
	}
	// Now lets test some errors
	{
		ADBCTestDatabase db("test_errors");
		// Create Arrow Result
		auto &input_data = db.QueryArrow("SELECT 42 as value");
		// Create Table 'my_table' from the Arrow Result
		db.CreateTable("my_table", input_data);

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream = {};

		REQUIRE(!SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, 42, nullptr, nullptr, nullptr, nullptr, nullptr,
		                                          &arrow_stream, &adbc_error)));
		REQUIRE((std::strcmp(adbc_error.message, "Invalid value of Depth") == 0));
		adbc_error.release(&adbc_error);
		if (arrow_stream.release) {
			arrow_stream.release(&arrow_stream);
		}

		// Invalid table type
		std::vector<const char *> table_type = {"INVALID", nullptr};
		REQUIRE(!SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_ALL, nullptr, nullptr, nullptr,
		                                          table_type.data(), nullptr, &arrow_stream, &adbc_error)));
		REQUIRE((strcmp(adbc_error.message,
		                "Table type must be \"LOCAL TABLE\", \"BASE TABLE\" or \"VIEW\": \"INVALID\"") == 0));
		adbc_error.release(&adbc_error);
		if (arrow_stream.release) {
			arrow_stream.release(&arrow_stream);
		}
	}

	// Test input quoting protection
	{
		ADBCTestDatabase db("test_input_quoting");
		db.CreateTable("test_table", db.QueryArrow("SELECT 42 as value"));

		AdbcError adbc_error = {};
		InitializeADBCError(&adbc_error);
		ArrowArrayStream arrow_stream;

		// Test catalog filter with special characters
		auto status =
		    AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_CATALOGS, "'; DROP TABLE test_table; --",
		                             nullptr, nullptr, nullptr, nullptr, &arrow_stream, &adbc_error);
		REQUIRE(status == ADBC_STATUS_OK);
		// Verify table still exists after special characters in filter
		auto res = db.Query("SELECT * FROM test_table");
		REQUIRE(res->RowCount() == 1);
		arrow_stream.release(&arrow_stream);

		// Test schema filter with special characters
		status = AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_DB_SCHEMAS, nullptr,
		                                  "'; DROP TABLE test_table; --", nullptr, nullptr, nullptr, &arrow_stream,
		                                  &adbc_error);
		REQUIRE(status == ADBC_STATUS_OK);
		// Verify table still exists
		res = db.Query("SELECT * FROM test_table");
		REQUIRE(res->RowCount() == 1);
		arrow_stream.release(&arrow_stream);

		// Test table name filter with special characters
		status = AdbcConnectionGetObjects(&db.adbc_connection, ADBC_OBJECT_DEPTH_TABLES, nullptr, nullptr,
		                                  "'; DROP TABLE test_table; --", nullptr, nullptr, &arrow_stream, &adbc_error);
		REQUIRE(status == ADBC_STATUS_OK);
		// Verify table still exists
		res = db.Query("SELECT * FROM test_table");
		REQUIRE(res->RowCount() == 1);
		arrow_stream.release(&arrow_stream);
	}
}

TEST_CASE("Test AdbcConnectionGetObjects - empty list not NULL", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	// ADBC spec: list-typed fields must be empty lists (not NULL) when the depth
	// includes that level but filters exclude all entries.

	ADBCTestDatabase db("test_empty_list");
	db.CreateTable("my_table", db.QueryArrow("SELECT 42 as value"));

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	ArrowArrayStream arrow_stream;

	// catalog_db_schemas must be [] (not NULL) when schema filter excludes all schemas
	for (auto depth : {ADBC_OBJECT_DEPTH_DB_SCHEMAS, ADBC_OBJECT_DEPTH_TABLES, ADBC_OBJECT_DEPTH_COLUMNS}) {
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, depth, nullptr, "nonexistent_schema", nullptr,
		                                         nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query("SELECT catalog_db_schemas FROM result ORDER BY catalog_name");
		REQUIRE(res->RowCount() > 0);
		for (idx_t i = 0; i < res->RowCount(); i++) {
			REQUIRE(res->GetValue(0, i).ToString() == "[]");
		}
		db.Query("Drop table result;");
	}

	// db_schema_tables must be [] (not NULL) when table name filter excludes all tables
	for (auto depth : {ADBC_OBJECT_DEPTH_TABLES, ADBC_OBJECT_DEPTH_COLUMNS}) {
		REQUIRE(SUCCESS(AdbcConnectionGetObjects(&db.adbc_connection, depth, "test_empty_list", nullptr,
		                                         "nonexistent_table", nullptr, nullptr, &arrow_stream, &adbc_error)));
		db.CreateTable("result", arrow_stream);
		auto res = db.Query(R"(
			SELECT list_transform(catalog_db_schemas, s -> s.db_schema_tables) AS tables
			FROM result WHERE catalog_name = 'test_empty_list'
		)");
		REQUIRE(res->RowCount() == 1);
		REQUIRE(res->GetValue(0, 0).ToString().find("NULL") == string::npos);
		db.Query("Drop table result;");
	}
}

TEST_CASE("Test ADBC 1.1.0 Ingestion Modes", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Test CREATE mode (default)
	{
		auto &input_data = db.QueryArrow("SELECT 42 as value");
		db.CreateTable("test_table", input_data);
		auto result = db.Query("SELECT * FROM test_table");
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 42);
	}

	// Test CREATE mode error (table already exists)
	{
		auto &input_data = db.QueryArrow("SELECT 43 as value");
		AdbcStatement adbc_statement;
		// Use ingest connection to avoid streaming conflict with QueryArrow
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_table", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_CREATE,
		                                       &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		int64_t rows_affected;
		auto status = AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error);
		REQUIRE(status == ADBC_STATUS_ALREADY_EXISTS);
		// Release the error set by the failed query
		adbc_error.release(&adbc_error);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	// Test APPEND mode
	{
		auto &input_data = db.QueryArrow("SELECT 43 as value");
		AdbcStatement adbc_statement;
		// Use ingest connection to avoid streaming conflict with QueryArrow
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_table", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND,
		                                       &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 1);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

		auto result = db.Query("SELECT * FROM test_table ORDER BY value");
		REQUIRE(result->RowCount() == 2);
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 42);
		REQUIRE(result->GetValue(0, 1).GetValue<int32_t>() == 43);
	}

	// Test REPLACE mode
	{
		auto &input_data = db.QueryArrow("SELECT 44 as value");
		AdbcStatement adbc_statement;
		// Use ingest connection to avoid streaming conflict with QueryArrow
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_table", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE,
		                                       ADBC_INGEST_OPTION_MODE_REPLACE, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 1);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

		auto result = db.Query("SELECT * FROM test_table");
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 44);
	}

	// Test CREATE_APPEND mode (table exists)
	{
		auto &input_data = db.QueryArrow("SELECT 45 as value");
		AdbcStatement adbc_statement;
		// Use ingest connection to avoid streaming conflict with QueryArrow
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_table", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE,
		                                       ADBC_INGEST_OPTION_MODE_CREATE_APPEND, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 1);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

		auto result = db.Query("SELECT * FROM test_table ORDER BY value");
		REQUIRE(result->RowCount() == 2);
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 44);
		REQUIRE(result->GetValue(0, 1).GetValue<int32_t>() == 45);
	}

	// Test CREATE_APPEND mode (table does not exist)
	{
		auto &input_data = db.QueryArrow("SELECT 46 as value");
		AdbcStatement adbc_statement;
		// Use ingest connection to avoid streaming conflict with QueryArrow
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_table2", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE,
		                                       ADBC_INGEST_OPTION_MODE_CREATE_APPEND, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 1);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));

		auto result = db.Query("SELECT * FROM test_table2");
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 46);
	}
}

TEST_CASE("Test ADBC 1.1.0 rows_affected", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Create a test table
	db.Query("CREATE TABLE test_rows (value INTEGER)");
	db.Query("INSERT INTO test_rows VALUES (1), (2), (3)");

	// Test rows_affected for INSERT
	{
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(
		    SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "INSERT INTO test_rows VALUES (4), (5)", &adbc_error)));
		int64_t rows_affected = -999;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 2);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	// Test rows_affected for UPDATE
	{
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetSqlQuery(&adbc_statement, "UPDATE test_rows SET value = 10 WHERE value < 3", &adbc_error)));
		int64_t rows_affected = -999;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 2);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	// Test rows_affected for DELETE
	{
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(
		    SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "DELETE FROM test_rows WHERE value = 10", &adbc_error)));
		int64_t rows_affected = -999;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 2);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	// Test rows_affected for SELECT (should return -1)
	{
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT * FROM test_rows", &adbc_error)));
		int64_t rows_affected = -999;
		ArrowArrayStream stream;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &stream, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == -1);
		stream.release(&stream);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}

	// Test rows_affected for ingestion
	{
		auto &input_data = db.QueryArrow("SELECT 100 as value UNION ALL SELECT 101 UNION ALL SELECT 102");
		AdbcStatement adbc_statement;
		// Use ingest connection to avoid streaming conflict with QueryArrow
		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_ingest", &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &adbc_error)));
		int64_t rows_affected = -999;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &adbc_error)));
		REQUIRE(rows_affected == 3);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
	}
}

TEST_CASE("Test ADBC URI option", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);
	auto file_exists = [](const char *path) {
		std::ifstream f(path);
		return f.good();
	};

	// Test URI with :memory:
	{
		AdbcDatabase adbc_database;
		AdbcConnection adbc_connection;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", ":memory:", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

		// Execute a simple query
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT 42 as value", &adbc_error)));
		ArrowArrayStream stream;
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &stream, &rows_affected, &adbc_error)));
		stream.release(&stream);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	}

	// Test URI with plain path
	{
		AdbcDatabase adbc_database;
		AdbcConnection adbc_connection;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", "test_uri_plain.db", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

		// Execute a simple query
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT 43 as value", &adbc_error)));
		ArrowArrayStream stream;
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &stream, &rows_affected, &adbc_error)));
		stream.release(&stream);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	}

	// Test file://<relative> is accepted
	{
		const char *expected_path = "test_uri_file.db";
		std::remove(expected_path);

		AdbcDatabase adbc_database;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", "file://test_uri_file.db", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

		REQUIRE(file_exists(expected_path));
		std::remove(expected_path);
	}

	// Test duckdb://<relative> is accepted
	{
		const char *expected_path = "test_duckdb_uri_file.db";
		std::remove(expected_path);

		AdbcDatabase adbc_database;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", "duckdb://test_duckdb_uri_file.db", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

		REQUIRE(file_exists(expected_path));
		std::remove(expected_path);
	}

	// Test URI overrides path (uri takes precedence)
	{
		AdbcDatabase adbc_database;
		AdbcConnection adbc_connection;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", "/invalid/path/db.duckdb", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", ":memory:", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

		// Should succeed because uri overrides the invalid path
		AdbcStatement adbc_statement;
		REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&adbc_statement, "SELECT 45 as value", &adbc_error)));
		ArrowArrayStream stream;
		int64_t rows_affected;
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&adbc_statement, &stream, &rows_affected, &adbc_error)));
		stream.release(&stream);
		REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &adbc_error)));
		REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	}

	// Test file: URI query is stripped
	{
		const char *expected_path = "test_uri_query.db";
		const char *unexpected_path = "test_uri_query.db?mode=ro";
		std::remove(expected_path);
		std::remove(unexpected_path);

		AdbcDatabase adbc_database;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", "file:test_uri_query.db?mode=ro", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

		REQUIRE(file_exists(expected_path));
		REQUIRE(!file_exists(unexpected_path));
		std::remove(expected_path);
		std::remove(unexpected_path);
	}

	// Test file: URI fragment is stripped
	{
		const char *expected_path = "test_uri_fragment.db";
		const char *unexpected_path = "test_uri_fragment.db#fragment";
		std::remove(expected_path);
		std::remove(unexpected_path);

		AdbcDatabase adbc_database;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(
		    SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", "file:test_uri_fragment.db#fragment", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

		REQUIRE(file_exists(expected_path));
		REQUIRE(!file_exists(unexpected_path));
		std::remove(expected_path);
		std::remove(unexpected_path);
	}

	// Test file://localhost/<abs path> is accepted
	{
		auto abs_path = TestCreatePath("test_uri_localhost.db");
		if (!abs_path.empty() && abs_path[0] != '/') {
			abs_path = duckdb::FileSystem::GetWorkingDirectory() + "/" + abs_path;
		}
		std::remove(abs_path.c_str());
		string uri = string("file://localhost") + abs_path;

		AdbcDatabase adbc_database;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", uri.c_str(), &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));

		REQUIRE(file_exists(abs_path.c_str()));
		std::remove(abs_path.c_str());
	}

	// Test file://<non-localhost>/<abs path> is rejected
	{
		auto abs_path = TestCreatePath("test_uri_non_localhost.db");
		if (!abs_path.empty() && abs_path[0] != '/') {
			abs_path = duckdb::FileSystem::GetWorkingDirectory() + "/" + abs_path;
		}
		string uri = string("file://example.com") + abs_path;

		AdbcDatabase adbc_database;
		REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
		REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "uri", uri.c_str(), &adbc_error)));
		REQUIRE(!SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));
		REQUIRE(adbc_error.message);
		REQUIRE((std::strcmp(adbc_error.message, "URI with a non-empty authority is not supported") == 0));
		adbc_error.release(&adbc_error);
		InitializeADBCError(&adbc_error);
		auto release_status = AdbcDatabaseRelease(&adbc_database, &adbc_error);
		REQUIRE((release_status == ADBC_STATUS_OK || release_status == ADBC_STATUS_INVALID_STATE));
	}

	adbc_error.release(&adbc_error);
}

TEST_CASE("ADBC - Database GetOption", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	AdbcError adbc_error = {};
	InitializeADBCError(&adbc_error);

	// Set up a database with a path and a config option
	AdbcDatabase adbc_database;
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "threads", "4", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	// GetOption: path
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcDatabaseGetOption(&adbc_database, "path", buf, &length, &adbc_error)));
		REQUIRE(std::string(buf) == ":memory:");
		REQUIRE(length == strlen(":memory:") + 1);
	}

	// GetOption: config round-trip
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcDatabaseGetOption(&adbc_database, "threads", buf, &length, &adbc_error)));
		REQUIRE(std::string(buf) == "4");
	}

	// GetOption: NOT_FOUND for unknown key
	{
		char buf[256];
		size_t length = sizeof(buf);
		auto status = AdbcDatabaseGetOption(&adbc_database, "nonexistent_option", buf, &length, &adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (adbc_error.release) {
			adbc_error.release(&adbc_error);
		}
		InitializeADBCError(&adbc_error);
	}

	// GetOption: buffer convention - size query (length=0)
	{
		size_t length = 0;
		REQUIRE(SUCCESS(AdbcDatabaseGetOption(&adbc_database, "path", nullptr, &length, &adbc_error)));
		REQUIRE(length == strlen(":memory:") + 1);
	}

	// GetOption: buffer convention - exact size
	{
		size_t length = strlen(":memory:") + 1;
		char buf[64];
		REQUIRE(SUCCESS(AdbcDatabaseGetOption(&adbc_database, "path", buf, &length, &adbc_error)));
		REQUIRE(std::string(buf) == ":memory:");
	}

	// GetOption: buffer convention - undersized buffer only sets length
	{
		size_t length = 2; // too small
		char buf[2] = {0};
		REQUIRE(SUCCESS(AdbcDatabaseGetOption(&adbc_database, "path", buf, &length, &adbc_error)));
		REQUIRE(length == strlen(":memory:") + 1);
		// Buffer should not have been written to (too small)
	}

	// GetOptionBytes: always NOT_FOUND
	{
		uint8_t buf[64];
		size_t length = sizeof(buf);
		auto status = AdbcDatabaseGetOptionBytes(&adbc_database, "path", buf, &length, &adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (adbc_error.release) {
			adbc_error.release(&adbc_error);
		}
		InitializeADBCError(&adbc_error);
	}

	// GetOptionInt: NOT_FOUND
	{
		int64_t val;
		auto status = AdbcDatabaseGetOptionInt(&adbc_database, "threads", &val, &adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (adbc_error.release) {
			adbc_error.release(&adbc_error);
		}
		InitializeADBCError(&adbc_error);
	}

	// SetOptionInt: convert to string and delegate
	{
		REQUIRE(SUCCESS(AdbcDatabaseSetOptionInt(&adbc_database, "threads", 2, &adbc_error)));
		// Verify round-trip via GetOption (string)
		char buf[64];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcDatabaseGetOption(&adbc_database, "threads", buf, &length, &adbc_error)));
		REQUIRE(std::string(buf) == "2");
	}

	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
	InitializeADBCError(&adbc_error);
}

TEST_CASE("ADBC - Connection GetOption autocommit", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Default: autocommit is enabled
	{
		char buf[64];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, buf, &length,
		                                        &db.adbc_error)));
		REQUIRE(std::string(buf) == ADBC_OPTION_VALUE_ENABLED);
	}

	// GetOptionInt: autocommit as integer
	{
		int64_t val = -1;
		REQUIRE(SUCCESS(
		    AdbcConnectionGetOptionInt(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, &val, &db.adbc_error)));
		REQUIRE(val == 1);
	}

	// Disable autocommit
	REQUIRE(SUCCESS(AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT,
	                                        ADBC_OPTION_VALUE_DISABLED, &db.adbc_error)));
	{
		char buf[64];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, buf, &length,
		                                        &db.adbc_error)));
		REQUIRE(std::string(buf) == ADBC_OPTION_VALUE_DISABLED);
	}

	// SetOptionInt: toggle autocommit back on
	REQUIRE(
	    SUCCESS(AdbcConnectionSetOptionInt(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, 1, &db.adbc_error)));
	{
		int64_t val = -1;
		REQUIRE(SUCCESS(
		    AdbcConnectionGetOptionInt(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, &val, &db.adbc_error)));
		REQUIRE(val == 1);
	}

	// NOT_FOUND for unknown key
	{
		char buf[64];
		size_t length = sizeof(buf);
		auto status = AdbcConnectionGetOption(&db.adbc_connection, "nonexistent", buf, &length, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}
}

TEST_CASE("ADBC - Connection GetOption current_catalog and current_db_schema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// current_catalog should return "memory" (DuckDB's default in-memory catalog name)
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "memory");
	}

	// current_db_schema should return "main"
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "main");
	}
}

TEST_CASE("ADBC - Connection SetOption current_catalog and current_db_schema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create a new schema to switch to
	db.Query("CREATE SCHEMA test_schema");

	// Set current_db_schema and verify
	REQUIRE(SUCCESS(AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
	                                        "test_schema", &db.adbc_error)));
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "test_schema");
	}

	// Switch back to main
	REQUIRE(SUCCESS(AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, "main",
	                                        &db.adbc_error)));
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "main");
	}

	// Set current_catalog (DuckDB in-memory database is "memory")
	// Setting to the same catalog should succeed
	REQUIRE(SUCCESS(AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, "memory",
	                                        &db.adbc_error)));
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "memory");
	}
}

TEST_CASE("ADBC - Database username/password not supported", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// SetOption for username should return NOT_IMPLEMENTED
	{
		auto status = AdbcDatabaseSetOption(&db.adbc_database, ADBC_OPTION_USERNAME, "user", &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// SetOption for password should return NOT_IMPLEMENTED
	{
		auto status = AdbcDatabaseSetOption(&db.adbc_database, ADBC_OPTION_PASSWORD, "pass", &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// GetOption for username should return NOT_IMPLEMENTED
	{
		char buf[64];
		size_t length = sizeof(buf);
		auto status = AdbcDatabaseGetOption(&db.adbc_database, ADBC_OPTION_USERNAME, buf, &length, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// GetOption for password should return NOT_IMPLEMENTED
	{
		char buf[64];
		size_t length = sizeof(buf);
		auto status = AdbcDatabaseGetOption(&db.adbc_database, ADBC_OPTION_PASSWORD, buf, &length, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}
}

TEST_CASE("ADBC - Statement GetOption ingestion options", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	AdbcStatement adbc_statement;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &adbc_statement, &db.adbc_error)));

	// Set all ingestion options
	REQUIRE(
	    SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "my_table", &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND,
	                                       &db.adbc_error)));
	REQUIRE(SUCCESS(
	    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "my_schema", &db.adbc_error)));
	REQUIRE(SUCCESS(
	    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_CATALOG, "my_catalog", &db.adbc_error)));

	// Read them back via GetOption
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(
		    AdbcStatementGetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, buf, &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "my_table");
	}
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(
		    SUCCESS(AdbcStatementGetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, buf, &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == ADBC_INGEST_OPTION_MODE_APPEND);
	}
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcStatementGetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, buf, &length,
		                                       &db.adbc_error)));
		REQUIRE(std::string(buf) == "my_schema");
	}
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(
		    AdbcStatementGetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_CATALOG, buf, &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "my_catalog");
	}

	// temporary: default is false
	{
		char buf[64];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(
		    AdbcStatementGetOption(&adbc_statement, ADBC_INGEST_OPTION_TEMPORARY, buf, &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == ADBC_OPTION_VALUE_DISABLED);
	}

	// SetOptionInt for temporary
	REQUIRE(SUCCESS(AdbcStatementSetOptionInt(&adbc_statement, ADBC_INGEST_OPTION_TEMPORARY, 1, &db.adbc_error)));
	{
		int64_t val = -1;
		REQUIRE(
		    SUCCESS(AdbcStatementGetOptionInt(&adbc_statement, ADBC_INGEST_OPTION_TEMPORARY, &val, &db.adbc_error)));
		REQUIRE(val == 1);
	}

	// Verify all mode strings round-trip
	for (const auto *mode : {ADBC_INGEST_OPTION_MODE_CREATE, ADBC_INGEST_OPTION_MODE_APPEND,
	                         ADBC_INGEST_OPTION_MODE_REPLACE, ADBC_INGEST_OPTION_MODE_CREATE_APPEND}) {
		REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, mode, &db.adbc_error)));
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(
		    SUCCESS(AdbcStatementGetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, buf, &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == mode);
	}

	// NOT_FOUND for unknown key
	{
		char buf[64];
		size_t length = sizeof(buf);
		auto status = AdbcStatementGetOption(&adbc_statement, "nonexistent", buf, &length, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// GetOptionBytes: NOT_FOUND
	{
		uint8_t buf[64];
		size_t length = sizeof(buf);
		auto status =
		    AdbcStatementGetOptionBytes(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, buf, &length, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// SetOptionBytes: NOT_IMPLEMENTED
	{
		uint8_t data[] = {1, 2, 3};
		auto status = AdbcStatementSetOptionBytes(&adbc_statement, "some_key", data, sizeof(data), &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// SetOptionDouble: NOT_IMPLEMENTED
	{
		auto status = AdbcStatementSetOptionDouble(&adbc_statement, "some_key", 3.14, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));
}

TEST_CASE("ADBC - Connection SetOptionBytes/Double not implemented", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// SetOptionBytes: NOT_IMPLEMENTED
	{
		uint8_t data[] = {1, 2, 3};
		auto status = AdbcConnectionSetOptionBytes(&db.adbc_connection, "some_key", data, sizeof(data), &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// SetOptionDouble: NOT_IMPLEMENTED
	{
		auto status = AdbcConnectionSetOptionDouble(&db.adbc_connection, "some_key", 3.14, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_IMPLEMENTED);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// GetOptionBytes: NOT_FOUND
	{
		uint8_t buf[64];
		size_t length = sizeof(buf);
		auto status = AdbcConnectionGetOptionBytes(&db.adbc_connection, "some_key", buf, &length, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// GetOptionDouble: NOT_FOUND
	{
		double val;
		auto status = AdbcConnectionGetOptionDouble(&db.adbc_connection, "some_key", &val, &db.adbc_error);
		REQUIRE(status == ADBC_STATUS_NOT_FOUND);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}
}

TEST_CASE("ADBC - ConnectionSetOption NULL value should not crash", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// ConnectionSetOption with NULL value for CURRENT_CATALOG should not crash
	{
		auto status = AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, nullptr,
		                                      &db.adbc_error);
		REQUIRE(status != ADBC_STATUS_OK);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// ConnectionSetOption with NULL value for CURRENT_DB_SCHEMA should not crash
	{
		auto status = AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, nullptr,
		                                      &db.adbc_error);
		REQUIRE(status != ADBC_STATUS_OK);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// ConnectionSetOption with NULL value for AUTOCOMMIT should not crash
	{
		auto status =
		    AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_AUTOCOMMIT, nullptr, &db.adbc_error);
		REQUIRE(status != ADBC_STATUS_OK);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// ConnectionSetOption with NULL value for unknown key should not crash
	{
		auto status = AdbcConnectionSetOption(&db.adbc_connection, "unknown_key", nullptr, &db.adbc_error);
		REQUIRE(status != ADBC_STATUS_OK);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// Connection should still be functional after failed SetOption calls
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "memory");
	}
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "main");
	}
}

TEST_CASE("ADBC - ConnectionSetOption non-existent catalog and schema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Setting a non-existent catalog should fail gracefully
	{
		auto status = AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG,
		                                      "nonexistent_catalog", &db.adbc_error);
		REQUIRE(status != ADBC_STATUS_OK);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// Setting a non-existent schema should fail gracefully
	{
		auto status = AdbcConnectionSetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA,
		                                      "nonexistent_schema", &db.adbc_error);
		REQUIRE(status != ADBC_STATUS_OK);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	// Connection should still be functional after failed SetOption calls
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_CATALOG, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "memory");
	}
	{
		char buf[256];
		size_t length = sizeof(buf);
		REQUIRE(SUCCESS(AdbcConnectionGetOption(&db.adbc_connection, ADBC_CONNECTION_OPTION_CURRENT_DB_SCHEMA, buf,
		                                        &length, &db.adbc_error)));
		REQUIRE(std::string(buf) == "main");
	}
}

TEST_CASE("ADBC - Concurrent statements on same connection", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create two statements on the same connection
	AdbcStatement stmt1, stmt2;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt2, &db.adbc_error)));

	// Execute first query - get stream1
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt1, "SELECT 1 AS a, 'hello' AS b", &db.adbc_error)));
	ArrowArrayStream stream1;
	int64_t rows1;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt1, &stream1, &rows1, &db.adbc_error)));

	// Execute second query on the same connection - this should materialize stream1
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt2, "SELECT 2 AS c, 'world' AS d", &db.adbc_error)));
	ArrowArrayStream stream2;
	int64_t rows2;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt2, &stream2, &rows2, &db.adbc_error)));

	// Read from stream1 - should work because it was materialized
	ArrowArray batch1;
	REQUIRE(stream1.get_next(&stream1, &batch1) == 0);
	REQUIRE(batch1.release != nullptr);
	REQUIRE(batch1.length == 1);
	REQUIRE(batch1.n_children == 2);
	batch1.release(&batch1);

	// Verify end of stream1
	ArrowArray batch1_end;
	REQUIRE(stream1.get_next(&stream1, &batch1_end) == 0);
	REQUIRE(batch1_end.release == nullptr);

	// Read from stream2 - should work as the active streaming result
	ArrowArray batch2;
	REQUIRE(stream2.get_next(&stream2, &batch2) == 0);
	REQUIRE(batch2.release != nullptr);
	REQUIRE(batch2.length == 1);
	REQUIRE(batch2.n_children == 2);
	batch2.release(&batch2);

	// Verify end of stream2
	ArrowArray batch2_end;
	REQUIRE(stream2.get_next(&stream2, &batch2_end) == 0);
	REQUIRE(batch2_end.release == nullptr);

	// Clean up
	stream1.release(&stream1);
	stream2.release(&stream2);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt2, &db.adbc_error)));
}

TEST_CASE("ADBC - Concurrent statements with large result", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create a table with enough data to produce multiple batches
	db.Query("CREATE TABLE test_large AS SELECT i, 'value_' || i::VARCHAR AS v FROM range(5000) t(i)");

	AdbcStatement stmt1, stmt2;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt2, &db.adbc_error)));

	// Execute first query
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt1, "SELECT * FROM test_large ORDER BY i", &db.adbc_error)));
	ArrowArrayStream stream1;
	int64_t rows1;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt1, &stream1, &rows1, &db.adbc_error)));

	// Read first batch from stream1 before executing second query
	ArrowArray first_batch;
	REQUIRE(stream1.get_next(&stream1, &first_batch) == 0);
	REQUIRE(first_batch.release != nullptr);
	REQUIRE(first_batch.length > 0);
	auto first_batch_len = first_batch.length;
	first_batch.release(&first_batch);

	// Execute second query - materializes remaining data in stream1
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt2, "SELECT 42 AS answer", &db.adbc_error)));
	ArrowArrayStream stream2;
	int64_t rows2;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt2, &stream2, &rows2, &db.adbc_error)));

	// Continue reading from stream1 - should get remaining materialized data
	int64_t total_rows = first_batch_len;
	while (true) {
		ArrowArray batch;
		REQUIRE(stream1.get_next(&stream1, &batch) == 0);
		if (!batch.release) {
			break;
		}
		total_rows += batch.length;
		batch.release(&batch);
	}
	REQUIRE(total_rows == 5000);

	// Read from stream2
	ArrowArray batch2;
	REQUIRE(stream2.get_next(&stream2, &batch2) == 0);
	REQUIRE(batch2.release != nullptr);
	REQUIRE(batch2.length == 1);
	batch2.release(&batch2);

	// Clean up
	stream1.release(&stream1);
	stream2.release(&stream2);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt2, &db.adbc_error)));
}

TEST_CASE("ADBC - Concurrent statements: release before second query", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	AdbcStatement stmt1, stmt2;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt2, &db.adbc_error)));

	// Execute and release first stream before second query
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt1, "SELECT 1 AS a", &db.adbc_error)));
	ArrowArrayStream stream1;
	int64_t rows1;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt1, &stream1, &rows1, &db.adbc_error)));
	stream1.release(&stream1);

	// Second query should work fine (active_streams was cleaned up by release)
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt2, "SELECT 2 AS b", &db.adbc_error)));
	ArrowArrayStream stream2;
	int64_t rows2;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt2, &stream2, &rows2, &db.adbc_error)));

	ArrowArray batch;
	REQUIRE(stream2.get_next(&stream2, &batch) == 0);
	REQUIRE(batch.release != nullptr);
	REQUIRE(batch.length == 1);
	batch.release(&batch);

	stream2.release(&stream2);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt2, &db.adbc_error)));
}

TEST_CASE("ADBC - Three concurrent statements", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	AdbcStatement stmt1, stmt2, stmt3;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt2, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt3, &db.adbc_error)));

	// Execute three queries sequentially, keeping all streams open
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt1, "SELECT 1 AS a", &db.adbc_error)));
	ArrowArrayStream stream1;
	int64_t rows;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt1, &stream1, &rows, &db.adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt2, "SELECT 2 AS b", &db.adbc_error)));
	ArrowArrayStream stream2;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt2, &stream2, &rows, &db.adbc_error)));

	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt3, "SELECT 3 AS c", &db.adbc_error)));
	ArrowArrayStream stream3;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt3, &stream3, &rows, &db.adbc_error)));

	// Read all three streams — stream1 and stream2 are materialized, stream3 is live
	ArrowArray batch1, batch2, batch3;
	REQUIRE(stream1.get_next(&stream1, &batch1) == 0);
	REQUIRE(batch1.release != nullptr);
	REQUIRE(batch1.length == 1);
	batch1.release(&batch1);

	REQUIRE(stream2.get_next(&stream2, &batch2) == 0);
	REQUIRE(batch2.release != nullptr);
	REQUIRE(batch2.length == 1);
	batch2.release(&batch2);

	REQUIRE(stream3.get_next(&stream3, &batch3) == 0);
	REQUIRE(batch3.release != nullptr);
	REQUIRE(batch3.length == 1);
	batch3.release(&batch3);

	// Verify all streams end
	ArrowArray end;
	REQUIRE(stream1.get_next(&stream1, &end) == 0);
	REQUIRE(end.release == nullptr);
	REQUIRE(stream2.get_next(&stream2, &end) == 0);
	REQUIRE(end.release == nullptr);
	REQUIRE(stream3.get_next(&stream3, &end) == 0);
	REQUIRE(end.release == nullptr);

	stream1.release(&stream1);
	stream2.release(&stream2);
	stream3.release(&stream3);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt1, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt2, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt3, &db.adbc_error)));
}

TEST_CASE("Test ADBC ConnectionGetInfo with an open result stream", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	// Create enough rows that the query result spans multiple Arrow batches, so that
	// partially reading it and resuming afterwards is actually meaningful.
	db.Query("CREATE TABLE test_getinfo_large AS SELECT i, 'value_' || i::VARCHAR AS v FROM range(5000) t(i)");

	// Open a streaming result on the connection and keep it alive.
	AdbcStatement stmt;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "SELECT * FROM test_getinfo_large ORDER BY i", &db.adbc_error)));
	ArrowArrayStream query_stream = {};
	int64_t rows;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, &query_stream, &rows, &db.adbc_error)));

	// Read only the first batch before calling GetInfo, leaving the rest of the
	// result unread on the still-open stream.
	ArrowArray first_batch;
	REQUIRE(query_stream.get_next(&query_stream, &first_batch) == 0);
	REQUIRE(first_batch.release != nullptr);
	REQUIRE(first_batch.length > 0);
	REQUIRE(first_batch.length < 5000);
	int64_t total_rows = first_batch.length;
	first_batch.release(&first_batch);

	// ConnectionGetInfo assembles its result without executing a query. Interleaving it
	// with a partially-read query_stream must therefore leave that stream able to keep
	// yielding its remaining batches. This checks that observable behaviour only; it
	// does not prove anything about how GetInfo is implemented internally.
	static uint32_t info_codes[] = {ADBC_INFO_VENDOR_NAME};
	ArrowArrayStream info_stream = {};
	REQUIRE(SUCCESS(AdbcConnectionGetInfo(&db.adbc_connection, info_codes, 1, &info_stream, &db.adbc_error)));

	// Read the remaining batches of query_stream to completion, counting how many
	// batches get_next actually hands back. If this were only ever 1, the "partial
	// read" above would be meaningless and the test would be vacuous again.
	int64_t batch_count = 1;
	while (true) {
		ArrowArray batch;
		REQUIRE(query_stream.get_next(&query_stream, &batch) == 0);
		if (!batch.release) {
			break;
		}
		batch_count++;
		total_rows += batch.length;
		batch.release(&batch);
	}
	REQUIRE(batch_count > 1);
	REQUIRE(total_rows == 5000);

	// The GetInfo stream itself must also be readable.
	ArrowArray info_batch;
	REQUIRE(info_stream.get_next(&info_stream, &info_batch) == 0);
	REQUIRE(info_batch.release != nullptr);
	REQUIRE(info_batch.length == 1);
	info_batch.release(&info_batch);

	// The connection must still be usable for further queries afterwards.
	AdbcStatement stmt2;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt2, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt2, "SELECT 2 AS c", &db.adbc_error)));
	ArrowArrayStream stream2 = {};
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt2, &stream2, &rows, &db.adbc_error)));
	ArrowArray batch2;
	REQUIRE(stream2.get_next(&stream2, &batch2) == 0);
	REQUIRE(batch2.release != nullptr);
	REQUIRE(batch2.length == 1);
	batch2.release(&batch2);

	// Clean up.
	query_stream.release(&query_stream);
	info_stream.release(&info_stream);
	stream2.release(&stream2);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt2, &db.adbc_error)));
}

TEST_CASE("ADBC - Connection release while stream is active", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	// Set up database manually (not using ADBCTestDatabase since we need to control release order)
	AdbcDatabase adbc_database;
	AdbcError adbc_error;
	std::memset(&adbc_error, 0, sizeof(adbc_error));
	std::memset(&adbc_database, 0, sizeof(adbc_database));
	REQUIRE(SUCCESS(AdbcDatabaseNew(&adbc_database, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "driver", duckdb_lib, &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "entrypoint", "duckdb_adbc_init", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseSetOption(&adbc_database, "path", ":memory:", &adbc_error)));
	REQUIRE(SUCCESS(AdbcDatabaseInit(&adbc_database, &adbc_error)));

	AdbcConnection adbc_connection;
	std::memset(&adbc_connection, 0, sizeof(adbc_connection));
	REQUIRE(SUCCESS(AdbcConnectionNew(&adbc_connection, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionInit(&adbc_connection, &adbc_database, &adbc_error)));

	AdbcStatement stmt;
	REQUIRE(SUCCESS(AdbcStatementNew(&adbc_connection, &stmt, &adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "SELECT 1 AS a", &adbc_error)));

	ArrowArrayStream stream;
	int64_t rows;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, &stream, &rows, &adbc_error)));

	// Release connection while stream is still active
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &adbc_error)));
	REQUIRE(SUCCESS(AdbcConnectionRelease(&adbc_connection, &adbc_error)));

	// Stream should still be usable (materialized data is independent, conn_wrapper is nulled out)
	ArrowArray batch;
	REQUIRE(stream.get_next(&stream, &batch) == 0);
	REQUIRE(batch.release != nullptr);
	REQUIRE(batch.length == 1);
	batch.release(&batch);

	// Release stream after connection is gone — should not crash
	stream.release(&stream);

	REQUIRE(SUCCESS(AdbcDatabaseRelease(&adbc_database, &adbc_error)));
}

TEST_CASE("ADBC - Re-execute same statement while prior stream is open", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	AdbcStatement stmt;
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &db.adbc_error)));

	// Execute first query
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "SELECT 1 AS a", &db.adbc_error)));
	ArrowArrayStream stream1;
	int64_t rows;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, &stream1, &rows, &db.adbc_error)));

	// Re-execute the same statement — should materialize stream1
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "SELECT 2 AS b", &db.adbc_error)));
	ArrowArrayStream stream2;
	REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&stmt, &stream2, &rows, &db.adbc_error)));

	// Both streams should be readable
	ArrowArray batch1;
	REQUIRE(stream1.get_next(&stream1, &batch1) == 0);
	REQUIRE(batch1.release != nullptr);
	REQUIRE(batch1.length == 1);
	batch1.release(&batch1);

	ArrowArray batch2;
	REQUIRE(stream2.get_next(&stream2, &batch2) == 0);
	REQUIRE(batch2.release != nullptr);
	REQUIRE(batch2.length == 1);
	batch2.release(&batch2);

	stream1.release(&stream1);
	stream2.release(&stream2);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
}

TEST_CASE("ADBC - Create same macro twice (unhappy)", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	const auto query = "CREATE MACRO my_macro(x) AS x + 1";
	auto result = db.Query(query);
	REQUIRE(!result->HasError());

	AdbcStatement stmt;
	InitializeADBCError(&db.adbc_error);
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, query, &db.adbc_error)));

	ArrowArrayStream out_stream;
	out_stream.release = nullptr;
	int64_t rows_affected = -1;
	auto status = AdbcStatementExecuteQuery(&stmt, &out_stream, &rows_affected, &db.adbc_error);

	REQUIRE(!SUCCESS(status));
	REQUIRE(status == ADBC_STATUS_INVALID_ARGUMENT);

	REQUIRE(out_stream.release == nullptr);
	REQUIRE(db.adbc_error.release != nullptr);
	db.adbc_error.release(&db.adbc_error);

	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
}

TEST_CASE("ADBC - Ingest to nonexistent schema (unhappy)", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	auto &input_data = db.QueryArrow("SELECT 44 as value");

	AdbcStatement adbc_statement;
	InitializeADBCError(&db.adbc_error);

	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &adbc_statement, &db.adbc_error)));
	REQUIRE(SUCCESS(
	    AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_TABLE, "test_table", &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "nonexistent_schema",
	                                       &db.adbc_error)));

	const char *ingest_option_mode;
	SECTION("REPLACE") {
		ingest_option_mode = ADBC_INGEST_OPTION_MODE_REPLACE;
	}
	SECTION("CREATE_APPEND") {
		ingest_option_mode = ADBC_INGEST_OPTION_MODE_CREATE_APPEND;
	}

	REQUIRE(
	    SUCCESS(AdbcStatementSetOption(&adbc_statement, ADBC_INGEST_OPTION_MODE, ingest_option_mode, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementBindStream(&adbc_statement, &input_data, &db.adbc_error)));
	int64_t rows_affected;
	auto status = AdbcStatementExecuteQuery(&adbc_statement, nullptr, &rows_affected, &db.adbc_error);
	REQUIRE(!SUCCESS(status));

	REQUIRE(input_data.release == nullptr);
	REQUIRE(db.adbc_error.release != nullptr);
	db.adbc_error.release(&db.adbc_error);

	REQUIRE(SUCCESS(AdbcStatementRelease(&adbc_statement, &db.adbc_error)));
}

TEST_CASE("ADBC - Parameterized statement breaking unique constraint (unhappy)", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	auto r1 = db.Query("CREATE TABLE test_table(x INTEGER UNIQUE)");
	REQUIRE(!r1->HasError());
	auto r2 = db.Query("INSERT INTO test_table VALUES (42)");
	REQUIRE(!r2->HasError());

	AdbcStatement stmt;
	InitializeADBCError(&db.adbc_error);
	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "INSERT INTO test_table VALUES ($1)", &db.adbc_error)));
	REQUIRE(SUCCESS(AdbcStatementPrepare(&stmt, &db.adbc_error)));

	// Bind a stream containing the duplicate value — will trigger a UNIQUE violation
	auto &input_data = db.QueryArrow("SELECT 42 AS x");
	REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input_data, &db.adbc_error)));

	int64_t rows_affected;
	auto status = AdbcStatementExecuteQuery(&stmt, nullptr, &rows_affected, &db.adbc_error);

	REQUIRE(!SUCCESS(status));
	REQUIRE(status == ADBC_STATUS_INVALID_ARGUMENT);

	REQUIRE(input_data.release == nullptr);
	REQUIRE(db.adbc_error.release != nullptr);
	db.adbc_error.release(&db.adbc_error);
	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
}

TEST_CASE("ADBC - regression test for #21772", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	ADBCTestDatabase db;

	// Create a table large enough that streaming requires multiple fetch calls.
	auto r = db.Query("CREATE TABLE big AS SELECT * FROM range(500000) r(i)");
	REQUIRE(!r->HasError());

	constexpr int ITERATIONS = 50;

	for (int iter = 0; iter < ITERATIONS; iter++) {
		AdbcStatement query_stmt;
		ArrowArrayStream stream;
		stream.release = nullptr;
		int64_t rows_affected;

		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &query_stmt, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&query_stmt, "SELECT * FROM big", &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementExecuteQuery(&query_stmt, &stream, &rows_affected, &db.adbc_error)));
		REQUIRE(SUCCESS(AdbcStatementRelease(&query_stmt, &db.adbc_error)));

		std::thread releaser([&stream]() {
			if (stream.release) {
				stream.release(&stream);
			}
		});

		{
			AdbcStatement materialize_stmt;
			auto status = AdbcStatementNew(&db.adbc_connection, &materialize_stmt, &db.adbc_error);
			if (status == ADBC_STATUS_OK) {
				AdbcStatementSetSqlQuery(&materialize_stmt, "SELECT 1", &db.adbc_error);
				AdbcStatementRelease(&materialize_stmt, &db.adbc_error);
			}
		}

		releaser.join();
		if (stream.release) {
			stream.release(&stream);
		}
	}
}

TEST_CASE("ADBC - StatementExecuteSchema", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}

	ADBCTestDatabase db;
	AdbcStatement stmt;

	REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &db.adbc_error)));

	SECTION("basic SELECT") {
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "SELECT 42 AS answer, 'hello' AS greeting", &db.adbc_error)));

		ArrowSchema schema;
		schema.release = nullptr;
		REQUIRE(SUCCESS(AdbcStatementExecuteSchema(&stmt, &schema, &db.adbc_error)));

		REQUIRE(schema.n_children == 2);
		REQUIRE(string(schema.children[0]->name) == "answer");
		REQUIRE(string(schema.children[1]->name) == "greeting");
		schema.release(&schema);
	}

	SECTION("no query set") {
		ArrowSchema schema;
		schema.release = nullptr;
		REQUIRE(!SUCCESS(AdbcStatementExecuteSchema(&stmt, &schema, &db.adbc_error)));
		REQUIRE(string(db.adbc_error.message).find("StatementSetSqlQuery") != string::npos);
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	SECTION("missing schema pointer") {
		REQUIRE(SUCCESS(AdbcStatementSetSqlQuery(&stmt, "SELECT 1", &db.adbc_error)));
		REQUIRE(!SUCCESS(AdbcStatementExecuteSchema(&stmt, nullptr, &db.adbc_error)));
		if (db.adbc_error.release) {
			db.adbc_error.release(&db.adbc_error);
		}
		InitializeADBCError(&db.adbc_error);
	}

	REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
}

TEST_CASE("ADBC - Rich Error Metadata API", "[adbc]") {
	if (!duckdb_lib) {
		return;
	}
	ADBCTestDatabase db;

	SECTION("Function pointers are wired for 1.1.0") {
		auto *driver = db.adbc_database.private_driver;
		REQUIRE(driver != nullptr);
		REQUIRE(driver->ErrorGetDetailCount != nullptr);
		REQUIRE(driver->ErrorGetDetail != nullptr);
	}

	SECTION("No details for zero-initialized error") {
		AdbcError error = {};
		auto *driver = db.adbc_database.private_driver;
		REQUIRE(driver->ErrorGetDetailCount(&error) == 0);
		auto detail = driver->ErrorGetDetail(&error, 0);
		REQUIRE(detail.key == nullptr);
		REQUIRE(detail.value == nullptr);
	}

	SECTION("Provides duckdb:error_type detail for ingestion CREATE conflict") {
		// Create a table first (uses db.adbc_error which is zero-init)
		auto &input = db.QueryArrow("SELECT 1 AS id");
		db.CreateTable("rich_error_meta_tbl", input);

		// Try to ingest to the same table in CREATE mode (default) with ADBC_ERROR_INIT
		// This triggers a Catalog error ("already exists")
		auto &input2 = db.QueryArrow("SELECT 2 AS id");
		AdbcStatement stmt = {};
		AdbcError error = ADBC_ERROR_INIT;

		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &stmt, &error)));
		REQUIRE(SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "rich_error_meta_tbl", &error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input2, &error)));

		auto status = AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &error);
		REQUIRE(status == ADBC_STATUS_ALREADY_EXISTS);
		REQUIRE(error.message != nullptr);

		// With ADBC_ERROR_INIT, structured error details should be available
		int count = AdbcErrorGetDetailCount(&error);
		REQUIRE(count > 0);

		bool found_error_type = false;
		for (int i = 0; i < count; i++) {
			auto detail = AdbcErrorGetDetail(&error, i);
			if (detail.key && std::strcmp(detail.key, "duckdb:error_type") == 0) {
				found_error_type = true;
				REQUIRE(detail.value != nullptr);
				REQUIRE(detail.value_length > 0);
				std::string type_str(reinterpret_cast<const char *>(detail.value), detail.value_length);
				REQUIRE(type_str == "Catalog");
			}
		}
		REQUIRE(found_error_type);

		// Out-of-bounds index returns empty detail
		auto oob = AdbcErrorGetDetail(&error, 999);
		REQUIRE(oob.key == nullptr);
		REQUIRE(oob.value == nullptr);

		if (error.release) {
			error.release(&error);
		}
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
	}

	SECTION("No duckdb:error_type detail when error is zero-initialized") {
		// Same scenario but with zero-init error (no ADBC_ERROR_INIT)
		auto &input = db.QueryArrow("SELECT 1 AS id");
		db.CreateTable("rich_error_meta_tbl2", input);

		auto &input2 = db.QueryArrow("SELECT 2 AS id");
		AdbcStatement stmt = {};
		AdbcError error = {}; // zero-init: vendor_code = 0, not ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA

		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection_ingest, &stmt, &error)));
		REQUIRE(
		    SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "rich_error_meta_tbl2", &error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input2, &error)));

		auto status = AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &error);
		REQUIRE(status == ADBC_STATUS_ALREADY_EXISTS);

		// With zero-init error, no structured details
		REQUIRE(AdbcErrorGetDetailCount(&error) == 0);

		if (error.release) {
			error.release(&error);
		}
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
	}

	SECTION("Provides duckdb:error_type detail for APPEND to missing table") {
		auto &input = db.QueryArrow("SELECT 42 as value");
		AdbcStatement stmt = {};
		AdbcError error = ADBC_ERROR_INIT;

		REQUIRE(SUCCESS(AdbcStatementNew(&db.adbc_connection, &stmt, &error)));
		REQUIRE(SUCCESS(
		    AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "missing_table_rich_append", &error)));
		REQUIRE(
		    SUCCESS(AdbcStatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &error)));
		REQUIRE(SUCCESS(AdbcStatementBindStream(&stmt, &input, &error)));

		auto status = AdbcStatementExecuteQuery(&stmt, nullptr, nullptr, &error);
		REQUIRE(!SUCCESS(status));
		REQUIRE(error.message != nullptr);

		int count = AdbcErrorGetDetailCount(&error);
		REQUIRE(count > 0);

		bool found_error_type = false;
		for (int i = 0; i < count; i++) {
			auto detail = AdbcErrorGetDetail(&error, i);
			if (detail.key && std::strcmp(detail.key, "duckdb:error_type") == 0) {
				found_error_type = true;
				REQUIRE(detail.value != nullptr);
				REQUIRE(detail.value_length > 0);
				std::string type_str(reinterpret_cast<const char *>(detail.value), detail.value_length);
				REQUIRE(type_str == "Catalog");
			}
		}
		REQUIRE(found_error_type);

		if (error.release) {
			error.release(&error);
		}
		REQUIRE(SUCCESS(AdbcStatementRelease(&stmt, &db.adbc_error)));
		if (input.release) {
			input.release(&input);
		}
	}
}

} // namespace duckdb
