#include "catch.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static void WriteExtensionDirectoryFile(FileSystem &fs, const string &path, const string &contents) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                    FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
	handle->Write((void *)contents.data(), contents.size());
}

TEST_CASE("Test that listing and updating extensions skip an install in progress", "[api]") {
	DBConfig config;
	config.SetOptionByName("extension_directory", TestCreatePath("extension_install_in_progress"));
	DuckDB db(nullptr, &config);
	Connection con(db);

	// what an install writes before it moves the files in place, first the extension and then its .info
	auto fs = FileSystem::CreateLocal();
	auto directory = ExtensionHelper::ExtensionDirectory(*db.instance, *fs);
	auto temp_path = fs->JoinPath(directory, "in_progress.duckdb_extension.tmp-0b0e7d6c-5a0e-4e76-9d1f-"
	                                         "3c1a5e9b2f40.duckdb_extension");
	WriteExtensionDirectoryFile(*fs, temp_path, "not an extension yet");

	auto result = con.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'in_progress'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	result = con.Query("UPDATE EXTENSIONS");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 0);

	WriteExtensionDirectoryFile(*fs, temp_path + ".info", "");
	result = con.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'in_progress'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	result = con.Query("UPDATE EXTENSIONS");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 0);

	// an installed extension is still listed, also while it is installed again
	WriteExtensionDirectoryFile(*fs, fs->JoinPath(directory, "installed.duckdb_extension"), "stub");
	WriteExtensionDirectoryFile(*fs,
	                            fs->JoinPath(directory, "installed.duckdb_extension.tmp-5d2f8c1e-7b3a-4c9d-8e6f-"
	                                                    "1a2b3c4d5e6f.duckdb_extension"),
	                            "not an extension yet");
	result = con.Query("SELECT extension_name, installed, install_path LIKE '%.tmp-%' FROM duckdb_extensions() WHERE "
	                   "extension_name IN ('in_progress', 'installed')");
	REQUIRE(CHECK_COLUMN(result, 0, {"installed"}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));
	REQUIRE(CHECK_COLUMN(result, 2, {false}));
}
