#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {
class InMemoryTestDirectory {
public:
	InMemoryTestDirectory() : original_directory(TestGetCurrentDirectory()) {
		TestChangeDirectory(LocalTestDirectoryPath());
	}

	~InMemoryTestDirectory() {
		TestChangeDirectory(original_directory);
	}

private:
	string original_directory;
};
} // namespace

static void test_in_memory_initialization(string dbdir) {
	// Exercise the default ".tmp" path without sharing it with concurrent unittest processes.
	// Restore the working directory after the database closes, including on assertion failure.
	InMemoryTestDirectory test_directory;
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	duckdb::unique_ptr<DuckDB> db;
	duckdb::unique_ptr<Connection> con;
	string in_memory_tmp = ".tmp";

	// make sure the temporary folder does not exist
	DeleteDatabase(dbdir);
	fs->RemoveDirectory(in_memory_tmp);

	// cannot create an in-memory database using ":memory:" argument
	REQUIRE_NOTHROW(db = make_uniq<DuckDB>(dbdir));
	REQUIRE_NOTHROW(con = make_uniq<Connection>(*db));

	REQUIRE_NO_FAIL(con->Query("SET threads=1"));

	// force the in-memory directory to be created by creating a table bigger than the memory limit
	REQUIRE_NO_FAIL(con->Query("PRAGMA memory_limit='4MB'"));
	REQUIRE_NO_FAIL(con->Query("CREATE TABLE integers AS SELECT * FROM range(1000000)"));

	// the temporary folder .tmp should be created in in-memory mode
	REQUIRE(fs->DirectoryExists(in_memory_tmp));

	// the database dir should not be created in in-memory mode
	REQUIRE(!fs->DirectoryExists(dbdir));

	// clean up
	con.reset();
	db.reset();

	// make sure to clean up the database & temporary folder
	DeleteDatabase(dbdir);
	fs->RemoveDirectory(in_memory_tmp);
}

TEST_CASE("Test in-memory database initialization argument \":memory:\"", "[api][.]") {
	test_in_memory_initialization(":memory:");
}

TEST_CASE("Test in-memory database initialization argument \"\"", "[api][.]") {
	test_in_memory_initialization("");
}
