#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: LogSink. A log sink of our own, selected with
// SET logging_storage, receiving the entries DuckDB logs.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

constexpr const char *CPP_LOG_TYPE = "CppLogSinkTest";

struct CollectedEntry {
	std::string message;
	LogLevel level;
	LogScope scope;
	bool has_connection_id;
	int64_t micros;
};

// What the sink saw, carried in its user data.
struct Collected {
	std::vector<CollectedEntry> entries;
	size_t flushes = 0;
	bool throw_on_write = false;
};

// The collection of the registered sink, latched so the test can read it after the user data moved into it.
Collected *registered_collected = nullptr;

void Collect(LogSink::Entry &entry) {
	auto &collected = entry.GetUserData<Collected>();
	registered_collected = &collected;
	if (collected.throw_on_write) {
		throw InvalidInputException("the sink is broken");
	}
	if (entry.GetLogType() != CPP_LOG_TYPE) {
		return;
	}
	collected.entries.push_back({std::string(entry.GetLogMessage()), entry.GetLevel(), entry.GetScope(),
	                             entry.GetConnectionId().has_value(), entry.GetTimestamp().micros});
}

void CountFlush(LogSink::FlushInput &input) {
	input.GetUserData<Collected>().flushes++;
}

} // namespace

TEST_CASE("Stable C++API: log sink receives entries", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto sink = LogSink::Create(db.GetFactory());
	sink.SetName("cpp_store").SetWriteCallback(Collect).SetFlushCallback(CountFlush).SetUserData<Collected>();
	db.Register(sink);

	registered_collected = nullptr;
	conn.Execute("CALL enable_logging()").Drain();
	conn.Execute("SET logging_storage = 'CPP_STORE'").Drain();
	conn.Execute("SELECT write_log('hello', level := 'warning', log_type := 'CppLogSinkTest')").Drain();
	db.Log(LogLevel::LOG_ERROR, "from the instance", CPP_LOG_TYPE);

	REQUIRE(registered_collected != nullptr);
	auto &entries = registered_collected->entries;
	REQUIRE(entries.size() == 2);
	REQUIRE(entries[0].message == "hello");
	REQUIRE(entries[0].level == LogLevel::LOG_WARNING);
	REQUIRE(entries[0].scope == LogScope::CONNECTION);
	REQUIRE(entries[0].has_connection_id);
	REQUIRE(entries[0].micros > 0);
	REQUIRE(entries[1].message == "from the instance");
	REQUIRE(entries[1].scope == LogScope::DATABASE);
	REQUIRE_FALSE(entries[1].has_connection_id);
	// The end of every query flushes the log.
	REQUIRE(registered_collected->flushes > 0);

	conn.Execute("SET logging_storage = 'memory'").Drain();
}

TEST_CASE("Stable C++API: log sink errors", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// A throwing write callback drops the entry, and the query that logged still succeeds.
	{
		auto sink = LogSink::Create(db.GetFactory());
		Collected broken;
		broken.throw_on_write = true;
		sink.SetName("cpp_broken").SetWriteCallback(Collect).SetUserData<Collected>(broken);
		db.Register(sink);
		registered_collected = nullptr;
		conn.Execute("CALL enable_logging()").Drain();
		conn.Execute("SET logging_storage = 'cpp_broken'").Drain();
		conn.Execute("SELECT write_log('lost', log_type := 'CppLogSinkTest')").Drain();
		REQUIRE(registered_collected != nullptr);
		REQUIRE(registered_collected->entries.empty());
		conn.Execute("SET logging_storage = 'memory'").Drain();
	}
	// No write callback.
	{
		auto sink = LogSink::Create(db.GetFactory());
		sink.SetName("cpp_no_callback");
		REQUIRE_THROWS_MATCHES(db.Register(sink), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// A built-in name.
	{
		auto sink = LogSink::Create(db.GetFactory());
		sink.SetName("stdout").SetWriteCallback(Collect);
		REQUIRE_THROWS_MATCHES(db.Register(sink), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}
