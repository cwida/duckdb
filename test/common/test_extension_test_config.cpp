#include "catch.hpp"
#include "test_config.hpp"
#include "test_helpers.hpp"

#include <fstream>

namespace duckdb {

static string WriteScopedTestConfig(const string &name, const string &contents) {
	auto path = TestCreatePath(name);
	std::ofstream output(path);
	output << contents;
	output.close();
	REQUIRE(output.good());
	return path;
}

TEST_CASE("Extension test configs restore initialization, environment and exclusions", "[test_config]") {
	auto &config = TestConfiguration::Get();
	auto initial_command = config.OnInitCommand();
	auto outer_path = WriteScopedTestConfig("outer.json", R"({
		"on_init": "SELECT 'outer'",
		"test_env": [{"env_name": "SCOPED_TEST_CONFIG", "env_value": "outer"}],
		"skip_error_messages": ["outer error"]
	})");
	WriteScopedTestConfig("inherited.json", R"({
		"on_init": "SELECT 'inherited'",
		"skip_tests": [{"reason": "scope regression", "paths": ["test/sql/scoped_config.test"]}]
	})");
	auto inner_path = WriteScopedTestConfig("inner.json", R"({
		"extends": ["inherited.json"],
		"test_env": [{"env_name": "SCOPED_TEST_CONFIG", "env_value": "inner"}],
		"skip_error_messages": [],
		"statically_loaded_extensions": ["core_functions", "json"]
	})");

	{
		ScopedTestConfiguration outer(outer_path);
		REQUIRE(config.OnInitCommand() == "SELECT 'outer'");
		REQUIRE(config.GetTestEnv("SCOPED_TEST_CONFIG", "") == "outer");
		auto outer_extensions = config.ExtensionToBeLoadedOnLoad();
		{
			ScopedTestConfiguration inner(inner_path);
			REQUIRE(config.OnInitCommand() == "SELECT 'inherited'");
			REQUIRE(config.GetTestEnv("SCOPED_TEST_CONFIG", "") == "inner");
			REQUIRE(config.ShouldSkipTest("test/sql/scoped_config.test"));
			REQUIRE(config.ErrorMessagesToBeSkipped().empty());
			REQUIRE(config.ExtensionToBeLoadedOnLoad() == vector<string> {"core_functions", "json"});
		}
		REQUIRE(config.OnInitCommand() == "SELECT 'outer'");
		REQUIRE(config.GetTestEnv("SCOPED_TEST_CONFIG", "") == "outer");
		REQUIRE_FALSE(config.ShouldSkipTest("test/sql/scoped_config.test"));
		REQUIRE(config.ErrorMessagesToBeSkipped() == vector<string> {"outer error"});
		REQUIRE(config.ExtensionToBeLoadedOnLoad() == outer_extensions);

		try {
			ScopedTestConfiguration inner(inner_path);
			throw std::runtime_error("test body failed");
		} catch (std::runtime_error &) {
		}
		REQUIRE(config.OnInitCommand() == "SELECT 'outer'");
		REQUIRE(config.GetTestEnv("SCOPED_TEST_CONFIG", "") == "outer");
	}
	REQUIRE(config.OnInitCommand() == initial_command);
}

TEST_CASE("Failed extension config loads do not change invocation state", "[test_config]") {
	auto &config = TestConfiguration::Get();
	auto initial_command = config.OnInitCommand();
	auto initial_temp_root = GetTempDirRoot();
	auto invalid_path = WriteScopedTestConfig("invalid-scoped-config.json", R"({
		"on_init": "SELECT 'must not leak'",
		"temp_dir_root": "must-not-replace-the-invocation-root"
	})");
	REQUIRE_THROWS_AS(ScopedTestConfiguration(invalid_path), std::runtime_error);
	REQUIRE(config.OnInitCommand() == initial_command);
	REQUIRE(GetTempDirRoot() == initial_temp_root);

	auto valid_path = WriteScopedTestConfig("valid-scoped-config.json", R"({"on_init": "SELECT 'valid'"})");
	{
		ScopedTestConfiguration valid(valid_path);
		REQUIRE(config.OnInitCommand() == "SELECT 'valid'");
	}
	REQUIRE(config.OnInitCommand() == initial_command);
}

} // namespace duckdb
