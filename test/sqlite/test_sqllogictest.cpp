#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"
#include "sqllogic_test_runner.hpp"
#include "test_helpers.hpp"
#include "test_config.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include <duckdb/main/extension_helper.hpp>

using namespace duckdb;

// code below traverses the test directory and makes individual test cases out
// of each script
static void listFiles(FileSystem &fs, const string &path, std::function<void(const string &)> cb) {
	fs.ListFiles(path, [&](string fname, bool is_dir) {
		string full_path = fs.JoinPath(path, fname);
		if (is_dir) {
			// recurse into directory
			listFiles(fs, full_path, cb);
		} else {
			cb(full_path);
		}
	});
}

static bool endsWith(const string &mainStr, const string &toMatch) {
	return (mainStr.size() >= toMatch.size() &&
	        mainStr.compare(mainStr.size() - toMatch.size(), toMatch.size(), toMatch) == 0);
}

static bool IsSQLLogicTestFile(const string &path) {
	return endsWith(path, ".test") || endsWith(path, ".test_slow") || endsWith(path, ".test_coverage");
}

static void register_sqllogic_test_case(void (*test_fun)(), const string &path, const string &tags) {
	auto normalized_path = StringUtil::Replace(path, "\\", "/");
	if (TestConfiguration::Get().ShouldSkipTest(normalized_path)) {
		return;
	}
	REGISTER_TEST_CASE(test_fun, normalized_path, tags);
}

