#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/common/local_file_system.hpp"

#include <set>
#include <map>

using namespace duckdb;

class FileCleaner {
public:
	FileCleaner(LocalFileSystem *fs, string file) : fs(*fs), file(std::move(file)) {
	}

	~FileCleaner() {
		if (fs.FileExists(file)) {
			fs.RemoveFile(file);
		}
	}

private:
	LocalFileSystem &fs;
	string file;
};

TEST_CASE("Test DB config configuration", "[api]") {
	DBConfig config;

	auto options = config.GetOptions();

	map<string, duckdb::vector<string>> test_options;
	test_options["access_mode"] = {"automatic", "read_only", "read_write"};
	test_options["default_order"] = {"asc", "desc"};
	test_options["default_null_order"] = {"nulls_first", "nulls_last"};
	test_options["enable_external_access"] = {"true", "false"};
	test_options["max_memory"] = {"-1", "16GB"};
	test_options["threads"] = {"1", "4"};

	REQUIRE(config.GetOptionByName("unknownoption") == nullptr);

	for (auto &option : options) {
		auto op = config.GetOptionByName(option.name);
		REQUIRE(op);

		auto entry = test_options.find(option.name);
		if (entry != test_options.end()) {
			for (auto &str_val : entry->second) {
				Value val(str_val);
				REQUIRE_NOTHROW(config.SetOption(option, val));
			}
			Value invalid_val("___this_is_probably_invalid");
			REQUIRE_THROWS(config.SetOption(option, invalid_val));
		}
	}
}

TEST_CASE("Test allowed options", "[api]") {
	identifier_map_t<Value> config_dict;
	string option;

	SECTION("allowed_directories") {
		config_dict.emplace("allowed_directories", Value::LIST({Value("test")}));
		option = "allowed_directories";
	}
	SECTION("allowed_paths") {
		config_dict.emplace("allowed_paths", Value::LIST({Value("test")}));
		option = "allowed_paths";
	}

	SECTION("enable_logging") {
		config_dict.emplace("enable_logging", Value::BOOLEAN(false));
		option = "enable_logging";
	}

	SECTION("disabled_filesystems") {
		config_dict.emplace("disabled_filesystems", Value::BOOLEAN(false));
		option = "disabled_filesystems";
	}

	SECTION("logging_mode") {
		config_dict.emplace("logging_mode", Value::BOOLEAN(false));
		option = "logging_mode";
	}

	SECTION("logging_storage") {
		config_dict.emplace("logging_storage", Value::BOOLEAN(false));
		option = "logging_storage";
	}

	SECTION("logging_level") {
		config_dict.emplace("logging_level", Value::BOOLEAN(false));
		option = "logging_level";
	}

	SECTION("enabled_log_types") {
		config_dict.emplace("enabled_log_types", Value::BOOLEAN(false));
		option = "enabled_log_types";
	}

	SECTION("disabled_log_types") {
		config_dict.emplace("disabled_log_types", Value::BOOLEAN(false));
		option = "disabled_log_types";
	}

	try {
		DBConfig config(config_dict, false);
	} catch (std::exception &ex) {
		ErrorData error_data(ex);
		REQUIRE(error_data.Type() == ExceptionType::INVALID_INPUT);
		REQUIRE(error_data.RawMessage() ==
		        StringUtil::Format("Cannot change/set %s before the database is started", option));
	}
}

TEST_CASE("Test user_agent", "[api]") {
	{
		// Default duckdb_api is cpp
		DuckDB db(nullptr);
		Connection con(db);
		auto res = con.Query("PRAGMA user_agent");
		REQUIRE_THAT(res->Collection().GetValue(0, 0).ToString(), Catch::Matchers::Matches("duckdb/.*(.*) cpp"));
	}
	{
		// The latest provided duckdb_api is used
		DBConfig config;
		config.SetOptionByName("duckdb_api", "capi");
		config.SetOptionByName("duckdb_api", "go");
		DuckDB db("", &config);
		Connection con(db);
		auto res = con.Query("PRAGMA user_agent");
		REQUIRE_THAT(res->Collection().GetValue(0, 0).ToString(), Catch::Matchers::Matches("duckdb/.*(.*) go"));
	}
	{
		DuckDB db(nullptr);
		HTTPHeaders headers(*db.instance);
		auto user_agent = headers.GetHeaderValue("User-Agent");
		REQUIRE(!user_agent.empty());
		REQUIRE(user_agent.back() != ' ');
		REQUIRE(user_agent.back() != '\t');
	}
}

TEST_CASE("HTTPHeaders preserves repeated field values", "[api]") {
	HTTPHeaders headers;
	headers.Insert("Cache-Control", "max-age=600");
	headers.Append("cache-control", "no-store");

	REQUIRE(headers.GetHeaderValue("CACHE-CONTROL") == "max-age=600");
	const auto values = headers.GetHeaderValues("Cache-Control");
	REQUIRE(values.size() == 2);
	REQUIRE(values[0] == "max-age=600");
	REQUIRE(values[1] == "no-store");
}

