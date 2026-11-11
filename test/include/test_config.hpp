//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// test_config.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/enums/debug_vector_verification.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/enums/debug_initialize.hpp"
#include <sys/types.h>
#include <unordered_map>

namespace duckdb {

enum class SortStyle : uint8_t { NO_SORT, ROW_SORT, VALUE_SORT };

struct ConfigSetting {
	Identifier name;
	Value value;
};

class TestConfiguration {
public:
	enum class ExtensionAutoLoadingMode { NONE = 0, AVAILABLE = 1, ALL = 2 };

	enum class SelectPolicy : uint8_t {
		NONE,   // does not match any explicit policy (default: policy=SELECT)
		SELECT, // matches explicit select
		SKIP    // matches explicit skip
	};

	static TestConfiguration &Get();

	void Initialize();
	bool ParseArgument(const string &arg, idx_t argc, char **argv, idx_t &i);
	bool TryParseOption(const string &name, const Value &value);
	void ParseOption(const string &name, const Value &value);
	void LoadConfig(const string &config_path);

	void UpdateEnvironment();
	string GetWorkingDirectory();
	bool ChangeWorkingDirectory(const string &dir); // true -> changed

	void ProcessPath(string &path, const string &test_name);
	// Override the value {TEST_DIR}/__TEST_DIR__ expands to during ProcessPath. Used by the extension
	// (AUTO_SWITCH_TEST_DIR) runner: after it chdir's into the extension source dir, the default
	// (cwd-relative) TestDirectoryPath() would resolve to a non-existent sibling, so the runner pins
	// {TEST_DIR} to the absolute, main-cwd-anchored temp dir. Empty (the default) uses TestDirectoryPath().
	void SetTestDirOverride(const string &absolute_test_dir);
	void ClearTestDirOverride();

	string GetDescription();
	string GetInitialDBPath();
	optional_idx GetMaxThreads();
	optional_idx GetBlockAllocSize();
	optional_idx GetMaxTestThreads();
	idx_t GetCheckpointWALSize();
	bool GetForceRestart();
	bool GetCheckpointOnShutdown();
	bool GetTestMemoryLeaks();
	bool RunStorageFuzzer();
	bool GetSummarizeFailures();
	bool GetSkipCompiledTests();
	bool GetWarningsAsErrors();
	DebugVectorVerification GetVectorVerification();
	DebugInitialize GetDebugInitialize();
	ExtensionAutoLoadingMode GetExtensionAutoLoadingMode();
	bool ShouldSkipTest(const string &test_name);
	string DataLocation();
	string OnInitCommand();
	string OnLoadCommand();
	string OnConnectionCommand();
	string OnCleanupCommand();
	string GetInitSqllogic();
	string GetCleanupSqllogic();
	SortStyle GetDefaultSortStyle();
	vector<string> ExtensionToBeLoadedOnLoad();
	vector<string> ErrorMessagesToBeSkipped();
	string GetStorageVersion();
	string GetTestEnv(const string &key, const string &default_value);
	bool HasTestEnv(const string &key);
	const unordered_map<string, string> &GetTestEnvMap();
	vector<unordered_set<string>> GetSelectTagSets();
	vector<unordered_set<string>> GetSkipTagSets();
	SelectPolicy GetPolicyForTagSet(const vector<string> &tag_set);
	vector<ConfigSetting> GetConfigSettings();
	unordered_map<string, Value> GetMainDatabaseOptions();

	static bool TestForceStorage();
	static bool TestForceReload();
	static bool TestMemoryLeaks();
	static bool TestRunStorageFuzzer();

	static void LoadBaseConfig(const Value &input);
	static void ParseConnectScript(const Value &input);
	static void CheckSortStyle(const Value &input);
	static bool TryParseSortStyle(const string &sort_style, SortStyle &result);
	static void AppendSelectTagSet(const Value &tag_set);
	static void AppendSkipTagSet(const Value &tag_set);

	//! --temp-dir-* family + run-id + env-passthrough: on_set_option hooks bridging the generic option
	//! table to the test_helpers setters of record (see test_helpers.hpp).
	static void SetTempDirRootOption(const Value &input);
	static void SetLocalTempDirRootOption(const Value &input);
	static void SetRunIdOption(const Value &input);
	static void SetTempDirRunIdOption(const Value &input);
	static void SetTempDirTestIdOption(const Value &input);
	static void SetTempDirDestroyOption(const Value &input);
	static void SetDatabaseDestroyOption(const Value &input);
	static void AppendEnvPassthrough(const Value &input);
	static void CheckLocalDataDir(const Value &input);

	//! false + `error` set when LOCAL_DATA_DIR was pinned away from a DATA_DIR that is itself local.
	//! Call after UpdateEnvironment(), which is where both are resolved.
	bool ValidateDataDirs(string &error);
	//! false + `error` set when a config `test_env` entry names a runner-resolved variable
	//! (IsReservedEnvName). Engine-populated names are reserved against every external input.
	bool ValidateTestEnv(string &error);

	string GetLocalExtensionRepository() const;
	void SetLocalExtensionRepository(const string &repo);

private:
	void LoadTestEnvFromConfig();

	//! Parse options into this instance without running their on_set_option hooks. Set on a
	//! throwaway config loaded only for its `skip_tests` -- the hooks write process-global
	//! lifecycle state that belongs to the selected invocation, not to an inherited file.
	bool ignore_option_side_effects = false;

	//! Give preference to settings from loaded configs
	bool test_env_from_config_loaded = false;
	unordered_set<string> test_env_from_config_keys;
	//! Parsed once from the `test_env` config list; re-overlaid onto test_env on every
	//! LoadTestEnvFromConfig() so caller overrides survive UpdateEnvironment's default recompute.
	unordered_map<string, string> config_test_env;
	case_insensitive_map_t<Value> options;
	unordered_set<string> tests_to_be_skipped;

	// explicitly take ownership of working_dir here, giving runners an API to chdir,
	// and get env updates to match
	string working_dir;
	string test_uuid;
	// When non-empty, the value {TEST_DIR}/__TEST_DIR__ resolve to in ProcessPath (see SetTestDirOverride).
	string test_dir_override;
	unordered_map<string, string> test_env;

	vector<unordered_set<string>> select_tag_sets;
	vector<unordered_set<string>> skip_tag_sets;

	string local_extension_repo;

private:
	template <class T, class VAL_T = T>
	T GetOptionOrDefault(const string &name, T default_val);

	static string ReadFileToString(const string &path);
};

class FailureSummary {
public:
	FailureSummary();

	static void Log(string message);
	static string GetFailureSummary();
	static idx_t GetSummaryCounter();
	static bool SkipLoggingSameError(const string &file_name);

private:
	static FailureSummary &Instance();
	bool SkipLoggingSameErrorInternal(const string &file_name);

private:
	mutex failures_lock;
	atomic<idx_t> failures_summary_counter;
	vector<string> failures_summary;
	set<string> reported_files;
};

} // namespace duckdb