template <bool AUTO_SWITCH_TEST_DIR>
static void RunSQLLogicTest(const string &name, optional_ptr<std::istream> input) {
	const auto test_dir_path = TestDirectoryPath(); // can vary between tests, and does IO
	// Tracks TEMP_DIR test-for-test, on the same materialization schedule.
	const auto local_test_dir_path = LocalTestDirectoryPath();
	// Absolute (main-cwd-anchored) form of the per-test temp dir we just materialized. Captured HERE,
	// before the AUTO_SWITCH_TEST_DIR block below may chdir into an extension source dir: it names the
	// physical dir regardless of the cwd in effect when {TEST_DIR}/TEMP_DIR are later substituted.
	// (After the extension chdir the relative test_dir_path would resolve to a non-existent sibling.)
	string test_dir_absolute = TestMakeAbsolute(test_dir_path, TestGetCurrentDirectory());
	// HOME is NOT touched per-test: main sets it once to the run-wide sandbox (GetTempDirHome(), a
	// sibling of the run root). Pointing HOME at {TEST_DIR} here would put ~/.duckdb inside a dir tests
	// whitelist via allowed_directories, breaking permission tests (e.g. INSTALL-is-denied).
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
	runner.output_sql = Catch::getCurrentContext().getConfig()->outputSQL();

	string prev_directory;
	// The temp dir exposed to this test (TEMP_DIR / {TEST_DIR}). Normally the cwd-relative path; for the
	// extension case below it becomes the absolute path, since the chdir makes the relative form point
	// at a non-existent sibling of the extension source dir.
	string temp_dir_for_test = test_dir_path;

	// We assume the test working dir for extensions to be one dir above the test/sql. Note that this is very hacky.
	// however for now it suffices: we use it to run tests from out-of-tree extensions that are based on the extension
	// template which adheres to this convention.
	if (!input && AUTO_SWITCH_TEST_DIR) {
		prev_directory = TestGetCurrentDirectory();

		std::size_t found = name.rfind("/test/sql");
		if (found == std::string::npos) {
			throw InvalidInputException("Failed to auto detect working dir for test '" + name +
			                            "' because a non-standard path was used!");
		}
		auto test_working_dir = name.substr(0, found);
		test_config.ChangeWorkingDirectory(test_working_dir);
		// After the chdir, the temp dir (materialized above at the main cwd) is only reachable by its
		// absolute path -- pin TEMP_DIR/{TEST_DIR} to it so `load {TEST_DIR}/x.db` & friends resolve.
		temp_dir_for_test = test_dir_absolute;
		test_config.SetTestDirOverride(test_dir_absolute);
	}

	// setup this test runner with Config-based env, then override with ephemerals (only WORKING_DIR at this point)
	for (auto &kv : test_config.GetTestEnvMap()) {
		runner.environment_variables[kv.first] = kv.second;
	}
	// Per runner vars. Every name set here is reserved against --env-passthrough
	// (IsReservedEnvName, test_helpers.cpp).
	runner.environment_variables["WORKING_DIR"] = TestGetCurrentDirectory();
	runner.environment_variables["TEST_NAME"] = name;
	runner.environment_variables["TEST_NAME__NO_SLASH"] = StringUtil::Replace(name, "/", "_");
	runner.environment_variables["TEST_ID"] = TestNameToId(name); // full test name, sanitized to a path component
	// TEMP_DIR -> assigned per-test to $BASE[/$RUN_ID][/$TEST_ID] -- RUN_ID and TEST_ID when enabled
	// (absolute in the extension case; see temp_dir_for_test above).
	runner.environment_variables["TEMP_DIR"] = temp_dir_for_test;
	// A separate local tree is already absolute, so only the base-is-local case needs chdir pinning.
	runner.environment_variables["LOCAL_TEMP_DIR"] =
	    (local_test_dir_path == test_dir_path) ? temp_dir_for_test : local_test_dir_path;
	// TEMP_DIR_ABSOLUTE is always the main-cwd-anchored absolute path captured before any extension
	// chdir -- computing it from the post-chdir cwd here would point at the wrong (extension) tree.
	runner.environment_variables["TEMP_DIR_ABSOLUTE"] = test_dir_absolute;
	runner.environment_variables["CATALOG_DIR"] = temp_dir_for_test + "/" + runner.environment_variables["TEST_UUID"];

	runner.EmitBegin(name);

	ErrorData error;
	try {
		if (input) {
			runner.ExecuteStream(*input, name);
		} else {
			runner.ExecuteFile(name);
		}
	} catch (std::exception &ex) {
		error = ErrorData(ex);
	} catch (...) {
		// Catch's own assertion-failure control exception (not std::exception): the test aborted
		// mid-run. Catch carries no message; emit the locator stashed at the throw site (file:line —
		// the full diff is in the captured output), then rethrow so Catch still records the verdict.
		runner.EmitExecutionComplete(name, "error", runner.test_failure_locator);
		runner.EmitEnd(name, "error", runner.test_failure_locator);
		throw;
	}

	if (error.HasError()) {
		runner.EmitExecutionComplete(name, "error", error.Message());
	} else if (runner.test_skipped_requirement) {
		runner.EmitExecutionComplete(name, "skip-requirement", runner.test_skip_reason);
	} else {
		runner.EmitExecutionComplete(name, "ok", "");
	}

	if (!input && AUTO_SWITCH_TEST_DIR) {
		test_config.ClearTestDirOverride();
		test_config.ChangeWorkingDirectory(prev_directory);
	}

	auto on_cleanup = test_config.OnCleanupCommand();
	if (!on_cleanup.empty() && runner.db) {
		// perform clean-up if any is defined
		try {
			if (!runner.con) {
				runner.Reconnect();
			}
			auto res = runner.con->Query(on_cleanup);
			res->ThrowIfError();
		} catch (std::exception &ex) {
			string cleanup_failure = "Error while running clean-up routine:\n";
			ErrorData cleanup_error(ex);
			cleanup_failure += cleanup_error.Message();
			// FAIL throws, unwinding past the unified terminal-event emit below. Emit the end event
			// here first so a cleanup failure still yields one — preserving the one-begin/one-end
			// invariant that --emit-test-events consumers rely on.
			runner.EmitEnd(name, "error", cleanup_failure);
			FAIL(cleanup_failure);
		}
	}

	// $TEST_ID destroy: execute this test's destroy disposition using its pass/fail. On the
	// on-success default a passing test's dir is reclaimed now; a failing test's is retained
	// (main's DestroyTempDir then reclaims the whole run root only on overall success).
	runner.test_succeeded = !error.HasError();
	DestroyTestTempDir(runner.test_succeeded);

	// Single terminal event (--emit-test-events): status = skip-requirement (whole-test skip) /
	// error (a std::exception escaped: LoadDatabase / cleanup / unexpected) / ok. Catch-thrown
	// statement failures are handled by the catch(...) above.
	if (runner.test_skipped_requirement) {
		runner.EmitEnd(name, "skip-requirement", runner.test_skip_reason);
	} else if (error.HasError()) {
		runner.EmitEnd(name, "error", error.Message());
	} else {
		runner.EmitEnd(name, "ok", "");
	}
	if (error.HasError()) {
		FAIL(error.Message());
	}
}

