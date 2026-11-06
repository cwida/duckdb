#include "catch.hpp"
#include "duckdb.h"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "test_helpers.hpp"

#include <iostream>
#include <map>
#include <set>

using namespace duckdb;

TEST_CASE("Test autoload of extension settings", "[api]") {
	DBConfig config;
	config.SetOptionByName("timezone", "America/Los_Angeles");

	config.SetOptionByName("allow_unsigned_extensions", true);
	config.SetOptionByName("autoload_known_extensions", true);
	auto env_var = std::getenv("LOCAL_EXTENSION_REPO");
	if (!env_var) {
		return;
	}
	config.SetOptionByName("autoinstall_extension_repo", string(env_var));
	REQUIRE(config.options.unrecognized_options.count("timezone"));

	// Create a connection
	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_NOTHROW(db = make_uniq<DuckDB>(nullptr, &config));
	Connection con(*db);

	auto res = con.Query("select current_setting('timezone')");
	REQUIRE(CHECK_COLUMN(res, 0, {Value("America/Los_Angeles")}));
}

static bool IsLinked(const string &extension_name) {
	for (auto &linked : LinkedExtensionRegistry::Get()) {
		if (linked.load && StringUtil::CIEquals(linked.name, extension_name)) {
			return true;
		}
	}
	return false;
}

// a repository that does not exist and a fresh extension directory, so no case can install or reach the network
static string IsolateFromRepository(DBConfig &config) {
	auto extension_directory = TestCreatePath("startup_setting_extensions");
	TestDeleteDirectory(extension_directory);
	config.SetOptionByName("autoinstall_extension_repository", TestCreatePath("no_such_repository"));
	config.SetOptionByName("extension_directories", Value::LIST({Value(extension_directory)}));
	config.SetOptionByName("autoload_known_extensions", true);
	config.SetOptionByName("autoinstall_known_extensions", true);
	return extension_directory;
}

static void RequireLoaded(Connection &con, const string &extension_name) {
	auto res = con.Query("SELECT loaded FROM duckdb_extensions() WHERE extension_name = '" + extension_name + "'");
	REQUIRE(CHECK_COLUMN(res, 0, {true}));
}

static void RequireNothingInstalled(const string &extension_directory) {
	auto fs = FileSystem::CreateLocal();
	REQUIRE(!fs->DirectoryExists(extension_directory));
}

TEST_CASE("A startup setting of a linked extension does not need the extension repository", "[api]") {
	if (!IsLinked("icu")) {
		return;
	}
	DBConfig config;
	auto extension_directory = IsolateFromRepository(config);
	config.SetOptionByName("TimeZone", "America/Los_Angeles");

	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_NOTHROW(db = make_uniq<DuckDB>(nullptr, &config));
	Connection con(*db);
	auto res = con.Query("SELECT current_setting('TimeZone'), TIMESTAMPTZ '2024-01-01 00:00:00+00'::VARCHAR");
	REQUIRE(CHECK_COLUMN(res, 0, {"America/Los_Angeles"}));
	REQUIRE(CHECK_COLUMN(res, 1, {"2023-12-31 16:00:00-08"}));
	RequireLoaded(con, "icu");
	RequireNothingInstalled(extension_directory);
}

TEST_CASE("Two startup settings of a linked extension both take effect without the repository", "[api]") {
	if (!IsLinked("icu")) {
		return;
	}
	DBConfig config;
	auto extension_directory = IsolateFromRepository(config);
	config.SetOptionByName("TimeZone", "America/Los_Angeles");
	config.SetOptionByName("Calendar", "japanese");

	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_NOTHROW(db = make_uniq<DuckDB>(nullptr, &config));
	Connection con(*db);
	auto res = con.Query("SELECT current_setting('TimeZone'), current_setting('Calendar')");
	REQUIRE(CHECK_COLUMN(res, 0, {"America/Los_Angeles"}));
	REQUIRE(CHECK_COLUMN(res, 1, {"japanese"}));
	RequireNothingInstalled(extension_directory);
}

TEST_CASE("A startup setting of a linked extension with autoinstall off", "[api]") {
	if (!IsLinked("icu")) {
		return;
	}
	DBConfig config;
	IsolateFromRepository(config);
	config.SetOptionByName("autoinstall_known_extensions", false);
	config.SetOptionByName("TimeZone", "America/Los_Angeles");

	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_NOTHROW(db = make_uniq<DuckDB>(nullptr, &config));
	Connection con(*db);
	auto res = con.Query("SELECT current_setting('TimeZone')");
	REQUIRE(CHECK_COLUMN(res, 0, {"America/Los_Angeles"}));
	RequireLoaded(con, "icu");
}

