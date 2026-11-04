#include "catch.hpp"

#include "duckdb/execution/operator/persistent/batch_memory_manager.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

static constexpr idx_t BATCH_MEMORY_PER_THREAD = 4ULL * 1024ULL * 1024ULL;

TEST_CASE("BatchMemoryManager reserves memory only once it is requested", "[storage][batch_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto &temporary_memory_manager = TemporaryMemoryManager::Get(context);
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 0);

	BatchMemoryManager memory_manager(context, BATCH_MEMORY_PER_THREAD);
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 0);
	REQUIRE(temporary_memory_manager.GetTotalReservation() == 0);
	REQUIRE(memory_manager.AvailableMemory() == 0);

	REQUIRE(memory_manager.MaxThreads(4) >= 1);
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 1);
	REQUIRE(memory_manager.AvailableMemory() >= BATCH_MEMORY_PER_THREAD);
	REQUIRE(temporary_memory_manager.GetTotalReservation() == memory_manager.AvailableMemory());

	memory_manager.Release();
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 0);
	REQUIRE(temporary_memory_manager.GetTotalReservation() == 0);
	REQUIRE(memory_manager.AvailableMemory() == 0);
}

// alternative verify makes OutOfMemory always report true without requesting memory
#ifndef DUCKDB_ALTERNATIVE_VERIFY
TEST_CASE("BatchMemoryManager reserves memory when a sink runs out without scheduling",
          "[storage][batch_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto &temporary_memory_manager = TemporaryMemoryManager::Get(context);

	BatchMemoryManager memory_manager(context, BATCH_MEMORY_PER_THREAD);
	memory_manager.IncreaseUnflushedMemory(1);
	// a batch above the minimum batch index that exceeds the (empty) reservation asks for memory
	REQUIRE(!memory_manager.OutOfMemory(1));
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 1);
	REQUIRE(memory_manager.AvailableMemory() >= BATCH_MEMORY_PER_THREAD);

	memory_manager.ReduceUnflushedMemory(1);
	memory_manager.FinalCheck();
	memory_manager.Release();
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 0);
	REQUIRE(temporary_memory_manager.GetTotalReservation() == 0);
}
#endif

TEST_CASE("Batch insert in a CTE releases its reservation before the query ends", "[storage][batch_memory_manager]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &temporary_memory_manager = TemporaryMemoryManager::Get(*con.context);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE src AS SELECT i, i::VARCHAR AS s FROM range(1000000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE dst (i BIGINT, s VARCHAR)"));

	// the CTE runs the batch insert to completion before the main query streams its first chunk
	auto stream = OpenStream(con, "WITH ins AS (INSERT INTO dst FROM src) SELECT * FROM range(100000)");
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	REQUIRE(temporary_memory_manager.GetActiveStateCount() == 0);
	REQUIRE(temporary_memory_manager.GetTotalReservation() == 0);
	while (stream->Fetch()) {
	}

	auto result = con.Query("SELECT count(*) FROM dst");
	REQUIRE(CHECK_COLUMN(result, 0, {1000000}));
}
