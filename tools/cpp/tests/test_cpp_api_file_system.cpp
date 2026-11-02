#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: FileSystem and FileHandle.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

void WriteAll(FileHandle &file, const std::string &data) {
	REQUIRE(file.Write(data.data(), data.size()) == data.size());
}

std::string ReadAll(FileHandle &file, idx_t capacity) {
	std::vector<char> buffer(capacity, '\0');
	auto read = file.Read(buffer.data(), capacity);
	return std::string(buffer.data(), read);
}

} // namespace

TEST_CASE("Stable C++API: file system round-trip", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_roundtrip.bin");

	{
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE_NEW});
		WriteAll(file, "hello world");
		REQUIRE(file.Tell() == 11);
		REQUIRE(file.Size() == 11);
		file.Sync();
	}

	auto file = fs.OpenFile(path, {FileFlags::READ});
	REQUIRE(file.Size() == 11);
	REQUIRE(ReadAll(file, 32) == "hello world");
	// At the end, a read yields nothing rather than failing.
	REQUIRE(ReadAll(file, 32).empty());

	file.Seek(6);
	REQUIRE(file.Tell() == 6);
	REQUIRE(ReadAll(file, 5) == "world");
}

TEST_CASE("Stable C++API: an instance's file system", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto fs = db.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_instance.bin");
	{
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE_NEW});
		WriteAll(file, "hello");
	}
	auto file = fs.OpenFile(path, {FileFlags::READ});
	REQUIRE(ReadAll(file, 16) == "hello");
}

TEST_CASE("Stable C++API: file flags", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_flags.bin");

	{
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE});
		WriteAll(file, "hello");
	}
	{
		// FILE_CREATE leaves an existing file alone.
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE});
		REQUIRE(file.Size() == 5);
	}
	{
		// FILE_CREATE_NEW truncates it.
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE_NEW});
		REQUIRE(file.Size() == 0);
		WriteAll(file, "hi");
	}
	// EXCLUSIVE_CREATE refuses it, and leaves the contents intact.
	REQUIRE_THROWS_AS(fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE, FileFlags::EXCLUSIVE_CREATE}),
	                  Exception);
	{
		auto file = fs.OpenFile(path, {FileFlags::READ});
		REQUIRE(ReadAll(file, 32) == "hi");
	}
	{
		// APPEND writes at the end.
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::APPEND});
		WriteAll(file, "there");
	}
	{
		auto file = fs.OpenFile(path, {FileFlags::READ});
		REQUIRE(ReadAll(file, 32) == "hithere");
	}
}

TEST_CASE("Stable C++API: file system refusals", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();

	// A missing file without a create flag.
	REQUIRE_THROWS_AS(fs.OpenFile(duckdb::TestCreatePath("cpp_fs_missing.bin"), {FileFlags::READ}), Exception);
	// No capability at all.
	REQUIRE_THROWS_MATCHES(fs.OpenFile(duckdb::TestCreatePath("cpp_fs_none.bin"), {}), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	// INVALID names no behaviour.
	REQUIRE_THROWS_MATCHES(fs.OpenFile(duckdb::TestCreatePath("cpp_fs_none.bin"), {FileFlags::INVALID}), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: file handle close then destroy", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_close.bin");

	auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE_NEW});
	WriteAll(file, "data");
	file.Close();
	// The handle is still alive; its destructor runs at the end of the scope.
}

TEST_CASE("Stable C++API: positional file read and write", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_positional.bin");

	auto file =
	    fs.OpenFile(path, {FileFlags::READ, FileFlags::WRITE, FileFlags::FILE_CREATE_NEW, FileFlags::PARALLEL_ACCESS});
	WriteAll(file, "abcdefghij");
	REQUIRE(file.Tell() == 10);

	// Positional access leaves the file's own position alone.
	char buffer[4] = {0};
	file.ReadAt(buffer, 4, 2);
	REQUIRE(std::string(buffer, 4) == "cdef");
	REQUIRE(file.Tell() == 10);

	file.WriteAt("XY", 2, 4);
	REQUIRE(file.Tell() == 10);
	file.ReadAt(buffer, 4, 2);
	REQUIRE(std::string(buffer, 4) == "cdXY");

	// A short positional read is an error, not a result.
	REQUIRE_THROWS_AS(file.ReadAt(buffer, 4, 8), Exception);
}

