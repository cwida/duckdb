#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "test_helpers.hpp"

#include <cstdio>

using namespace duckdb;

namespace {

// A read chunk of the parallel NDJSON scanner holds (maximum_object_size - YYJSON_PADDING_SIZE)
// bytes, i.e. 16 MiB - 4 by default - the user can only increase maximum_object_size, so this is
// the smallest chunk size we can get. We write a file of chunk_size + 4 bytes: the final read
// chunk then contains only the 4-byte tail of an unterminated last record, which used to make the
// parallel scan lose that record (issue #26787).
constexpr idx_t CHUNK_SIZE = 16 * 1024 * 1024 - 4;
constexpr idx_t LAST_RECORD_SIZE = 200;

string MakeJSONRecord(idx_t size) {
	// {"pad":"xxx..."} - size includes the braces and quotes
	string rec = "{\"pad\":\"";
	rec += string(size - rec.size() - 2, 'x');
	rec += "\"}";
	return rec;
}

void WriteUnterminatedLastLineFile(const string &path) {
	const string last_record = MakeJSONRecord(LAST_RECORD_SIZE);
	// total size: one full chunk + the first 4 bytes of the last record
	idx_t body_size = CHUNK_SIZE + 4 - last_record.size();
	string filler = MakeJSONRecord(99); // 99 bytes + newline = 100 bytes per line
	FILE *f = fopen(path.c_str(), "wb");
	REQUIRE(f);
	idx_t written = 0;
	while (body_size - written >= 121) {
		fwrite(filler.c_str(), 1, filler.size(), f);
		fputc('\n', f);
		written += filler.size() + 1;
	}
	// final filler record absorbs the remaining bytes (>= 21) so the last record starts inside
	// the first chunk and only 4 of its bytes land in the last read chunk
	string final_filler = MakeJSONRecord(body_size - written - 1);
	fwrite(final_filler.c_str(), 1, final_filler.size(), f);
	fputc('\n', f);
	// the last record is deliberately NOT newline-terminated
	fwrite(last_record.c_str(), 1, last_record.size(), f);
	fclose(f);
}

} // namespace

TEST_CASE("Parallel NDJSON scan with unterminated last line split across chunks (#26787)", "[json]") {
	const auto path = TestCreatePath("ndjson_unterminated_last_line.jsonl");
	WriteUnterminatedLastLineFile(path);
	// expected row count: every line is 100 bytes except the 200-byte last record
	const int64_t expected_rows = static_cast<int64_t>((CHUNK_SIZE + 4 - LAST_RECORD_SIZE) / 100) + 1;

	DuckDB db(nullptr);
	Connection con(db);
	// skip quietly if this build does not have the JSON extension
	{
		auto ext = con.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'json' AND loaded");
		if (ext->HasError() || ext->GetValue<int64_t>(0, 0) == 0) {
			return;
		}
	}
	con.Query("SET threads=4");

	SECTION("strict mode errors out on a nonexistent line") {
		auto result = con.Query("SELECT count(*) FROM read_json('" + path + "', format='newline_delimited')");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetValue<int64_t>(0, 0) == expected_rows);
	}

	SECTION("ignore_errors keeps the last record instead of returning NULL") {
		auto result = con.Query("SELECT count(pad) FROM read_json('" + path +
		                        "', format='newline_delimited', ignore_errors=true)");
		REQUIRE_NO_FAIL(*result);
		// all rows must be non-NULL - the last record used to be silently replaced by NULL
		REQUIRE(result->GetValue<int64_t>(0, 0) == expected_rows);
	}

	SECTION("single-threaded scan still works") {
		con.Query("SET threads=1");
		auto result = con.Query("SELECT count(*) FROM read_json('" + path + "', format='newline_delimited')");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetValue<int64_t>(0, 0) == expected_rows);
	}

	SECTION("newline-terminated file of the same size is unaffected") {
		FILE *f = fopen(path.c_str(), "ab");
		REQUIRE(f);
		fputc('\n', f);
		fclose(f);
		auto result = con.Query("SELECT count(*) FROM read_json('" + path + "', format='newline_delimited')");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetValue<int64_t>(0, 0) == expected_rows);
	}

	TestDeleteFile(path);
}
