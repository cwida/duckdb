#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

// Small data payloads fit in one partial block, whose first write occurs after preparation but before publication.
// A filesystem gate pauses that write to test concurrent inserts/deletes, rollback, and old-reader visibility.
// The full-checkpoint case checks that vacuum exclusion protects remapped indexes until table publication.
// Each scenario either completes the write or injects an I/O failure, then verifies the data after reopening.
// The gate controls the publication window without relying on sleep-based scheduling.
namespace {

class GatedCheckpointFileSystem : public LocalFileSystem {
public:
	explicit GatedCheckpointFileSystem(string database_name_p) : database_name(std::move(database_name_p)) {
	}

	using LocalFileSystem::Write;

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		bool fail_write = false;
		if (StringUtil::EndsWith(handle.GetPath(), database_name)) {
			unique_lock<mutex> guard(lock);
			if (armed) {
				armed = false;
				parked = true;
				cv.notify_all();
				if (!cv.wait_for(guard, std::chrono::seconds(60), [&]() { return released; })) {
					throw IOException("Timed out waiting to release checkpoint write");
				}
				fail_write = fail_checkpoint;
			}
		}
		if (fail_write) {
			throw IOException("Injected deferred checkpoint write failure");
		}
		LocalFileSystem::Write(handle, buffer, nr_bytes, location);
	}

	void Arm(bool fail) {
		lock_guard<mutex> guard(lock);
		armed = true;
		parked = false;
		released = false;
		fail_checkpoint = fail;
	}

	bool WaitUntilParked() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(60), [&]() { return parked; });
	}

	void Release() {
		{
			lock_guard<mutex> guard(lock);
			released = true;
			armed = false;
		}
		cv.notify_all();
	}

private:
	string database_name;
	mutex lock;
	std::condition_variable cv;
	bool armed = false;
	bool parked = false;
	bool released = false;
	bool fail_checkpoint = false;
};

struct GatedCheckpointDatabase {
	explicit GatedCheckpointDatabase(const string &name, bool small_row_groups_p = false)
	    : path(TestCreatePath(name)), small_row_groups(small_row_groups_p) {
		DeleteDatabase(path);
		auto fs = make_uniq<GatedCheckpointFileSystem>(name);
		gate = *fs;
		DBConfig config;
		config.file_system = make_uniq<VirtualFileSystem>(std::move(fs));
		if (small_row_groups) {
			db = make_uniq<DuckDB>(nullptr, &config);
			Connection con(*db);
			REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS data (ROW_GROUP_SIZE 2048, STORAGE_VERSION 'latest')"));
		} else {
			db = make_uniq<DuckDB>(path, &config);
		}
	}

	~GatedCheckpointDatabase() {
		FinishCheckpoint();
	}

	void StartCheckpoint(bool fail, const string &query = "CHECKPOINT") {
		gate->Arm(fail);
		checkpoint_thread = std::thread([this, query]() {
			Connection con(*db);
			checkpoint_result = con.Query(query);
		});
		REQUIRE(gate->WaitUntilParked());
	}

	void FinishCheckpoint() {
		if (gate) {
			gate->Release();
		}
		if (checkpoint_thread.joinable()) {
			checkpoint_thread.join();
		}
	}

	void Reopen() {
		FinishCheckpoint();
		checkpoint_result.reset();
		gate = nullptr;
		db.reset();
		if (small_row_groups) {
			db = make_uniq<DuckDB>(nullptr);
			Connection con(*db);
			REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS data (ROW_GROUP_SIZE 2048, STORAGE_VERSION 'latest')"));
		} else {
			db = make_uniq<DuckDB>(path);
		}
	}

	string path;
	bool small_row_groups;
	unique_ptr<DuckDB> db;
	optional_ptr<GatedCheckpointFileSystem> gate;
	std::thread checkpoint_thread;
	unique_ptr<QueryResult> checkpoint_result;
};

struct CheckpointReleaseGuard {
	GatedCheckpointDatabase &database;

