#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace duckdb;

#ifndef _WIN32
namespace {

//! Makes closing the file of a persistent secret fail after its content is written
class FailingSecretCloseFileSystem : public LocalFileSystem {
public:
	bool fail_close = true;

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		if (!fail_close || !IsSecretFile(path) || !flags.OpenForWriting()) {
			return LocalFileSystem::OpenFile(path, flags, opener);
		}
		unique_ptr<FileHandle> handle;
		auto open_file = [&]() {
			handle = LocalFileSystem::OpenFile(path, flags, opener);
		};
		secret_fd = TestOpenDescriptor(path, open_file);
		if (secret_fd == -1) {
			throw InternalException("Could not find the descriptor of \"%s\"", path);
		}
		return handle;
	}

	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto written = LocalFileSystem::Write(handle, buffer, nr_bytes);
		if (secret_fd != -1 && IsSecretFile(handle.GetPath())) {
			// closing the descriptor behind the handle makes the close of the handle fail
			close(secret_fd);
			secret_fd = -1;
		}
		return written;
	}

private:
	static bool IsSecretFile(const string &path) {
		return StringUtil::Contains(path, ".duckdb_secret");
	}

	int secret_fd = -1;
};

} // namespace

TEST_CASE("A failed close of a persistent secret file is reported", "[api]") {
	auto fs = make_uniq<FailingSecretCloseFileSystem>();
	auto &secret_fs = *fs;
	DBConfig config;
	config.file_system = make_uniq<VirtualFileSystem>(std::move(fs));
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto secret_directory = TestCreatePath("failed_secret_close");
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET secret_directory='" + secret_directory + "'"));

	auto result = con.Query("CREATE PERSISTENT SECRET s (TYPE http, BEARER_TOKEN 'abc')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not close file"));

	// no secret file and no temporary file are left
	result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 's'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	vector<string> files;
	secret_fs.ListFiles(secret_directory, [&](const string &name, bool) { files.push_back(name); });
	REQUIRE(files.empty());

	secret_fs.fail_close = false;
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s (TYPE http, BEARER_TOKEN 'abc')"));
	REQUIRE(secret_fs.FileExists(secret_fs.JoinPath(secret_directory, "s.duckdb_secret")));
}
#endif