TEST_CASE("Test secret_directory configuration", "[api]") {
	DBConfig config;

	auto options = config.GetOptions();

	config.SetOptionByName("secret_directory", Value("my_secret_dir"));
	config.SetOptionByName("extension_directory", Value("my_extension_dir"));

	DuckDB db(nullptr, &config);
	Connection con(db);

	// Ensure that the extension directory is set correctly (according to the inital config)
	auto select_extension_dir = con.Query("SELECT current_setting('extension_directory') AS extdir;");
	REQUIRE(select_extension_dir->Collection().GetValue(0, 0).ToString() == "my_extension_dir");

	auto select_secret_dir = con.Query("SELECT current_setting('secret_directory') AS secretdir;");
	REQUIRE(select_secret_dir->Collection().GetValue(0, 0).ToString() == "my_secret_dir");
}

TEST_CASE("Test secret creation with a custom secret_directory configuration", "[api]") {
	LocalFileSystem fs;
	string directory_path = TestDirectoryPath();
	string my_secret_dir = fs.JoinPath(directory_path, "my_secret_dir");
	string my_secret_file = fs.JoinPath(my_secret_dir, "my_secret.duckdb_secret");
	FileCleaner cleaner(&fs, my_secret_file);

	DBConfig config;

	auto options = config.GetOptions();

	config.SetOptionByName("secret_directory", Value(my_secret_dir));

	DuckDB db(nullptr, &config);
	Connection con(db);

	// Ensure that the extension directory is set correctly (according to the inital config)
	auto select_secret_dir = con.Query("SELECT current_setting('secret_directory') AS secretdir;");
	REQUIRE(select_secret_dir->Collection().GetValue(0, 0).ToString() == my_secret_dir);

	// Ensure that creating a secret works and the secret file is created in the correct directory
	auto create_secret = con.Query("CREATE PERSISTENT SECRET my_secret (TYPE http, BEARER_TOKEN 'token')");
	REQUIRE(create_secret->Collection().GetValue(0, 0).GetValue<bool>());
	REQUIRE(fs.FileExists(my_secret_file));
}

TEST_CASE("Test secret creation with a custom secret_directory configuration update", "[api]") {
	LocalFileSystem fs;
	string directory_path = TestDirectoryPath();
	string my_secret_dir = fs.JoinPath(directory_path, "my_secret_dir");
	string new_secret_dir = fs.JoinPath(directory_path, "new_secret_dir");
	string my_other_secret_file = fs.JoinPath(new_secret_dir, "my_other_secret.duckdb_secret");
	FileCleaner cleaner(&fs, my_other_secret_file);

	DBConfig config;

	auto options = config.GetOptions();

	config.SetOptionByName("secret_directory", Value(my_secret_dir));

	DuckDB db(nullptr, &config);
	Connection con(db);

	// Ensure that the extension directory is set correctly (according to the inital config)
	auto select_secret_dir = con.Query("SELECT current_setting('secret_directory') AS secretdir;");
	REQUIRE(select_secret_dir->Collection().GetValue(0, 0).ToString() == my_secret_dir);

	// Do not create a secret here because it will initialize the secret manager and forbid us to update the value.

	// Update the secret directory and ensure that the setting is updated
	con.Query("SET secret_directory='" + new_secret_dir + "';");
	auto select_new_secret_dir = con.Query("SELECT current_setting('secret_directory') AS secretdir;");
	REQUIRE(select_new_secret_dir->Collection().GetValue(0, 0).ToString() == new_secret_dir);

	// Create another secret and ensure that it is created in the new directory
	auto new_create_secret = con.Query("CREATE PERSISTENT SECRET my_other_secret (TYPE http, BEARER_TOKEN 'token')");
	REQUIRE(new_create_secret->Collection().GetValue(0, 0).GetValue<bool>());
	REQUIRE(fs.FileExists(my_other_secret_file));
}

