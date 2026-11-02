#include "catch.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static idx_t ConnectionSecretCount(Connection &con) {
	auto result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE storage = 'connection'");
	REQUIRE_NO_FAIL(*result);
	return static_cast<idx_t>(result->Collection().GetValue(0, 0).GetValue<int64_t>());
}

TEST_CASE("Test closing a connection that created connection secrets", "[api]") {
	DuckDB database(nullptr);

	// An older connection holds an open transaction, which keeps the transaction that creates the secret from being
	// cleaned up when it commits
	Connection older(database);
	REQUIRE_NO_FAIL(older.Query("BEGIN"));
	REQUIRE(ConnectionSecretCount(older) == 0);

	{
		Connection creator(database);
		REQUIRE_NO_FAIL(creator.Query("CREATE SECRET s IN connection (TYPE http)"));
		REQUIRE(ConnectionSecretCount(creator) == 1);
	}

	// The secrets of the closed connection are gone, and are never visible to another connection
	REQUIRE(ConnectionSecretCount(older) == 0);
	REQUIRE_NO_FAIL(older.Query("COMMIT"));
	REQUIRE(ConnectionSecretCount(older) == 0);
}

TEST_CASE("Test rolling back connection secrets", "[api]") {
	DuckDB database(nullptr);
	Connection con(database);

	REQUIRE_NO_FAIL(con.Query("CREATE SECRET s IN connection (TYPE http, SCOPE 'http://committed')"));

	// A rolled-back replace restores the original secret, not just its name
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE SECRET s IN connection (TYPE http, SCOPE 'http://replaced')"));
	auto result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 's'");
	REQUIRE(CHECK_COLUMN(result, 0, {Value("http://replaced")}));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 's'");
	REQUIRE(CHECK_COLUMN(result, 0, {Value("http://committed")}));

	// A secret created and dropped within the same aborted transaction does not come back
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("CREATE SECRET tmp IN connection (TYPE http)"));
	REQUIRE_NO_FAIL(con.Query("DROP SECRET tmp FROM connection"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(ConnectionSecretCount(con) == 1);

	// A failing statement rolls back its own implicit transaction
	REQUIRE_FAIL(con.Query("CREATE SECRET s IN connection (TYPE http)"));
	result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 's'");
	REQUIRE(CHECK_COLUMN(result, 0, {Value("http://committed")}));
}
