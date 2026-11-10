#include "sqllogic_test_executor.hpp"

#include "duckdb/common/string_util.hpp"
#include "sqllogic_test_runner.hpp"
#include "test_config.hpp"
#include "test_helpers.hpp"
#include "test_reporter.hpp"

namespace duckdb {

SQLLogicTestStats RunSQLLogicTestFile(const string &name, bool auto_switch_test_dir, bool output_sql) {
	const auto test_dir_path = TestDirectoryPath();
	const auto local_test_dir_path = LocalTestDirectoryPath();
	string test_dir_absolute = TestMakeAbsolute(test_dir_path, TestGetCurrentDirectory());
	auto &test_config = TestConfiguration::Get();

	string initial_dbpath = test_config.GetInitialDBPath();
	test_config.ProcessPath(initial_dbpath, name);
	if (!initial_dbpath.empty()) {
		auto test_path = StringUtil::Replace(initial_dbpath, test_dir_path, string());
		test_path = StringUtil::Replace(test_path, "\\", "/");
		auto components = StringUtil::Split(test_path, "/");
		components.pop_back();
		string total_path = test_dir_path;
		for (auto &component : components) {
			if (component.empty()) {
				continue;
			}
			total_path = TestJoinPath(total_path, component);
			TestCreateDirectory(total_path);
		}
	}

	SQLLogicTestRunner runner(std::move(initial_dbpath));
	runner.output_sql = output_sql;

	string previous_directory;
	string temp_dir_for_test = test_dir_path;
	if (auto_switch_test_dir) {
		previous_directory = TestGetCurrentDirectory();
		auto position = name.rfind("/test/sql");
		if (position == string::npos) {
			throw InvalidInputException("Failed to auto detect working dir for test '%s'", name);
		}
		test_config.ChangeWorkingDirectory(name.substr(0, position));
		temp_dir_for_test = test_dir_absolute;
		test_config.SetTestDirOverride(test_dir_absolute);
	}

	for (auto &entry : test_config.GetTestEnvMap()) {
		runner.environment_variables[entry.first] = entry.second;
	}
	runner.environment_variables["WORKING_DIR"] = TestGetCurrentDirectory();
	runner.environment_variables["TEST_NAME"] = name;
	runner.environment_variables["TEST_NAME__NO_SLASH"] = StringUtil::Replace(name, "/", "_");
	runner.environment_variables["TEST_ID"] = TestNameToId(name);
	runner.environment_variables["TEMP_DIR"] = temp_dir_for_test;
	runner.environment_variables["LOCAL_TEMP_DIR"] =
	    local_test_dir_path == test_dir_path ? temp_dir_for_test : local_test_dir_path;
	runner.environment_variables["TEMP_DIR_ABSOLUTE"] = test_dir_absolute;
	runner.environment_variables["CATALOG_DIR"] = temp_dir_for_test + "/" + runner.environment_variables["TEST_UUID"];

	runner.EmitBegin(name);
	ErrorData error;
	try {
		runner.ExecuteFile(name);
	} catch (std::exception &ex) {
		error = ErrorData(ex);
	} catch (...) {
		runner.EmitExecutionComplete(name, "error", runner.test_failure_locator);
		runner.EmitEnd(name, "error", runner.test_failure_locator);
		if (auto_switch_test_dir) {
			test_config.ClearTestDirOverride();
			test_config.ChangeWorkingDirectory(previous_directory);
		}
		throw;
	}

	if (error.HasError()) {
		runner.EmitExecutionComplete(name, "error", error.Message());
	} else if (runner.test_skipped_requirement) {
		runner.EmitExecutionComplete(name, "skip-requirement", runner.test_skip_reason);
	} else {
		runner.EmitExecutionComplete(name, "ok", "");
	}

	if (auto_switch_test_dir) {
		test_config.ClearTestDirOverride();
		test_config.ChangeWorkingDirectory(previous_directory);
	}

	auto cleanup = test_config.OnCleanupCommand();
	if (!cleanup.empty() && runner.db) {
		try {
			if (!runner.con) {
				runner.Reconnect();
			}
			auto result = runner.con->Query(cleanup);
			result->ThrowIfError();
		} catch (std::exception &ex) {
			ErrorData cleanup_error(ex);
			auto message = "Error while running clean-up routine:\n" + cleanup_error.Message();
			runner.EmitEnd(name, "error", message);
			TEST_FAIL(message);
		}
	}

	runner.test_succeeded = !error.HasError();
	DestroyTestTempDir(runner.test_succeeded);
	if (runner.test_skipped_requirement) {
		runner.EmitEnd(name, "skip-requirement", runner.test_skip_reason);
	} else if (error.HasError()) {
		runner.EmitEnd(name, "error", error.Message());
	} else {
		runner.EmitEnd(name, "ok", "");
	}
	if (error.HasError()) {
		TEST_FAIL(error.Message());
	}
	SQLLogicTestStats stats;
	stats.skip_mode = runner.test_stat_skip_mode.load();
	stats.partial_skip_reasons = runner.GetPartialSkipReasons();
	return stats;
}

} // namespace duckdb
