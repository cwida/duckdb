#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Disk space queries go through the database's file system", "[api]") {
	auto allowed = TestDirectoryPath();
	DuckDB db(nullptr);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	REQUIRE(fs.GetAvailableDiskSpace(allowed).IsValid());

	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET allowed_directories = ['" + allowed + "']"));
	REQUIRE_NO_FAIL(con.Query("SET enable_external_access = false"));
	// the allowed directory still answers, anything else is refused like any other file system operation
	REQUIRE(fs.GetAvailableDiskSpace(allowed).IsValid());
	REQUIRE_THROWS(fs.GetAvailableDiskSpace(fs.GetWorkingDirectory()));
}
