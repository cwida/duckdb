#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/main/config.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

constexpr const char *TEST_INDEX_TYPE = "TEST_EXTENSION_INDEX";

//! Models an index type provided by an extension, by registering ART under another name.
void RegisterTestIndexType(DuckDB &db) {
	auto index_type = ART::GetARTIndexType();
	index_type.name = TEST_INDEX_TYPE;
	DBConfig::GetConfig(*db.instance).GetIndexTypes().RegisterIndexType(index_type);
}

//! Reading the WAL file while the database is open is not portable, so we go through the database size.
void RequireWALEmpty(Connection &con, const bool empty) {
	auto result = con.Query("SELECT wal_size = '0 bytes' FROM pragma_database_size()");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BOOLEAN(empty)}));
}

} // namespace

TEST_CASE("Checkpoint buffered replays of an index type that is not loaded", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;

	auto database_path = TestCreatePath("checkpoint_unknown_index_type");
	DeleteDatabase(database_path);

	{
		DuckDB db(database_path, config.get());
		RegisterTestIndexType(db);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext(k INTEGER)"));
		REQUIRE_NO_FAIL(
		    con.Query(StringUtil::Format("CREATE UNIQUE INDEX ext_idx ON ext USING %s (k)", TEST_INDEX_TYPE)));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT i AS a, i AS b FROM range(100000) r(i)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		// Only in the WAL, so that the replay buffers these rows in the unbound index.
		REQUIRE_NO_FAIL(con.Query("INSERT INTO ext SELECT i FROM range(100) r(i)"));
	}

	{
		// The index type is not loaded: the buffered replays cannot be applied.
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("CHECKPOINT");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Cannot CHECKPOINT"));

		// A failed bind leaves the index unbound, so binding it again fails instead of hanging.
		for (idx_t i = 0; i < 2; i++) {
			REQUIRE(con.Query("INSERT INTO ext VALUES (1000)")->HasError());
		}

		// The automatic checkpoint is skipped, so this commit must not skip its WAL write.
		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint='1KB'"));
		REQUIRE_NO_FAIL(con.Query("UPDATE big SET b = b + 1"));
		RequireWALEmpty(con, false);
	}

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT sum(b) FROM big");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(5000050000)}));

		// A failed bind can be retried: once the index type is loaded, the checkpoint persists the buffered replays.
		REQUIRE(con.Query("CHECKPOINT")->HasError());
		RegisterTestIndexType(db);
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		RequireWALEmpty(con, true);
	}

	{
		// The index type is not loaded, but the index has no buffered replays: it does not block the checkpoint.
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint='1KB'"));
		REQUIRE_NO_FAIL(con.Query("UPDATE big SET b = b - 1"));
		RequireWALEmpty(con, true);
	}

	{
		DuckDB db(database_path, config.get());
		RegisterTestIndexType(db);
		Connection con(db);
		// The unique index kept the rows that were only in the WAL.
		REQUIRE(con.Query("INSERT INTO ext VALUES (50)")->HasError());
		REQUIRE_NO_FAIL(con.Query("DELETE FROM ext"));
	}

	DeleteDatabase(database_path);
}