TEST_CASE("Test secret_directory configuration update after secret creation", "[api]") {
	LocalFileSystem fs;
	string directory_path = TestDirectoryPath();
	string my_secret_dir = fs.JoinPath(directory_path, "my_secret_dir");
	string my_secret_file = fs.JoinPath(my_secret_dir, "my_secret.duckdb_secret");
	FileCleaner cleaner(&fs, my_secret_file);

	DBConfig config;

	auto options = config.GetOptions();

	config.SetOptionByName("secret_directory", Value(my_secret_dir));

	DuckDB db(nullptr, &config);
	Connection con(db);

	// Ensure that the extension directory is set correctly (according to the inital config)
	auto select_secret_dir = con.Query("SELECT current_setting('secret_directory') AS secretdir;");
	REQUIRE(select_secret_dir->Collection().GetValue(0, 0).ToString() == my_secret_dir);

	// Create a secret here to initialize the secret manager
	auto create_secret = con.Query("CREATE PERSISTENT SECRET my_secret (TYPE http, BEARER_TOKEN 'token')");
	REQUIRE(create_secret->Collection().GetValue(0, 0).GetValue<bool>());
	REQUIRE(fs.FileExists(my_secret_file));

	// Try to update the secret directory and expect failure as the secret manager is already initialized
	auto update_secret_directory = con.Query("SET secret_directory='new_secret_dir';");
	REQUIRE_FAIL(update_secret_directory);
}

TEST_CASE("Test warnings_as_errors in the startup config", "[api]") {
	DBConfig config;
	REQUIRE_NOTHROW(config.SetOptionByName("warnings_as_errors", Value::BOOLEAN(false)));
	REQUIRE_THROWS_WITH(config.SetOptionByName("warnings_as_errors", Value::BOOLEAN(true)),
	                    Catch::Contains("startup config"));
	// string values, as the C API passes them
	REQUIRE_NOTHROW(config.SetOptionByName("warnings_as_errors", Value("false")));
	REQUIRE_THROWS_WITH(config.SetOptionByName("warnings_as_errors", Value("true")), Catch::Contains("startup config"));
	REQUIRE_THROWS_WITH(config.SetOptionByName("warnings_as_errors", Value()), Catch::Contains("cannot be NULL"));
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto result = con.Query("SELECT current_setting('warnings_as_errors')");
	REQUIRE(CHECK_COLUMN(result, 0, {false}));
}

TEST_CASE("Test deprecated extension options warn when set", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = DBConfig::GetConfig(*db.instance);
	config.AddExtensionOption("my_deprecated_option", "A deprecated option", LogicalType::BOOLEAN, Value(false),
	                          nullptr, SetScope::SESSION, false, true);
	config.AddExtensionOption("my_option", "An option", LogicalType::BOOLEAN, Value(false));

	REQUIRE_NO_FAIL(con.Query("SET enable_logging=true"));
	REQUIRE_NO_FAIL(con.Query("SET warnings_as_errors=true"));

	auto result = con.Query("SET my_deprecated_option=true");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(),
	                             "The 'my_deprecated_option' setting is deprecated and will be removed"));
	REQUIRE_NO_FAIL(con.Query("RESET my_deprecated_option"));
	REQUIRE_NO_FAIL(con.Query("SET my_option=true"));

	// setting the option directly, as ducklake does, warns as well
	ExtensionOption option;
	REQUIRE(config.TryGetExtensionOption(Identifier("my_deprecated_option"), option));
	REQUIRE_THROWS_WITH(PhysicalSet::SetExtensionVariable(*con.context, option, SetScope::SESSION, Value(true)),
	                    Catch::Contains("is deprecated"));
	option = ExtensionOption();
	REQUIRE(config.TryGetExtensionOption(Identifier("my_option"), option));
	REQUIRE_NOTHROW(PhysicalSet::SetExtensionVariable(*con.context, option, SetScope::SESSION, Value(true)));

	result = con.Query("SELECT count(*) FROM duckdb_settings() WHERE name = 'my_deprecated_option'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	result = con.Query("SELECT count(*) FROM duckdb_settings(deprecated := true) WHERE name = 'my_deprecated_option'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}

TEST_CASE("Test every deprecated setting warns when set", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET enable_logging=true"));
	REQUIRE_NO_FAIL(con.Query("SET warnings_as_errors=true"));
	// profiling_mode only has a value while profiling is on
	REQUIRE_NO_FAIL(con.Query("SET enable_profiling='no_output'"));

	auto settings = con.Query("SELECT name, value FROM duckdb_settings(deprecated := true) ORDER BY name");
	REQUIRE_NO_FAIL(*settings);
	idx_t checked = 0;
	while (auto chunk = settings->Fetch()) {
		for (idx_t i = 0; i < chunk->size(); i++) {
			auto name = chunk->GetValue(0, i).ToString();
			auto value = chunk->GetValue(1, i).ToSQLString();
			INFO(name);
			// setting a deprecated setting to its current value warns, which warnings_as_errors turns into an error
			auto result = con.Query(StringUtil::Format("SET %s = %s", name, value));
			REQUIRE(result->HasError());
			if (name == "vacuum_rebuild_indexes") {
				// only settable in the startup config, so it never reaches the warning
				REQUIRE(StringUtil::Contains(result->GetError(), "while database is running"));
			} else {
				REQUIRE(StringUtil::Contains(result->GetError(), "deprecated"));
			}
			checked++;
		}
	}
	REQUIRE(checked > 0);
}
