#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

#ifndef DUCKDB_NO_THREADS

namespace {

//! Serves a remote-looking prefix from the local file system, so a scan over "remote" files runs without a network
class RemotePrefixFileSystem : public LocalFileSystem {
public:
	static constexpr const char *PREFIX = "s3://read-ahead-budget";

	bool CanHandleFile(const string &fpath) override {
		return StringUtil::StartsWith(fpath, PREFIX);
	}
	string GetName() const override {
		return "RemotePrefixFileSystem";
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::OpenFile(Strip(path), flags, opener);
	}
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::FileExists(Strip(filename), opener);
	}
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::DirectoryExists(Strip(directory), opener);
	}

private:
	static string Strip(const string &path) {
		return StringUtil::StartsWith(path, PREFIX) ? path.substr(strlen(PREFIX)) : path;
	}
};

} // namespace

TEST_CASE("Read-ahead does not count remote files against the file descriptor budget", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	fs.RegisterSubSystem(make_uniq<RemotePrefixFileSystem>());

	const auto dir = TestCreatePath("read_ahead_remote_budget");
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=16"));
	// pin the read-ahead backlog bound to 64 MiB x threads, independent of the host's memory
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='8GiB'"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT i % 400 AS p, i AS v FROM range(400000) t(i)) TO '" + dir +
	                          "' (FORMAT parquet, PARTITION_BY (p))"));

	// a budget of eight descriptors for the whole process: a scan of local files keeps about that many open ahead
	REQUIRE_NO_FAIL(con.Query("SET debug_file_descriptor_limit=64"));
	REQUIRE_NO_FAIL(con.Query("CALL enable_logging('FileSystem')"));

	string files;
	for (idx_t p = 0; p < 400; p++) {
		files += (p == 0 ? "" : ", ") +
		         StringUtil::Format("'%s%s/p=%llu/data_0.parquet'", RemotePrefixFileSystem::PREFIX, dir, p);
	}
	auto result = con.Query("SELECT count(*), sum(v) FROM read_parquet([" + files + "])");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 400000);
	REQUIRE(result->Collection().GetValue(1, 0).GetValue<int64_t>() == 79999800000);
	REQUIRE_NO_FAIL(con.Query("CALL disable_logging()"));

	auto peak =
	    con.Query("WITH events AS ("
	              "    SELECT timestamp, CASE op WHEN 'OPEN' THEN 1 ELSE -1 END AS delta"
	              "    FROM duckdb_logs_parsed('FileSystem')"
	              "    WHERE op IN ('OPEN', 'CLOSE') AND path LIKE '%read_ahead_remote_budget%')"
	              "SELECT max(open_handles) FROM ("
	              "    SELECT sum(delta) OVER (ORDER BY timestamp, delta ROWS UNBOUNDED PRECEDING) AS open_handles"
	              "    FROM events)");
	REQUIRE_NO_FAIL(*peak);
	const auto peak_handles = peak->Collection().GetValue(0, 0).GetValue<int64_t>();
	// counted files peak near 20 under this budget, uncounted remote files over 100 at this memory limit
	REQUIRE(peak_handles > 48);
}

#endif
