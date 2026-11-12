#include "capi_tester.hpp"
#include "duckdb/common/vector.hpp"

#include <thread>

using namespace duckdb;

class CustomLogStore {
public:
	// Concurrent insertions.
	void Insert(const string &msg) {
		m.lock();
		store.insert(msg);
		m.unlock();
	}

	// NOTE: Not concurrency-safe helper functions.

	void Reset() {
		store.clear();
	}
	bool Contains(const string &key) const {
		for (const auto &elem : store) {
			if (StringUtil::Contains(elem, key)) {
				return true;
			}
		}
		return false;
	}
	bool Find(const string &key) const {
		return store.find(key) != store.end();
	}

private:
	mutex m;
	unordered_set<string> store;
};

CustomLogStore my_log_store;

struct MyCopyFunctionExtraData {
	string mode;
};

void WriteLogEntry(void *extra_data, duckdb_timestamp *timestamp, const char *level, const char *log_type,
                   const char *log_message) {
	if (!extra_data) {
		my_log_store.Insert(log_message);
		return;
	}
	const auto data = static_cast<MyCopyFunctionExtraData *>(extra_data);
	if (StringUtil::CIEquals(data->mode, "full log")) {
		my_log_store.Insert(to_string(timestamp->micros) + ", " + level + ", " + log_type + ", " + log_message);
		return;
	}
	my_log_store.Insert(log_message);
}

TEST_CASE("Test a custom log storage in the CAPI with extra data", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	my_log_store.Reset();
	REQUIRE(tester.OpenDatabase(nullptr));

	auto my_extra_data = new MyCopyFunctionExtraData();
	my_extra_data->mode = "full log";

	auto log_storage = duckdb_create_log_storage();
	duckdb_log_storage_set_write_log_entry(log_storage, nullptr);
	duckdb_log_storage_set_write_log_entry(nullptr, WriteLogEntry);
	duckdb_log_storage_set_write_log_entry(log_storage, WriteLogEntry);
	duckdb_log_storage_set_name(log_storage, "MyCustomStorage");

	duckdb_log_storage_set_extra_data(log_storage, my_extra_data, [](void *extra_data) {
		const auto data = static_cast<MyCopyFunctionExtraData *>(extra_data);
		delete data;
	});
	auto state = duckdb_register_log_storage(tester.database, log_storage);
	REQUIRE(state == DuckDBSuccess);

	// Log storage already exists.
	state = duckdb_register_log_storage(tester.database, log_storage);
	REQUIRE(state == DuckDBError);

	REQUIRE_NO_FAIL(tester.Query("SET enable_logging = true;"));
	REQUIRE_NO_FAIL(tester.Query("SET logging_sink = 'MyCustomStorage';"));
	REQUIRE_NO_FAIL(tester.Query("SELECT write_log('HELLO, BRO');"));

	REQUIRE(my_log_store.Contains("INFO, QueryLog, SELECT write_log('HELLO, BRO');"));

	duckdb_destroy_log_storage(&log_storage);
}

TEST_CASE("Test logging silent exceptions using a custom log storage in the CAPI", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	my_log_store.Reset();
	REQUIRE(tester.OpenDatabase(nullptr));

	auto log_storage = duckdb_create_log_storage();
	duckdb_log_storage_set_write_log_entry(log_storage, WriteLogEntry);
	duckdb_log_storage_set_name(log_storage, "MyCustomStorage");
	duckdb_register_log_storage(tester.database, log_storage);

	REQUIRE_NO_FAIL(tester.Query("CALL enable_logging(level = 'error');"));
	REQUIRE_NO_FAIL(tester.Query("SET logging_sink = 'MyCustomStorage';"));

	auto path = TestCreatePath("log_storage_test.db");
	REQUIRE_NO_FAIL(tester.Query("ATTACH '" + path + "'"));
	REQUIRE_NO_FAIL(tester.Query("PRAGMA wal_autocheckpoint = '1TB';"));
	REQUIRE_NO_FAIL(tester.Query("PRAGMA debug_checkpoint_abort = 'before_header';"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE log_storage_test.integers AS SELECT * FROM range(100) tbl(i);"));

	// Shut down - should silently fail while resetting the databases.
	tester.Cleanup();

	REQUIRE(my_log_store.Contains("Checkpoint failed for database"));

	duckdb_destroy_log_storage(&log_storage);
}

void workUnit(duckdb_database db, idx_t worker_id) {
	duckdb_connection conn;
	if (duckdb_connect(db, &conn) != DuckDBSuccess) {
		return;
	}

	duckdb_result result;
	auto state = duckdb_query(conn, "PRAGMA disable_profiling;", &result);
	if (state != DuckDBSuccess) {
		return;
	}
	duckdb_destroy_result(&result);

	for (idx_t i = 0; i < 10; i++) {
		string log_msg = "worker: " + to_string(worker_id) + " iteration: " + to_string(i);
		string query = "SELECT write_log('" + log_msg + "');";

		state = duckdb_query(conn, query.c_str(), &result);
		duckdb_destroy_result(&result);
		if (state != DuckDBSuccess) {
			return;
		}
	}

	duckdb_disconnect(&conn);
}

TEST_CASE("Test a concurrent custom log storage in the CAPI", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	my_log_store.Reset();
	REQUIRE(tester.OpenDatabase(nullptr));

	auto log_storage = duckdb_create_log_storage();
	duckdb_log_storage_set_write_log_entry(log_storage, WriteLogEntry);
	duckdb_log_storage_set_name(log_storage, "MyCustomStorage");
	duckdb_register_log_storage(tester.database, log_storage);

	REQUIRE_NO_FAIL(tester.Query("PRAGMA disable_profiling;"));
	REQUIRE_NO_FAIL(tester.Query("SET enable_logging = true;"));
	REQUIRE_NO_FAIL(tester.Query("SET logging_sink = 'MyCustomStorage';"));

	duckdb::vector<std::thread> workers;
	for (idx_t worker_id = 0; worker_id < 10; worker_id++) {
		workers.emplace_back(workUnit, tester.database, worker_id);
	}
	for (auto &worker : workers) {
		worker.join();
	}

	// Ensure that all our logs are there.
	for (idx_t worker_id = 0; worker_id < 10; worker_id++) {
		for (idx_t i = 0; i < 10; i++) {
			string log_msg = "worker: " + to_string(worker_id) + " iteration: " + to_string(i);
			REQUIRE(my_log_store.Find(log_msg));
		}
	}

	duckdb_destroy_log_storage(&log_storage);
}
