#include "catch.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

enum class ModificationTimeFailure { NOT_IMPLEMENTED, REWRAPPED_NOT_IMPLEMENTED, IO_ERROR };

//! Serves the files of a test directory under a remote and a local prefix, without modification times
class NoModificationTimeFileSystem : public LocalFileSystem {
public:
	static constexpr const char *REMOTE_PREFIX = "s3://no-modification-time/";
	static constexpr const char *LOCAL_PREFIX = "no-modification-time://";

	ModificationTimeFailure failure = ModificationTimeFailure::NOT_IMPLEMENTED;

	explicit NoModificationTimeFileSystem(string directory_p) : directory(std::move(directory_p)) {
	}

	string MapPath(const string &path) {
		for (auto prefix : {REMOTE_PREFIX, LOCAL_PREFIX}) {
			if (StringUtil::StartsWith(path, prefix)) {
				return JoinPath(directory, path.substr(string(prefix).size()));
			}
		}
		return path;
	}

	string GetName() const override {
		return "NoModificationTimeFileSystem";
	}

	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, REMOTE_PREFIX) || StringUtil::StartsWith(path, LOCAL_PREFIX);
	}

	bool IsLocalFileSystem() const override {
		return false;
	}

	bool FileExists(const string &path, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::FileExists(MapPath(path), opener);
	}

	//! Without the direct IO of remote reads, which needs aligned buffers on a local file
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		FileOpenFlags read_flags = FileFlags::FILE_FLAGS_READ;
		if (flags.ReturnNullIfNotExists()) {
			read_flags |= FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS;
		}
		return LocalFileSystem::OpenFile(MapPath(path), read_flags, opener);
	}

	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		switch (failure) {
		case ModificationTimeFailure::IO_ERROR:
			throw IOException("no time today");
		case ModificationTimeFailure::REWRAPPED_NOT_IMPLEMENTED:
			try {
				return FileSystem::GetLastModifiedTime(handle);
			} catch (std::exception &ex) {
				// an error passed on through ErrorData keeps its type but not its class
				ErrorData error(ex);
				error.Throw();
			}
		default:
			return FileSystem::GetLastModifiedTime(handle);
		}
	}

	//! The generic stats ask GetLastModifiedTime, the local stat call would not
	FileMetadata Stats(FileHandle &handle) override {
		return FileSystem::Stats(handle);
	}

	//! Avoid recursion through the overridden Stats
	int64_t GetFileSize(FileHandle &handle) override {
		return LocalFileSystem::Stats(handle).file_size;
	}

	FileType GetFileType(FileHandle &handle) override {
		return LocalFileSystem::Stats(handle).file_type;
	}

	string GetVersionTag(FileHandle &handle) override {
		return FileSystem::GetVersionTag(handle);
	}

	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) override {
		return FileSystem::GetStatsIfExists(file, opener);
	}

private:
	string directory;
};

TEST_CASE("read_blob leaves last_modified NULL when the file system has no modification times", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto directory = TestJoinPath(LocalTestDirectoryPath(), "no_modification_time");
	LocalFileSystem local;
	local.CreateDirectory(directory);
	auto handle = local.OpenFile(local.JoinPath(directory, "a.txt"),
	                             FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	char content[] = "one";
	handle->Write(content, 3);
	handle.reset();
	auto fs = make_uniq<NoModificationTimeFileSystem>(directory);
	auto &no_time_fs = *fs;
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(std::move(fs));

	for (auto failure :
	     {ModificationTimeFailure::NOT_IMPLEMENTED, ModificationTimeFailure::REWRAPPED_NOT_IMPLEMENTED}) {
		no_time_fs.failure = failure;
		for (auto prefix : {NoModificationTimeFileSystem::REMOTE_PREFIX, NoModificationTimeFileSystem::LOCAL_PREFIX}) {
			string file = string(prefix) + "a.txt";
			// metadata only reads take the stats of the file, remote ones through the caching file system
			auto result = con.Query("SELECT size, last_modified FROM read_blob('" + file + "')");
			REQUIRE_NO_FAIL(*result);
			REQUIRE(CHECK_COLUMN(result, 0, {3}));
			REQUIRE(CHECK_COLUMN(result, 1, {Value()}));

			result = con.Query("SELECT content, last_modified FROM read_blob('" + file + "')");
			REQUIRE_NO_FAIL(*result);
			REQUIRE(CHECK_COLUMN(result, 0, {Value::BLOB_RAW(string("one"))}));
			REQUIRE(CHECK_COLUMN(result, 1, {Value()}));
		}
	}

	// other errors of the modification time still fail the read
	no_time_fs.failure = ModificationTimeFailure::IO_ERROR;
	for (auto prefix : {NoModificationTimeFileSystem::REMOTE_PREFIX, NoModificationTimeFileSystem::LOCAL_PREFIX}) {
		string file = string(prefix) + "a.txt";
		auto result = con.Query("SELECT size, last_modified FROM read_blob('" + file + "')");
		REQUIRE_FAIL(result);
		REQUIRE(StringUtil::Contains(result->GetError(), "no time today"));
	}
}