TEST_CASE("A startup setting of an extension that is neither linked nor installed still fails to autoload", "[api]") {
	if (IsLinked("httpfs")) {
		return;
	}
	DBConfig config;
	auto extension_directory = IsolateFromRepository(config);
	config.SetOptionByName("s3_region", "eu-west-1");

	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_THROWS_WITH(db = make_uniq<DuckDB>(nullptr, &config),
	                    Catch::Contains("the httpfs extension needs to be loaded. But it could not be autoloaded"));
	auto fs = FileSystem::CreateLocal();
	REQUIRE(fs->DirectoryExists(extension_directory));
}

TEST_CASE("A runtime setting of a linked extension that is not loaded yet does not need the repository", "[api]") {
	if (!IsLinked("icu")) {
		return;
	}
	DBConfig config;
	auto extension_directory = IsolateFromRepository(config);
	config.options.load_extensions = false;

	DuckDB db(nullptr, &config);
	if (!ExtensionHelper::CanAutoloadExtension(*db.instance, "icu")) {
		return;
	}
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET TimeZone = 'America/Los_Angeles'"));
	auto res = con.Query("SELECT current_setting('TimeZone')");
	REQUIRE(CHECK_COLUMN(res, 0, {"America/Los_Angeles"}));
	RequireLoaded(con, "icu");
	RequireNothingInstalled(extension_directory);
}

TEST_CASE("A runtime autoload through the client context of a linked extension does not need the repository", "[api]") {
	if (!IsLinked("parquet")) {
		return;
	}
	DBConfig config;
	auto extension_directory = IsolateFromRepository(config);
	config.options.load_extensions = false;

	DuckDB db(nullptr, &config);
	if (!ExtensionHelper::CanAutoloadExtension(*db.instance, "parquet")) {
		return;
	}
	Connection con(db);
	// a zst replacement scan asks for parquet through the client context before it looks at the file
	auto scan = con.Query("SELECT * FROM 'no_such_file.csv.zst'");
	REQUIRE(scan->HasError());
	REQUIRE(StringUtil::Contains(scan->GetError(), "No files found that match the pattern"));
	RequireLoaded(con, "parquet");
	RequireNothingInstalled(extension_directory);
}

TEST_CASE("An autoload by the file path of a linked extension still goes through the install step", "[api]") {
	if (!IsLinked("icu")) {
		return;
	}
	DBConfig config;
	auto extension_directory = IsolateFromRepository(config);
	config.options.load_extensions = false;

	DuckDB db(nullptr, &config);
	REQUIRE(!ExtensionHelper::TryAutoLoadExtension(*db.instance, TestCreatePath("icu.duckdb_extension")));
	auto fs = FileSystem::CreateLocal();
	REQUIRE(fs->DirectoryExists(extension_directory));
	Connection con(db);
	auto res = con.Query("SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'icu'");
	REQUIRE(CHECK_COLUMN(res, 0, {false}));
}

TEST_CASE("A startup setting of a linked extension through the C API does not need the repository", "[api][capi]") {
	if (!IsLinked("icu")) {
		return;
	}
	auto extension_directory = TestCreatePath("startup_setting_extensions");
	TestDeleteDirectory(extension_directory);
	auto repository = TestCreatePath("no_such_repository");

	duckdb_config config;
	REQUIRE(duckdb_create_config(&config) == DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "autoinstall_extension_repository", repository.c_str()) == DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "extension_directories", ("[" + extension_directory + "]").c_str()) ==
	        DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "autoinstall_known_extensions", "true") == DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "autoload_known_extensions", "true") == DuckDBSuccess);
	REQUIRE(duckdb_set_config(config, "TimeZone", "America/Los_Angeles") == DuckDBSuccess);

	duckdb_database database;
	char *error = nullptr;
	auto state = duckdb_open_ext(nullptr, &database, config, &error);
	duckdb_destroy_config(&config);
	string error_message = error ? error : "";
	duckdb_free(error);
	REQUIRE(error_message == "");
	REQUIRE(state == DuckDBSuccess);

	duckdb_connection connection;
	REQUIRE(duckdb_connect(database, &connection) == DuckDBSuccess);
	duckdb_result result;
	REQUIRE(duckdb_query(connection, "SELECT current_setting('TimeZone')", &result) == DuckDBSuccess);
	auto zone = duckdb_value_varchar(&result, 0, 0);
	string zone_value = zone;
	duckdb_free(zone);
	duckdb_destroy_result(&result);
	duckdb_disconnect(&connection);
	duckdb_close(&database);
	REQUIRE(zone_value == "America/Los_Angeles");
	RequireNothingInstalled(extension_directory);
}
