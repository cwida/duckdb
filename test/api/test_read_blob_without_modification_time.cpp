#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

//! Serves the files of a test directory under a remote and a local prefix, without modification times, like a
//! Python file system whose info() has no mtime
class NoModificationTimeFileSystem : public LocalFileSystem {
public:
	static constexpr const char *REMOTE_PREFIX = "s3://no-modification-time/";
	static constexpr const char *LOCAL_PREFIX = "no-modification-time://";

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

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override {
		vector<OpenFileInfo> result;
		if (LocalFileSystem::FileExists(MapPath(path), opener)) {
			result.emplace_back(path);
		}
		return result;
	}

	//! Opens for reading without the direct IO that the reader asks of remote files, which a local file rejects
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		FileOpenFlags read_flags = FileFlags::FILE_FLAGS_READ;
		if (flags.ReturnNullIfNotExists()) {
			read_flags |= FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS;
		}
		return LocalFileSystem::OpenFile(MapPath(path), read_flags, opener);
	}

	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		throw NotImplementedException("%s: GetLastModifiedTime is not implemented!", GetName());
	}

	//! The generic stats, which ask GetLastModifiedTime, instead of the local stat call
	FileMetadata Stats(FileHandle &handle) override {
		return FileSystem::Stats(handle);
	}

	//! The local answers go through the stat call, not through the generic stats, which would ask them again
	int64_t GetFileSize(FileHandle &handle) override {
		return LocalFileSystem::Stats(handle).file_size;
	}

	FileType GetFileType(FileHandle &handle) override {
		return LocalFileSystem::Stats(handle).file_type;
	}

	string GetVersionTag(FileHandle &handle) override {
		return string();
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

	auto directory = TestCreatePath("no_modification_time");
	LocalFileSystem local;
	local.CreateDirectory(directory);
	auto handle = local.OpenFile(local.JoinPath(directory, "a.txt"),
	                             FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	char content[] = "one";
	handle->Write(content, 3);
	handle.reset();
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(make_uniq<NoModificationTimeFileSystem>(directory));

	for (auto prefix : {NoModificationTimeFileSystem::REMOTE_PREFIX, NoModificationTimeFileSystem::LOCAL_PREFIX}) {
		string file = string(prefix) + "a.txt";
		// metadata only reads take the stats of the file, remote ones through the caching file system
		auto result = con.Query("SELECT size, last_modified FROM read_blob('" + file + "')");
		string error = result->HasError() ? result->GetError() : string();
		INFO(error);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {3}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value()}));

		result = con.Query("SELECT content, last_modified FROM read_blob('" + file + "')");
		error = result->HasError() ? result->GetError() : string();
		INFO(error);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BLOB_RAW(string("one"))}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value()}));
	}
}
