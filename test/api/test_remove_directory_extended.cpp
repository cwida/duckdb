#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/local_file_system.hpp"

using namespace duckdb;

namespace {

string ProbeDir(const string &name) {
	LocalFileSystem fs;
	return fs.JoinPath(TestDirectoryPath(), name);
}

void MakeFile(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	handle->Close();
}

} // namespace

TEST_CASE("Test RemoveDirectoryExtended in SINGLE mode", "[api]") {
	LocalFileSystem fs;
	const RemoveDirectoryOptions single {RemoveDirectoryMode::SINGLE};

	SECTION("an empty directory is removed") {
		auto dir = ProbeDir("rde_empty");
		fs.RemoveDirectory(dir);
		fs.CreateDirectory(dir);
		REQUIRE(fs.RemoveDirectoryExtended(dir, single));
		REQUIRE(!fs.DirectoryExists(dir));
	}

	SECTION("a directory holding a file is left alone, and the file survives") {
		// this is the property the caller depends on: refusing must not destroy what is inside,
		// which is exactly what the recursive RemoveDirectory would have done
		auto dir = ProbeDir("rde_file");
		fs.RemoveDirectory(dir);
		fs.CreateDirectory(dir);
		auto file = fs.JoinPath(dir, "occupied.txt");
		MakeFile(fs, file);

		REQUIRE(!fs.RemoveDirectoryExtended(dir, single));
		REQUIRE(fs.DirectoryExists(dir));
		REQUIRE(fs.FileExists(file));

		fs.RemoveDirectory(dir);
	}

	SECTION("a directory holding a subdirectory is left alone") {
		auto dir = ProbeDir("rde_subdir");
		fs.RemoveDirectory(dir);
		fs.CreateDirectory(dir);
		auto sub = fs.JoinPath(dir, "child");
		fs.CreateDirectory(sub);

		REQUIRE(!fs.RemoveDirectoryExtended(dir, single));
		REQUIRE(fs.DirectoryExists(sub));

		fs.RemoveDirectory(dir);
	}

	SECTION("a directory that is already gone reports not removed, and does not throw") {
		// teardown may race another instance that removed it first; that is not an error
		auto dir = ProbeDir("rde_absent");
		fs.RemoveDirectory(dir);
		REQUIRE(!fs.RemoveDirectoryExtended(dir, single));
		REQUIRE(!fs.DirectoryExists(dir));
	}
}
