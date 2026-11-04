#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/profiler/metrics.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

// the rest is in duckdb_live_query_metrics.test: SQL cannot pause a running query or hook into its start

static idx_t LiveMetric(Connection &con, const char *name) {
	auto metrics = con.context->GetLiveQueryMetrics();
	auto entry = metrics.find(name);
	REQUIRE(entry != metrics.end());
	return entry->second.GetValue<idx_t>();
}

TEST_CASE("Test live query metrics of a running query are readable from another connection", "[api]") {
	auto path = TestCreatePath("live_query_metrics_remote.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(10000000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	{
		DuckDB db(path);
		Connection con(db);
		Connection monitor(db);

		// pause a streaming scan after its first chunk; a second connection sees its bytes read so far
		auto streaming = OpenStream(con, "SELECT i FROM t");
		REQUIRE(streaming->Fetch());
		auto result = monitor.Query("SELECT CAST(metric_value AS UBIGINT) > 0 FROM duckdb_live_query_metrics() "
		                            "WHERE metric_name = 'io.total_bytes_read' AND connection_id = " +
		                            to_string(con.context->GetConnectionId()));
		REQUIRE(CHECK_COLUMN(result, 0, {true}));
		REQUIRE(LiveMetric(con, MetricIOTotalReadOperations::Name) > 0);
	}
	DeleteDatabase(path);
}

// smaller vectors run the sort out of memory before it spills
#if STANDARD_VECTOR_SIZE >= 2048
TEST_CASE("Test live query metrics show that a running query has spilled", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET temp_directory = '" + TestCreatePath("live_query_metrics_spill") + "'"));
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("SET preserve_insertion_order = false"));
	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '100MB'"));

	// sorting ~160MB under a 100MB limit spills before the first chunk is returned; the query is still running
	auto streaming = OpenStream(con, "SELECT i FROM range(20000000) t(i) ORDER BY i DESC");
	auto chunk = streaming->Fetch();
	if (streaming->HasError()) {
		FAIL(streaming->GetError());
	}
	REQUIRE(chunk);
	REQUIRE(LiveMetric(con, MetricSystemTotalBytesSpilled::Name) > 0);
}
#endif

//! Allocates a buffer when a query begins, before the statement is planned, and holds it until the query ends
struct AllocateAtQueryBeginState : public ClientContextState {
	static constexpr idx_t ALLOCATION_SIZE = 1024 * 1024;

	void QueryBegin(ClientContext &context) override {
		buffer = BufferManager::GetBufferManager(context).Allocate(MemoryTag::EXTENSION, ALLOCATION_SIZE);
	}
	void QueryEnd(ClientContext &context) override {
		buffer.Destroy();
	}

	BufferHandle buffer;
};

TEST_CASE("Test live query metrics keep the memory a statement allocates before it is planned", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.context->registered_state->Insert("allocate_at_query_begin", make_shared_ptr<AllocateAtQueryBeginState>());

	SECTION("profiling disabled") {
	}
	SECTION("profiling enabled") {
		REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	}
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE(LiveMetric(con, MetricSystemTotalMemoryAllocated::Name) >= AllocateAtQueryBeginState::ALLOCATION_SIZE);
}
