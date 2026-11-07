#include "catch.hpp"
#include "duckdb/main/extension/external_extension_provider.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "test_helpers.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace duckdb;

static string DemoExtensionPath() {
	return TestJoinPath(TestJoinPath(TestJoinPath(TestBuildDirectory(), "test"), "extension"),
	                    "loadable_extension_demo.duckdb_extension");
}

static void SetExtensionDirectories(DBConfig &config, const vector<string> &directories) {
	vector<Value> values;
	for (auto &directory : directories) {
		values.emplace_back(directory);
	}
	config.SetOptionByName("allow_unsigned_extensions", true);
	config.SetOptionByName("extension_directories", Value::LIST(LogicalType::VARCHAR, std::move(values)));
}

static bool CanInstallDemoExtension() {
#ifdef __MINGW32__
	return false;
#else
	DuckDB db(nullptr);
	return DBConfig::GetConfig(*db.instance).GetExternalExtensionProvider().SupportsExternalExtensions();
#endif
}

static void InstallDemoExtensionInto(const string &directory) {
	DBConfig config;
	SetExtensionDirectories(config, {directory});
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("INSTALL '" + DemoExtensionPath() + "'"));
}

static void RequireInstalledFileCount(Connection &con, const string &directory, int64_t count) {
	auto result = con.Query("SELECT count(*) FROM glob('" + directory + "/**/*.duckdb_extension*')");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(count)}));
}

TEST_CASE("Autoinstall finds a later extension directory", "[api]") {
	if (!CanInstallDemoExtension()) {
		return;
	}
	auto first = TestCreatePath("ext_a1");
	auto second = TestCreatePath("ext_a2");
	InstallDemoExtensionInto(second);

	// the autoinstall repository is unreachable, so autoloading succeeds only if the install finds the copy
	DBConfig config;
	SetExtensionDirectories(config, {first, second});
	config.SetOptionByName("autoinstall_known_extensions", true);
	config.SetOptionByName("autoload_known_extensions", true);
	config.SetOptionByName("autoinstall_extension_repository", TestCreatePath("autoinstall_missing_repository"));

	SECTION("database instance") {
		DuckDB db(nullptr, &config);
		REQUIRE(ExtensionHelper::TryAutoLoadExtension(*db.instance, "loadable_extension_demo"));
		REQUIRE(db.instance->ExtensionIsLoaded("loadable_extension_demo"));
		REQUIRE(!FileSystem::CreateLocal()->DirectoryExists(first));
	}
	SECTION("autoload") {
		DuckDB db(nullptr, &config);
		REQUIRE_NOTHROW(ExtensionHelper::AutoLoadExtension(*db.instance, "loadable_extension_demo"));
		REQUIRE(db.instance->ExtensionIsLoaded("loadable_extension_demo"));
		REQUIRE(!FileSystem::CreateLocal()->DirectoryExists(first));
	}
	SECTION("client context") {
		DuckDB db(nullptr, &config);
		Connection con(db);
		REQUIRE(ExtensionHelper::TryAutoLoadExtension(*con.context, "loadable_extension_demo"));
		REQUIRE(db.instance->ExtensionIsLoaded("loadable_extension_demo"));
		REQUIRE(!FileSystem::CreateLocal()->DirectoryExists(first));
	}
}

