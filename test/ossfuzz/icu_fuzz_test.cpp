// Fuzzer for DuckDB's ICU extension (collations, time zones, calendars,
// date formatting) -- 45K lines that the existing SQL harness cannot reach
// because the oss-fuzz build does not build/load ICU today.
//
// Design
// ------
// The statically-linked ICU extension is force-loaded into a single reused
// in-memory database (fresh instances cost ~ms and starve corpus growth).
// The first input byte selects a code path; the payload feeds strings into
// ICU entry points that take untrusted names/formats:
//   0: raw SQL batch (collation / timezone SQL comes from the corpus)
//   1: strftime() with a fuzzer-controlled format string
//   2: string comparison under a fuzzer-controlled ICU collation
//   3: SET TimeZone / AT TIME ZONE with a fuzzer-controlled zone name,
//      plus calendar-aware date_part
// Errors are expected (bad formats/zones raise DuckDB exceptions); only
// memory-safety faults, sanitizer reports and INTERNAL errors are findings.

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "icu_extension.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_set>

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
	// ICU is linked statically (BUILD_ICU=1) but not auto-loaded; register
	// it without disk/network access. Parquet-style autoload does not apply.
	g_db->LoadStaticExtension<duckdb::IcuExtension>();
	g_con = new duckdb::Connection(*g_db);
	g_con->Query("SET threads=1");
	g_con->Query("SET enable_progress_bar=false");
	g_con->Query("SET max_expression_depth=50");
	g_con->Query("SET TimeZone='UTC'");
}

std::string SqlString(const uint8_t *data, size_t size, size_t max_len) {
	std::string escaped;
	escaped.reserve(size * 2 + 2);
	escaped += '\'';
	for (size_t i = 0; i < size && i < max_len; i++) {
		if (data[i] == '\'') {
			escaped += "''";
		} else if (data[i] == '\0') {
			escaped += ' ';
		} else {
			escaped += static_cast<char>(data[i]);
		}
	}
	escaped += '\'';
	return escaped;
}

// Double-quoted SQL identifier (for collation / time zone names).
std::string SqlIdentifier(const uint8_t *data, size_t size, size_t max_len) {
	std::string escaped;
	escaped.reserve(size * 2 + 2);
	escaped += '"';
	for (size_t i = 0; i < size && i < max_len; i++) {
		if (data[i] == '"') {
			escaped += "\"\"";
		} else if (data[i] == '\0') {
			escaped += ' ';
		} else {
			escaped += static_cast<char>(data[i]);
		}
	}
	escaped += '"';
	return escaped;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 4) {
		return 0;
	}

	EnsureDatabase();
	if (++g_inputs % RESET_INTERVAL == 0) {
		ResetDatabase();
		EnsureDatabase();
	}
	duckdb::Connection &con = *g_con;

	const uint8_t selector = data[0] % 4;
	const uint8_t *payload = data + 1;
	const size_t payload_size = size - 1;
	const size_t half = payload_size / 2;

	std::string sql;
	switch (selector) {
	case 0:
		sql.assign(reinterpret_cast<const char *>(payload), payload_size > 4096 ? 4096 : payload_size);
		break;
	case 1:
		// Format string is fully fuzzer-controlled (strftime家族).
		sql = "SELECT strftime(TIMESTAMP '2021-06-01 12:34:56', " + SqlString(payload, payload_size, 64) + ")";
		break;
	case 2:
		// Compare short strings under a fuzzer-controlled ICU collation.
		sql = "SELECT " + SqlString(payload, half, 16) + " COLLATE " + SqlIdentifier(payload + half, payload_size - half, 32) +
		      " < " + SqlString(payload + (half ? half - 1 : 0), payload_size - (half ? half - 1 : 0), 16);
		break;
	default: {
		// Zone name drives SET TimeZone + AT TIME ZONE + calendar part.
		const std::string zone = SqlIdentifier(payload, payload_size, 48);
		sql = "SET TimeZone=" + zone;
		try {
			auto r = con.Query(sql);
			(void)r;
		} catch (std::exception &e) {
		}
		sql = "SELECT date_part('day', TIMESTAMPTZ '2021-06-01 00:30:00+00' AT TIME ZONE " + zone + ")";
		break;
	}
	}

	std::unordered_set<std::string> internal_error_messages = {"Unoptimized Result differs from original result!",
	                                                           "INTERNAL"};
	bool internal_error = false;
	try {
		auto result = con.Query(sql);
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
		// libFuzzer ignores the harness return value; report loudly.
		ResetDatabase();
		abort();
	}
	return 0;
}