template <bool AUTO_SWITCH_TEST_DIR = false>
static void testRunner() {
	// this is an ugly hack that uses the test case name to pass the script file
	// name if someone has a better idea...
	const auto name = Catch::getResultCapture().getCurrentTestName();
	RunSQLLogicTest<AUTO_SWITCH_TEST_DIR>(name, nullptr);
}

static void testRunnerFromStdin() {
	RunSQLLogicTest<false>("<stdin>", &std::cin);
}

static string ParseGroupFromPath(string file) {
	string extension = "";
	if (file.find(".test_slow") != std::string::npos) {
		// "slow" in the name indicates a slow test (i.e. only run as part of allunit)
		extension = "[.]";
	}
	if (file.find(".test_coverage") != std::string::npos) {
		// "coverage" in the name indicates a coverage test (i.e. only run as part of coverage)
		return "[coverage][.]";
	}
	// move backwards to the last slash
	int group_begin = -1, group_end = -1;
	for (idx_t i = file.size(); i > 0; i--) {
		if (file[i - 1] == '/' || file[i - 1] == '\\') {
			if (group_end == -1) {
				group_end = i - 1;
			} else {
				group_begin = i;
				return "[" + file.substr(group_begin, group_end - group_begin) + "]" + extension;
			}
		}
	}
	if (group_end == -1) {
		return "[" + file + "]" + extension;
	}
	return "[" + file.substr(0, group_end) + "]" + extension;
}