TEST_CASE("Invalid later copy installs but does not load", "[api]") {
	if (!CanInstallDemoExtension()) {
		return;
	}
	auto repository = TestCreatePath("invalid_copy_repository");
	InstallDemoExtensionInto(repository);
	auto first = TestCreatePath("ext_b1");
	auto second = TestCreatePath("ext_b2");

	DBConfig config;
	SetExtensionDirectories(config, {first, second});
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto &fs = FileSystem::GetFileSystem(*db.instance);
	auto second_path = ExtensionHelper::GetExtensionDirectoryPath(*db.instance, fs)[1];
	fs.CreateDirectoriesRecursive(second_path);
	auto handle = fs.OpenFile(fs.JoinPath(second_path, "loadable_extension_demo.duckdb_extension"),
	                          FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                              FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
	string contents = "not an extension";
	handle->Write((void *)contents.data(), contents.size());
	handle->Close();

	REQUIRE_NO_FAIL(con.Query("INSTALL loadable_extension_demo FROM '" + repository + "'"));
	REQUIRE(!FileSystem::CreateLocal()->DirectoryExists(first));
	auto load = con.Query("LOAD loadable_extension_demo");
	REQUIRE(load->HasError());
	REQUIRE(StringUtil::Contains(load->GetError(), "is not a DuckDB extension"));
	REQUIRE(!db.instance->ExtensionIsLoaded("loadable_extension_demo"));

	REQUIRE_NO_FAIL(con.Query("FORCE INSTALL loadable_extension_demo FROM '" + repository + "'"));
	RequireInstalledFileCount(con, first, 2);
	REQUIRE_NO_FAIL(con.Query("LOAD loadable_extension_demo"));
	REQUIRE(db.instance->ExtensionIsLoaded("loadable_extension_demo"));
}

TEST_CASE("Only the file the loader looks for is an installed copy", "[api]") {
	if (!CanInstallDemoExtension()) {
		return;
	}
	auto first = TestCreatePath("ext_d1");
	auto second = TestCreatePath("ext_d2");
	InstallDemoExtensionInto(second);

	DBConfig config;
	SetExtensionDirectories(config, {first, second});
	DuckDB db(nullptr, &config);
	Connection con(db);

	// a stray file and a directory in the first directory that both carry the extension's name
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	auto first_path = ExtensionHelper::GetExtensionDirectoryPath(*db.instance, fs)[0];
	fs.CreateDirectoriesRecursive(fs.JoinPath(first_path, "loadable_extension_demo.duckdb_extension"));
	auto handle = fs.OpenFile(fs.JoinPath(first_path, "loadable_extension_demo.old.duckdb_extension"),
	                          FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                              FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
	string contents = "not an extension";
	handle->Write((void *)contents.data(), contents.size());
	handle->Close();

	auto listed = con.Query("SELECT install_mode, install_path LIKE '%ext_d2%' FROM duckdb_extensions() WHERE "
	                        "extension_name = 'loadable_extension_demo'");
	REQUIRE(CHECK_COLUMN(listed, 0, {"CUSTOM_PATH"}));
	REQUIRE(CHECK_COLUMN(listed, 1, {true}));
	auto updated = con.Query("UPDATE EXTENSIONS");
	REQUIRE(CHECK_COLUMN(updated, 0, {"loadable_extension_demo"}));
	REQUIRE(CHECK_COLUMN(updated, 2, {"NOT_A_REPOSITORY"}));
	REQUIRE_NO_FAIL(con.Query("LOAD loadable_extension_demo"));
}

#ifndef _WIN32
struct RestrictedDirectory {
	RestrictedDirectory(string path_p, mode_t mode) : path(std::move(path_p)) {
		chmod(path.c_str(), mode);
	}
	~RestrictedDirectory() {
		chmod(path.c_str(), 0755);
	}
	string path;
};

TEST_CASE("Update writes the first extension directory and not a read only later one", "[api]") {
	// root writes into a read only directory anyway
	if (!CanInstallDemoExtension() || geteuid() == 0) {
		return;
	}
	auto repository = TestCreatePath("update_ro_repository");
	InstallDemoExtensionInto(repository);
	auto first = TestCreatePath("ext_c1");
	auto second = TestCreatePath("ext_c2");
	{
		DBConfig config;
		SetExtensionDirectories(config, {second});
		DuckDB db(nullptr, &config);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("INSTALL loadable_extension_demo FROM '" + repository + "'"));
	}

	DBConfig config;
	SetExtensionDirectories(config, {first, second});
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	RestrictedDirectory read_only(ExtensionHelper::GetExtensionDirectoryPath(*db.instance, fs)[1], 0555);
	REQUIRE(access(read_only.path.c_str(), W_OK) != 0);

	auto result = con.Query("UPDATE EXTENSIONS");
	REQUIRE(CHECK_COLUMN(result, 0, {"loadable_extension_demo"}));
	REQUIRE(CHECK_COLUMN(result, 2, {"NO_UPDATE_AVAILABLE"}));
	RequireInstalledFileCount(con, first, 2);
	RequireInstalledFileCount(con, second, 2);
	REQUIRE_NO_FAIL(con.Query("LOAD loadable_extension_demo"));
}

TEST_CASE("An extension directory that cannot be listed still holds the installed copy", "[api]") {
	// root lists a directory without read permission anyway
	if (!CanInstallDemoExtension() || geteuid() == 0) {
		return;
	}
	auto repository = TestCreatePath("unlisted_repository");
	InstallDemoExtensionInto(repository);
	auto first = TestCreatePath("ext_e1");
	auto second = TestCreatePath("ext_e2");
	InstallDemoExtensionInto(first);
	{
		DBConfig config;
		SetExtensionDirectories(config, {second});
		DuckDB db(nullptr, &config);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("INSTALL loadable_extension_demo FROM '" + repository + "'"));
	}

	DBConfig config;
	SetExtensionDirectories(config, {first, second});
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	RestrictedDirectory unlisted(ExtensionHelper::GetExtensionDirectoryPath(*db.instance, fs)[0], 0300);
	REQUIRE(access(unlisted.path.c_str(), R_OK) != 0);

	// the name comes from the second directory, the copy is the loader's in the first
	auto listed = con.Query("SELECT install_mode, install_path LIKE '%ext_e1%' FROM duckdb_extensions() WHERE "
	                        "extension_name = 'loadable_extension_demo'");
	REQUIRE(CHECK_COLUMN(listed, 0, {"CUSTOM_PATH"}));
	REQUIRE(CHECK_COLUMN(listed, 1, {true}));
	auto updated = con.Query("UPDATE EXTENSIONS");
	REQUIRE(CHECK_COLUMN(updated, 0, {"loadable_extension_demo"}));
	REQUIRE(CHECK_COLUMN(updated, 2, {"NOT_A_REPOSITORY"}));
}
#endif