	~CheckpointReleaseGuard() {
		database.FinishCheckpoint();
	}
};

void RequireScalar(Connection &con, const string &sql, int64_t expected) {
	INFO(sql);
	auto result = con.Query(sql);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == expected);
}

void VerifyCheckpointRows(Connection &con) {
	for (const auto &table : {"a", "b"}) {
		const string name(table);
		RequireScalar(con, "SELECT count(*) FROM " + name, 27);
		RequireScalar(con, "SELECT sum(k) FROM " + name, 810);
		RequireScalar(con, "SELECT sum(v) FROM " + name, 8100);
		RequireScalar(con, "SELECT count(DISTINCT rowid) FROM " + name, 27);
		RequireScalar(con, "SELECT count(*) FROM " + name + " WHERE k >= 100", 4);
		RequireScalar(con, "SELECT count(*) FROM " + name + " WHERE k = 0 OR k BETWEEN 8 AND 15", 0);
		RequireScalar(con, "SELECT v FROM " + name + " WHERE k = 101", 1010);
	}
	REQUIRE_FAIL(con.Query("INSERT INTO a VALUES (101, 1010)"));
}

} // namespace

TEST_CASE("Deferred checkpoint publication preserves concurrent writes and recovers failed flushes",
          "[api][storage][checkpoint]") {
	const bool fail_checkpoint = GENERATE(false, true);
	GatedCheckpointDatabase database("deferred_checkpoint_publication.db");
	{
		Connection con(*database.db);
		Connection reader(*database.db);
		REQUIRE_NO_FAIL(con.Query("SET threads=1"));
		REQUIRE_NO_FAIL(con.Query("SET checkpoint_threshold='1TB'"));
		REQUIRE_NO_FAIL(con.Query("SET force_compression='uncompressed'"));
		REQUIRE_NO_FAIL(con.Query("SET debug_verify_blocks=true"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(k BIGINT PRIMARY KEY, v BIGINT)"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE b(k BIGINT, v BIGINT)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a SELECT range, range*10 FROM range(32)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO b SELECT range, range*10 FROM range(32)"));

		REQUIRE_NO_FAIL(reader.Query("BEGIN"));
		RequireScalar(reader, "SELECT count(*) FROM a", 32);
		RequireScalar(reader, "SELECT count(*) FROM b", 32);
		REQUIRE_NO_FAIL(con.Query("DELETE FROM a WHERE k=0"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM b WHERE k=0"));

		CheckpointReleaseGuard release_guard {database};
		// All payloads fit in one partial block. Its first write occurs after both tables were prepared.
		database.StartCheckpoint(fail_checkpoint);
		for (const auto &table : {"a", "b"}) {
			const string name(table);
			RequireScalar(
			    con, "SELECT count(*) FROM pragma_storage_info('" + name + "') WHERE persistent AND block_id >= 0", 0);
			REQUIRE_NO_FAIL(con.Query("DELETE FROM " + name + " WHERE k BETWEEN 8 AND 15"));
			REQUIRE_NO_FAIL(con.Query("INSERT INTO " + name + " SELECT range, range*10 FROM range(100,104)"));
		}

		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM a WHERE k=1"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM b WHERE k=1"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (500, 5000)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO b VALUES (500, 5000)"));
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
		VerifyCheckpointRows(con);
		RequireScalar(reader, "SELECT count(*) FROM a", 32);
		RequireScalar(reader, "SELECT sum(k) FROM a", 496);

		database.FinishCheckpoint();
		REQUIRE(database.checkpoint_result);
		if (fail_checkpoint) {
			REQUIRE(database.checkpoint_result->HasError());
			REQUIRE(StringUtil::Contains(database.checkpoint_result->GetError(),
			                             "Injected deferred checkpoint write failure"));
		} else {
			REQUIRE_NO_FAIL(*database.checkpoint_result);
			VerifyCheckpointRows(con);
			RequireScalar(reader, "SELECT count(*) FROM a", 32);
			RequireScalar(reader, "SELECT sum(k) FROM a", 496);
			REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));
		}
	}

	// Reopen without a shutdown checkpoint to exercise WAL recovery of the concurrent commits.
	database.Reopen();
	{
		Connection con(*database.db);
		VerifyCheckpointRows(con);
		REQUIRE_NO_FAIL(con.Query("SET debug_verify_blocks=true"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	database.Reopen();
	Connection con(*database.db);
	VerifyCheckpointRows(con);
}

TEST_CASE("Full checkpoint defers remapped collections under vacuum exclusion", "[api][storage][checkpoint]") {
	const bool fail_checkpoint = GENERATE(false, true);
	CAPTURE(fail_checkpoint);
	GatedCheckpointDatabase database("deferred_checkpoint_remap.db", true);
	{
		Connection con(*database.db);
		REQUIRE_NO_FAIL(con.Query("USE data"));
		REQUIRE_NO_FAIL(con.Query("SET threads=1"));
		REQUIRE_NO_FAIL(con.Query("SET checkpoint_threshold='1TB'"));
		REQUIRE_NO_FAIL(con.Query("SET force_compression='uncompressed'"));
		REQUIRE_NO_FAIL(con.Query("SET debug_verify_blocks=true"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(k INTEGER PRIMARY KEY, v INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a SELECT range, range*10 FROM range(2048)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT data"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a SELECT range, range*10 FROM range(2048,3048)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT data"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM a WHERE k < 1000"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE b(k INTEGER REFERENCES a(k), v INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO b SELECT range, range*10 FROM range(1000,1032)"));

		CheckpointReleaseGuard release_guard {database};
		database.StartCheckpoint(fail_checkpoint, "CHECKPOINT data");

		// A table scan still sees the old layout. The filter inside SUM does not request an index probe.
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		RequireScalar(con, "SELECT sum(rowid) FILTER (WHERE k=2048) FROM a", 2048);
		auto &attached = Catalog::GetCatalog(*con.context, "data").GetAttached();
		auto &transaction_manager = DuckTransactionManager::Get(attached);
		REQUIRE(!transaction_manager.TryGetVacuumLock());
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));

		database.FinishCheckpoint();
		REQUIRE(database.checkpoint_result);
		if (fail_checkpoint) {
			REQUIRE(database.checkpoint_result->HasError());
			REQUIRE(StringUtil::Contains(database.checkpoint_result->GetError(),
			                             "Injected deferred checkpoint write failure"));
		} else {
			REQUIRE_NO_FAIL(*database.checkpoint_result);
			RequireScalar(con, "SELECT rowid FROM a WHERE k=2048", 1048);
			RequireScalar(con, "SELECT count(DISTINCT row_group_id) FROM pragma_storage_info('a')", 1);
			RequireScalar(con, "SELECT sum(v) FROM b", 324960);
		}
	}

	database.Reopen();
	{
		Connection con(*database.db);
		REQUIRE_NO_FAIL(con.Query("USE data"));
		RequireScalar(con, "SELECT count(*) FROM a", 2048);
		RequireScalar(con, "SELECT sum(k) FROM a", 4144128);
		RequireScalar(con, "SELECT rowid FROM a WHERE k=2048", fail_checkpoint ? 2048 : 1048);
		RequireScalar(con, "SELECT sum(v) FROM b", 324960);
		REQUIRE_FAIL(con.Query("INSERT INTO a VALUES (2048, 0)"));
		REQUIRE_NO_FAIL(con.Query("SET debug_verify_blocks=true"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	database.Reopen();
	Connection con(*database.db);
	REQUIRE_NO_FAIL(con.Query("USE data"));
	RequireScalar(con, "SELECT rowid FROM a WHERE k=2048", 1048);
	RequireScalar(con, "SELECT count(*) FROM a", 2048);
	RequireScalar(con, "SELECT sum(v) FROM b", 324960);
}