namespace duckdb {

static const vector<string> &SQLiteLogicTestExcludes() {
	static vector<string> excludes = {
	    // tested separately
	    "test/select1.test", "test/select2.test", "test/select3.test", "test/select4.test",
	    // feature not supported
	    "evidence/slt_lang_replace.test",       // INSERT OR REPLACE
	    "evidence/slt_lang_reindex.test",       // REINDEX
	    "evidence/slt_lang_update.test",        // Multiple assignments to same column "x" in update
	    "evidence/slt_lang_createtrigger.test", // TRIGGER
	    "evidence/slt_lang_droptrigger.test",   // TRIGGER
	                                            // no + for varchar columns
	    "test/index/random/10/slt_good_14.test", "test/index/random/10/slt_good_1.test",
	    "test/index/random/10/slt_good_0.test", "test/index/random/10/slt_good_12.test",
	    "test/index/random/10/slt_good_6.test", "test/index/random/10/slt_good_13.test",
	    "test/index/random/10/slt_good_5.test", "test/index/random/10/slt_good_10.test",
	    "test/index/random/10/slt_good_11.test", "test/index/random/10/slt_good_4.test",
	    "test/index/random/10/slt_good_8.test", "test/index/random/10/slt_good_3.test",
	    "test/index/random/10/slt_good_2.test", "test/index/random/100/slt_good_1.test",
	    "test/index/random/100/slt_good_0.test", "test/index/random/1000/slt_good_0.test",
	    "test/index/random/1000/slt_good_7.test", "test/index/random/1000/slt_good_6.test",
	    "test/index/random/1000/slt_good_5.test", "test/index/random/1000/slt_good_8.test",
	    // overflow in 32-bit integer multiplication (sqlite does automatic upcasting)
	    "test/random/aggregates/slt_good_96.test", "test/random/aggregates/slt_good_75.test",
	    "test/random/aggregates/slt_good_64.test", "test/random/aggregates/slt_good_9.test",
	    "test/random/aggregates/slt_good_110.test", "test/random/aggregates/slt_good_101.test",
	    "test/random/expr/slt_good_55.test", "test/random/expr/slt_good_115.test", "test/random/expr/slt_good_103.test",
	    "test/random/expr/slt_good_80.test", "test/random/expr/slt_good_75.test", "test/random/expr/slt_good_42.test",
	    "test/random/expr/slt_good_49.test", "test/random/expr/slt_good_24.test", "test/random/expr/slt_good_30.test",
	    "test/random/expr/slt_good_8.test", "test/random/expr/slt_good_61.test",
	    // dependencies between tables/views prevent dropping in DuckDB without CASCADE
	    "test/index/view/1000/slt_good_0.test", "test/index/view/100/slt_good_0.test",
	    "test/index/view/100/slt_good_5.test", "test/index/view/100/slt_good_1.test",
	    "test/index/view/100/slt_good_3.test", "test/index/view/100/slt_good_4.test",
	    "test/index/view/100/slt_good_2.test", "test/index/view/10000/slt_good_0.test",
	    "test/index/view/10/slt_good_5.test", "test/index/view/10/slt_good_7.test",
	    "test/index/view/10/slt_good_1.test", "test/index/view/10/slt_good_3.test",
	    "test/index/view/10/slt_good_4.test", "test/index/view/10/slt_good_6.test",
	    "test/index/view/10/slt_good_2.test",
	    // strange error in hash comparison, results appear correct...
	    "test/index/random/10/slt_good_7.test", "test/index/random/10/slt_good_9.test"};
	return excludes;
}

static bool IsExcludedSQLiteLogicTest(const string &path) {
	for (auto &excl : SQLiteLogicTestExcludes()) {
		if (path.find(excl) != string::npos) {
			return true;
		}
	}
	return false;
}

static bool PathStartsWith(const string &path, const string &prefix) {
	if (prefix.empty() || path.find(prefix) != 0) {
		return false;
	}
	return path.size() == prefix.size() || path[prefix.size()] == '/';
}

static bool IsSQLiteLogicTestPath(FileSystem &fs, const string &path) {
	auto sqlite_test_root = fs.JoinPath(fs.JoinPath("third_party", "sqllogictest"), "test");
	auto normalized_path = StringUtil::Replace(path, "\\", "/");
	auto normalized_root = StringUtil::Replace(sqlite_test_root, "\\", "/");
	return PathStartsWith(normalized_path, normalized_root);
}

static bool IsExtensionTestPath(const string &path) {
	auto normalized_path = StringUtil::Replace(path, "\\", "/");
	for (const auto &extension_test_path : LoadedExtensionTestPaths()) {
		auto normalized_root = StringUtil::Replace(extension_test_path, "\\", "/");
		if (PathStartsWith(normalized_path, normalized_root)) {
			return true;
		}
	}
	return false;
}

void RegisterSqllogictests(const vector<string> &test_paths) {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	for (const auto &path : test_paths) {
		if (IsSQLiteLogicTestPath(*fs, path)) {
			if (!IsExcludedSQLiteLogicTest(path)) {
				register_sqllogic_test_case(testRunner<>, path, "[sqlitelogic][.]");
			}
		} else if (IsExtensionTestPath(path)) {
			register_sqllogic_test_case(testRunner<true>, path, ParseGroupFromPath(path));
		} else {
			register_sqllogic_test_case(testRunner<false>, path, ParseGroupFromPath(path));
		}
	}
}

void RegisterSqllogictests() {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	listFiles(*fs, fs->JoinPath(fs->JoinPath("third_party", "sqllogictest"), "test"), [&](const string &path) {
		if (endsWith(path, ".test")) {
			if (IsExcludedSQLiteLogicTest(path)) {
				return;
			}
			register_sqllogic_test_case(testRunner<>, path, "[sqlitelogic][.]");
		}
	});
	listFiles(*fs, "test", [&](const string &path) {
		if (IsSQLLogicTestFile(path)) {
			// parse the name / group from the test
			register_sqllogic_test_case(testRunner<false>, path, ParseGroupFromPath(path));
		}
	});

	for (const auto &extension_test_path : LoadedExtensionTestPaths()) {
		listFiles(*fs, extension_test_path, [&](const string &path) {
			if (IsSQLLogicTestFile(path)) {
				auto fun = testRunner<true>;
				register_sqllogic_test_case(fun, path, ParseGroupFromPath(path));
			}
		});
	}
}

void RegisterSqllogictestStdin() {
	register_sqllogic_test_case(testRunnerFromStdin, "<stdin>", "[sqlitelogic][stdin]");
}
} // namespace duckdb
