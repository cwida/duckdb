#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

// A commit is published before its WAL is durable. These tests check that a transaction starting in
// that window does not get to see it
namespace {

constexpr idx_t ROW_COUNT = 5000;

//! Parks WAL fsyncs while armed
class GatedFsyncFileSystem : public LocalFileSystem {
public:
	void FileSync(FileHandle &handle) override {
		if (StringUtil::Contains(handle.GetPath(), ".wal")) {
			unique_lock<mutex> guard(lock);
			if (armed) {
				parked = true;
				cv.notify_all();
				cv.wait(guard, [&]() { return released; });
			}
		}
		LocalFileSystem::FileSync(handle);
	}
	void Arm() {
		lock_guard<mutex> guard(lock);
		armed = true;
		parked = false;
		released = false;
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
	mutex lock;
	std::condition_variable cv;
	bool armed = false;
	bool parked = false;
	bool released = false;
};

struct GatedDatabase {
	explicit GatedDatabase(const string &name) {
		auto path = TestCreatePath(name);
		DeleteDatabase(path);
		auto fs = make_uniq<GatedFsyncFileSystem>();
		gate = fs.get();
		DBConfig config;
		config.file_system = make_uniq<VirtualFileSystem>(std::move(fs));
		db = make_uniq<DuckDB>(path, &config);
	}
	~GatedDatabase() {
		FinishWriter();
	}

	void StartWriter(const string &query) {
		// keep another transaction open, or the writer syncs under the transaction lock
		holder = make_uniq<Connection>(*db);
		REQUIRE_NO_FAIL(holder->Query("BEGIN"));
		REQUIRE_NO_FAIL(holder->Query("SELECT count(*) FROM t"));
		gate->Arm();
		writer = std::thread([this, query]() {
			Connection con(*db);
			writer_failed = con.Query(query)->HasError();
		});
		REQUIRE(gate->WaitUntilParked());
	}
	bool FinishWriter() {
		gate->Release();
		if (writer.joinable()) {
			writer.join();
		}
		if (holder) {
			holder->Query("ROLLBACK");
			holder.reset();
		}
		return !writer_failed;
	}

	unique_ptr<DuckDB> db;
	GatedFsyncFileSystem *gate;
	unique_ptr<Connection> holder;
	std::thread writer;
	bool writer_failed = false;
};

idx_t ScalarValue(Connection &con, const string &query) {
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0).GetValue<idx_t>();
}

} // namespace

TEST_CASE("A commit pending durability is not visible to a transaction that starts after it", "[api][group_commit]") {
	GatedDatabase gated("group_commit_bound.db");
	auto &db = *gated.db;

	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("SET checkpoint_threshold='1TB'"));
	REQUIRE_NO_FAIL(setup.Query("PRAGMA disable_checkpoint_on_shutdown"));
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER)"));
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE scratch(i INTEGER)"));

	gated.StartWriter("INSERT INTO t SELECT * FROM range(" + to_string(ROW_COUNT) + ")");

	Connection reader(db);
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	REQUIRE(ScalarValue(reader, "SELECT count(*) FROM t") == 0);

	// the rollback runs the writer's queued cleanup, which must keep the pending rows hidden
	Connection drainer(db);
	REQUIRE_NO_FAIL(drainer.Query("BEGIN"));
	REQUIRE_NO_FAIL(drainer.Query("INSERT INTO scratch VALUES (1)"));
	REQUIRE_NO_FAIL(drainer.Query("ROLLBACK"));

	REQUIRE(ScalarValue(reader, "SELECT count(*) FROM t") == 0);
	REQUIRE_NO_FAIL(reader.Query("COMMIT"));

	REQUIRE(gated.FinishWriter());

	REQUIRE(ScalarValue(setup, "SELECT count(*) FROM t") == ROW_COUNT);
}

TEST_CASE("A bounded transaction conflicts with the commit it cannot see", "[api][group_commit]") {
	GatedDatabase gated("group_commit_bound_conflict.db");
	auto &db = *gated.db;

	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("SET checkpoint_threshold='1TB'"));
	REQUIRE_NO_FAIL(setup.Query("PRAGMA disable_checkpoint_on_shutdown"));
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER, v INTEGER)"));
	REQUIRE_NO_FAIL(setup.Query("INSERT INTO t VALUES (1, 0)"));

	gated.StartWriter("UPDATE t SET v = 1 WHERE i = 1");

	// cannot see the update, but must still conflict with it
	Connection writer2(db);
	REQUIRE_NO_FAIL(writer2.Query("BEGIN"));
	REQUIRE(ScalarValue(writer2, "SELECT v FROM t WHERE i = 1") == 0);
	auto conflict = writer2.Query("UPDATE t SET v = 2 WHERE i = 1");
	REQUIRE(conflict->HasError());
	REQUIRE_NO_FAIL(writer2.Query("ROLLBACK"));

	REQUIRE(gated.FinishWriter());

	REQUIRE(ScalarValue(setup, "SELECT v FROM t WHERE i = 1") == 1);
}

TEST_CASE("txid_current stays unique while commits are pending durability", "[api][group_commit]") {
	GatedDatabase gated("group_commit_bound_txid.db");
	auto &db = *gated.db;

	Connection setup(db);
	REQUIRE_NO_FAIL(setup.Query("SET checkpoint_threshold='1TB'"));
	REQUIRE_NO_FAIL(setup.Query("PRAGMA disable_checkpoint_on_shutdown"));
	REQUIRE_NO_FAIL(setup.Query("CREATE TABLE t(i INTEGER)"));

	gated.StartWriter("INSERT INTO t SELECT * FROM range(" + to_string(ROW_COUNT) + ")");

	// same visibility bound, distinct ids
	Connection first(db);
	Connection second(db);
	REQUIRE_NO_FAIL(first.Query("BEGIN"));
	REQUIRE_NO_FAIL(second.Query("BEGIN"));
	REQUIRE(ScalarValue(first, "SELECT count(*) FROM t") == 0);
	REQUIRE(ScalarValue(second, "SELECT count(*) FROM t") == 0);
	REQUIRE(ScalarValue(first, "SELECT txid_current()") != ScalarValue(second, "SELECT txid_current()"));
	REQUIRE_NO_FAIL(first.Query("COMMIT"));
	REQUIRE_NO_FAIL(second.Query("COMMIT"));

	REQUIRE(gated.FinishWriter());
}