TEST_CASE("Stable C++API: file open options", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto fs = conn.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_options.bin");

	auto options = fs.CreateOpenOptions();
	options.SetFlag(FileFlags::WRITE)
	    .SetFlag(FileFlags::FILE_CREATE_NEW)
	    .SetValue("file_size", Value::Create(factory, int64_t {4}))
	    .SetValue("made_up_option", Value::Create(factory, varchar_t("nobody-reads-this")));

	{
		auto file = fs.OpenFile(path, options);
		WriteAll(file, "data");
	}
	// One options object opens as many files as you like.
	{
		auto file = fs.OpenFile(duckdb::TestCreatePath("cpp_fs_options_second.bin"), options);
		WriteAll(file, "more");
	}
	{
		auto file = fs.OpenFile(path, {FileFlags::READ});
		REQUIRE(ReadAll(file, 32) == "data");
	}

	// Options carrying no flags cannot say whether the file is read or written.
	auto empty = fs.CreateOpenOptions();
	REQUIRE_THROWS_MATCHES(fs.OpenFile(path, empty), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: opening a file specified as a file struct", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();
	auto path = duckdb::TestCreatePath("cpp_fs_file_struct.bin");
	{
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE_NEW});
		WriteAll(file, "hello world");
	}

	auto file_struct = [&](Value option) {
		std::vector<std::pair<std::string, Value>> fields;
		fields.emplace_back("filename", Value::Create(conn.GetFactory(), varchar_t(path)));
		fields.emplace_back("validate_external_file_cache", std::move(option));
		return Value::CreateStruct(conn.GetFactory(), fields);
	};
	auto open = [&](const Value &file) {
		auto options = fs.CreateOpenOptions();
		options.SetFlag(FileFlags::READ).SetFlag(FileFlags::EXTERNAL_FILE_CACHE).SetValues(file);
		return fs.OpenFile(GetFilePath(file), options);
	};
	auto read = [&](const Value &file) {
		auto handle = open(file);
		return ReadAll(handle, 32);
	};

	// a path carries no options
	auto plain = Value::Create(conn.GetFactory(), varchar_t(path));
	REQUIRE(GetFilePath(plain) == path);
	REQUIRE(read(plain) == "hello world");

	// the other fields of a file struct are the options to open the file with - the external file cache reads this
	// one, and rejects a value that is not a BOOLEAN
	auto valid = file_struct(Value::Create(conn.GetFactory(), false));
	REQUIRE(GetFilePath(valid) == path);
	REQUIRE(read(valid) == "hello world");
	REQUIRE_THROWS_WITH(open(file_struct(Value::Create(conn.GetFactory(), varchar_t("notabool")))),
	                    Catch::Contains("validate_external_file_cache"));
	// an option set to NULL was not specified
	auto null_option = file_struct(Value::CreateNull(conn.GetFactory(), conn.GetFactory().ParseType("BOOLEAN")));
	REQUIRE(read(null_option) == "hello world");

	// refusals
	REQUIRE_THROWS_MATCHES(GetFilePath(Value::Create(conn.GetFactory(), int32_t(42))), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(GetFilePath(Value::CreateNull(conn.GetFactory(), conn.GetFactory().ParseType("VARCHAR"))),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	std::vector<std::pair<std::string, Value>> no_path;
	no_path.emplace_back("file_size", Value::Create(conn.GetFactory(), uint64_t(42)));
	auto no_path_struct = Value::CreateStruct(conn.GetFactory(), no_path);
	REQUIRE_THROWS_MATCHES(GetFilePath(no_path_struct), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	auto options = fs.CreateOpenOptions();
	REQUIRE_THROWS_MATCHES(options.SetValues(no_path_struct), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: file system path queries", "[cpp_api]") {
	using namespace duckdb::cxx;
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();

	REQUIRE(fs.IsRemote("s3://bucket/book.xlsx"));
	REQUIRE_FALSE(fs.IsRemote("/local/book.xlsx"));
	REQUIRE_FALSE(fs.Exists("/definitely/not/here.xlsx"));

	conn.Execute("SET home_directory = '/cpp_home'").Drain();
	REQUIRE(fs.Expand("~/book.xlsx") == "/cpp_home/book.xlsx");
}

TEST_CASE("Stable C++API: file system glob, move and remove", "[cpp_api]") {
	using namespace duckdb::cxx;
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto fs = conn.GetFileSystem();

	const auto first = duckdb::TestCreatePath("cpp_glob_first.globtest");
	const auto second = duckdb::TestCreatePath("cpp_glob_second.globtest");
	for (auto &path : {first, second}) {
		auto file = fs.OpenFile(path, {FileFlags::WRITE, FileFlags::FILE_CREATE_NEW});
		WriteAll(file, "x");
	}
	const auto pattern = duckdb::TestCreatePath("cpp_glob_*.globtest");
	auto matches = fs.Glob(pattern);
	std::sort(matches.begin(), matches.end());
	REQUIRE(matches.size() == 2);
	REQUIRE(duckdb::StringUtil::EndsWith(matches[0], "cpp_glob_first.globtest"));
	REQUIRE(duckdb::StringUtil::EndsWith(matches[1], "cpp_glob_second.globtest"));
	REQUIRE(fs.Glob(duckdb::TestCreatePath("cpp_glob_none_*.globtest")).empty());

	const auto moved = duckdb::TestCreatePath("cpp_glob_moved.globtest");
	fs.Move(first, moved);
	REQUIRE_FALSE(fs.Exists(first));
	REQUIRE(fs.Exists(moved));
	REQUIRE_THROWS_AS(fs.Move(first, moved), Exception);

	REQUIRE(fs.Remove(moved));
	REQUIRE_FALSE(fs.Exists(moved));
	// Nothing to remove is not an error.
	REQUIRE_FALSE(fs.Remove(moved));
	REQUIRE(fs.Remove(second));
	REQUIRE(fs.Glob(pattern).empty());
}
