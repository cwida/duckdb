#include "test_capi_v2.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 log sink tests: register a log sink of our own, select it with
// SET logging_storage, and observe the entries its write callback receives.
//
// Callbacks avoid Catch assertions: they run inside DuckDB, under its log
// lock. They record what they see, and the test asserts on it afterwards.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

constexpr const char *TEST_LOG_TYPE = "V2LogSinkTest";

struct StoredEntry {
	int64_t timestamp = 0;
	DUCKDB_V2_LOG_LEVEL level = DUCKDB_V2_LOG_LEVEL_TRACE;
	std::string type;
	std::string message;
	DUCKDB_V2_LOG_SCOPE scope = DUCKDB_V2_LOG_SCOPE_DATABASE;
	bool has_connection_id = false;
	bool has_thread_id = false;
};

// What the sink saw, carried in its user data.
struct Store {
	std::vector<StoredEntry> entries;
	idx_t flushes = 0;
	bool *destroyed = nullptr;
	bool fail_writes = false;
};

void DestroyStore(void *ptr) {
	auto store = static_cast<Store *>(ptr);
	if (store->destroyed) {
		*store->destroyed = true;
	}
	delete store;
}

void StoreWrite(duckdb_v2_log_sink_write_info_handle info, void *user_data, int64_t timestamp,
                DUCKDB_V2_LOG_LEVEL level, DUCKDB_V2_LOG_SCOPE scope, const duckdb_v2_str *type,
                const duckdb_v2_str *message, duckdb_v2_error_info_handle *err) {
	auto &store = *static_cast<Store *>(user_data);
	if (store.fail_writes) {
		duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
		return;
	}
	StoredEntry stored;
	stored.timestamp = timestamp;
	stored.level = level;
	stored.scope = scope;
	stored.type = Convert(*type);
	stored.message = Convert(*message);
	idx_t id = 0;
	if (duckdb_v2_log_sink_write_info_get_connection_id(info, &id, &stored.has_connection_id, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_log_sink_write_info_get_thread_id(info, &id, &stored.has_thread_id, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	store.entries.push_back(std::move(stored));
}

void StoreFlush(duckdb_v2_log_sink_flush_info_handle, void *user_data, duckdb_v2_error_info_handle *) {
	static_cast<Store *>(user_data)->flushes++;
}

// A sink collecting into a fresh Store, which the registered sink owns.
duckdb_v2_log_sink_handle MakeSink(duckdb_v2_factory_handle factory, const char *name, Store *store) {
	duckdb_v2_log_sink_handle sink = nullptr;
	REQUIRE(duckdb_v2_log_sink_create(factory, &sink, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (name) {
		auto name_str = Convert(name);
		REQUIRE(duckdb_v2_log_sink_set_name(sink, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_log_sink_set_write_callback(sink, StoreWrite, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_log_sink_set_flush_callback(sink, StoreFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (store) {
		duckdb_v2_opaque data = {store, DestroyStore, nullptr};
		REQUIRE(duckdb_v2_log_sink_set_user_data(sink, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	return sink;
}

DUCKDB_V2_ERROR RegisterSink(duckdb_v2_instance_handle instance, duckdb_v2_log_sink_handle sink) {
	auto rc = duckdb_v2_instance_register_log_sink(instance, sink, nullptr);
	duckdb_v2_log_sink_destroy(&sink);
	return rc;
}

void LogTest(duckdb_v2_connection_handle conn, const char *message) {
	auto type = Convert(TEST_LOG_TYPE);
	auto message_str = Convert(message);
	REQUIRE(duckdb_v2_connection_log(conn, DUCKDB_V2_LOG_LEVEL_WARNING, &type, &message_str, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
}

// The entries of our own log type, as the store saw them.
std::vector<StoredEntry> TestEntries(const Store &store) {
	std::vector<StoredEntry> result;
	for (auto &entry : store.entries) {
		if (entry.type == TEST_LOG_TYPE) {
			result.push_back(entry);
		}
	}
	return result;
}

} // namespace

TEST_CASE("V2 log sink: entries reach the write callback", "[capi_v2][log_sink]") {
	EnvFixture fx;
	auto store = new Store();
	REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "v2_store", store)) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "CALL enable_logging()") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET logging_storage = 'v2_store'") == DUCKDB_V2_ERROR_NONE);

	LogTest(fx.conn, "from a connection");
	auto type = Convert(TEST_LOG_TYPE);
	auto message = Convert("from the instance");
	REQUIRE(duckdb_v2_instance_log(fx.instance, DUCKDB_V2_LOG_LEVEL_ERROR, &type, &message, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	auto entries = TestEntries(*store);
	REQUIRE(entries.size() == 2);
	REQUIRE(entries[0].message == "from a connection");
	REQUIRE(entries[0].level == DUCKDB_V2_LOG_LEVEL_WARNING);
	REQUIRE(entries[0].scope == DUCKDB_V2_LOG_SCOPE_CONNECTION);
	REQUIRE(entries[0].has_connection_id);
	REQUIRE(entries[0].timestamp > 0);
	REQUIRE(entries[1].message == "from the instance");
	REQUIRE(entries[1].level == DUCKDB_V2_LOG_LEVEL_ERROR);
	REQUIRE(entries[1].scope == DUCKDB_V2_LOG_SCOPE_DATABASE);
	REQUIRE_FALSE(entries[1].has_connection_id);
	REQUIRE_FALSE(entries[1].has_thread_id);

	// Entries written from SQL arrive too.
	REQUIRE(RunSql(fx.conn, "SELECT write_log('from SQL', level := 'info', log_type := 'V2LogSinkTest')") ==
	        DUCKDB_V2_ERROR_NONE);
	entries = TestEntries(*store);
	REQUIRE(entries.size() == 3);
	REQUIRE(entries[2].message == "from SQL");
	REQUIRE(entries[2].level == DUCKDB_V2_LOG_LEVEL_INFO);
	REQUIRE(entries[2].scope == DUCKDB_V2_LOG_SCOPE_CONNECTION);
	REQUIRE(entries[2].has_connection_id);

	// A log sink only receives entries: they cannot be read back through duckdb_logs.
	REQUIRE(RunSql(fx.conn, "SELECT * FROM duckdb_logs") != DUCKDB_V2_ERROR_NONE);

	// Currently, selecting another sink flushes this one first.
	auto flushes = store->flushes;
	REQUIRE(RunSql(fx.conn, "SET logging_storage = 'memory'") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(store->flushes > flushes);
	LogTest(fx.conn, "not for the store");
	REQUIRE(TestEntries(*store).size() == 3);
}

TEST_CASE("V2 log sink: names match case-insensitively", "[capi_v2][log_sink]") {
	EnvFixture fx;
	auto store = new Store();
	REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "Mixed_Case_Store", store)) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "CALL enable_logging()") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET logging_storage = 'MIXED_case_STORE'") == DUCKDB_V2_ERROR_NONE);
	LogTest(fx.conn, "found");
	REQUIRE(TestEntries(*store).size() == 1);
	REQUIRE(RunSql(fx.conn, "SET logging_storage = 'memory'") == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 log sink: a failing write callback never fails the logging operation", "[capi_v2][log_sink]") {
	EnvFixture fx;
	auto store = new Store();
	store->fail_writes = true;
	REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "failing_store", store)) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "CALL enable_logging()") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET logging_storage = 'failing_store'") == DUCKDB_V2_ERROR_NONE);

	LogTest(fx.conn, "dropped");
	REQUIRE(RunSql(fx.conn, "SELECT 42") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(store->entries.empty());
	REQUIRE(RunSql(fx.conn, "SET logging_storage = 'memory'") == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 log sink: the user data lives as long as the instance", "[capi_v2][log_sink]") {
	bool destroyed = false;
	{
		EnvFixture fx;
		auto store = new Store();
		store->destroyed = &destroyed;
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "owned_store", store)) == DUCKDB_V2_ERROR_NONE);
		// Destroying the handle does not affect the registered sink.
		REQUIRE_FALSE(destroyed);
	}
	REQUIRE(destroyed);

	// An unregistered sink frees its user data with the handle.
	destroyed = false;
	{
		EnvFixture fx;
		auto store = new Store();
		store->destroyed = &destroyed;
		auto sink = MakeSink(fx.factory, "never_registered", store);
		duckdb_v2_log_sink_destroy(&sink);
		REQUIRE(destroyed);
	}
}

TEST_CASE("V2 log sink: registration validates the sink", "[capi_v2][log_sink]") {
	EnvFixture fx;

	SECTION("a name is required") {
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, nullptr, nullptr)) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("a write callback is required") {
		duckdb_v2_log_sink_handle sink = nullptr;
		REQUIRE(duckdb_v2_log_sink_create(fx.factory, &sink, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto name = Convert("no_callback");
		REQUIRE(duckdb_v2_log_sink_set_name(sink, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(RegisterSink(fx.instance, sink) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("built-in names are taken") {
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "memory", nullptr)) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "STDOUT", nullptr)) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "file", nullptr)) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("a name can only be registered once") {
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "taken", nullptr)) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(RegisterSink(fx.instance, MakeSink(fx.factory, "TAKEN", nullptr)) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("a sink registers only on its factory's instance") {
		duckdb_v2_instance_handle other = nullptr;
		REQUIRE(OpenInstance(fx.env, duckdb_v2_str {nullptr, 0}, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(RegisterSink(other, MakeSink(fx.factory, "foreign", nullptr)) == DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_instance_destroy(&other);
	}
	SECTION("null handles are rejected, and destroy is null-safe") {
		REQUIRE(duckdb_v2_instance_register_log_sink(fx.instance, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_log_sink_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_log_sink_handle sink = nullptr;
		REQUIRE(duckdb_v2_log_sink_destroy(&sink) == DUCKDB_V2_ERROR_NONE);
	}
}

} // namespace test_capi_v2
