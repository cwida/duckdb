#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_set>

// Fuzzer for DuckDB's SQL surface: one input is a full SQL script driven
// through the whole query pipeline (parser, binder, planner, optimizer,
// executor).

namespace {

duckdb::DuckDB *g_db = nullptr;
duckdb::Connection *g_con = nullptr;
size_t g_inputs = 0;

constexpr size_t RESET_INTERVAL = 4096;

void ResetDatabase() {
	if (g_con) {
		delete g_con;
		g_con = nullptr;
	}
	if (g_db) {
		delete g_db;
		g_db = nullptr;
	}
}

void EnsureDatabase() {
	if (g_db) {
		return;
	}
	g_db = new duckdb::DuckDB(nullptr);
	g_con = new duckdb::Connection(*g_db);
	// Deterministic, bounded execution: single thread, no progress updates,
	// and a capped expression depth so deeply nested auto-generated SQL
	// cannot exhaust the C++ stack.
	g_con->Query("SET threads=1");
	g_con->Query("SET enable_progress_bar=false");
	g_con->Query("SET max_expression_depth=50");
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	std::string input(reinterpret_cast<const char *>(data), size);

	EnsureDatabase();
	if (++g_inputs % RESET_INTERVAL == 0) {
		ResetDatabase();
		EnsureDatabase();
	}

	std::unordered_set<std::string> internal_error_messages = {"Unoptimized Result differs from original result!",
	                                                           "INTERNAL"};
	bool internal_error = false;
	try {
		auto result = g_con->Query(input);
		if (result->HasError()) {
			for (auto &internal_error_message : internal_error_messages) {
				if (duckdb::StringUtil::Contains(result->GetError(), internal_error_message)) {
					internal_error = true;
					break;
				}
			}
		}
	} catch (std::exception &e) {
	}
	if (internal_error) {
		// An INTERNAL error or an optimizer/result mismatch is always a
		// DuckDB bug. libFuzzer ignores the harness return value, so the old
		// `return 1` here was silently inert; report loudly instead.
		ResetDatabase();
		abort();
	}
	return 0;
}
