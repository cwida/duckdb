#include "test_config.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/enum_util.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/uuid.hpp"
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace duckdb {

typedef void (*on_set_option_t)(const Value &input);

struct TestConfigOption {
	const char *name;
	const char *description;
	LogicalType type;
	on_set_option_t on_set_option;
};

static const TestConfigOption test_config_options[] = {
    {"description", "Config description", LogicalType::VARCHAR, nullptr},
    {"comment", "Extra free form comment line", LogicalType::VARCHAR, nullptr},
    {"initial_db", "Initial database path", LogicalType::VARCHAR, nullptr},
    {"data_dir",
     "Root dir for test data (DATA_DIR / {DATA_DIR}); default {WORKING_DIR}/data. Relative values "
     "resolve per test cwd; absolute/remote (e.g. az://...) values are used verbatim.",
     LogicalType::VARCHAR, nullptr},
    {"local_data_dir",
     "Local-FS counterpart of data_dir (LOCAL_DATA_DIR / {LOCAL_DATA_DIR}); default {WORKING_DIR}/data. "
     "Must be local, and must equal data_dir when that is itself local -- a local data_dir is already "
     "its own LOCAL_DATA_DIR.",
     LogicalType::VARCHAR, TestConfiguration::CheckLocalDataDir},
    {"temp_dir_root",
     "Root of the temp-dir tree (TEMP_DIR_ROOT / $ROOT); default duckdb_unittest_tempdir. May be local "
     "or remote (e.g. az://...); a remote root is never created or reaped by unittest.",
     LogicalType::VARCHAR, TestConfiguration::SetTempDirRootOption},
    {"local_temp_dir_root",
     "Root of the LOCAL_TEMP_DIR tree when temp_dir_root is remote; default duckdb_unittest_tempdir. "
     "Must be local; an error against a differing local temp_dir_root, which is already its own "
     "LOCAL_TEMP_DIR.",
     LogicalType::VARCHAR, TestConfiguration::SetLocalTempDirRootOption},
    {"run_id", "Run identity (RUN_ID env, and the [RUN_ID] path level); 'auto' or absent generates one",
     LogicalType::VARCHAR, TestConfiguration::SetRunIdOption},
    {"temp_dir_run_id", "Include the [RUN_ID] level in TEMP_DIR: on, off", LogicalType::VARCHAR,
     TestConfiguration::SetTempDirRunIdOption},
    {"temp_dir_test_id", "Include the [TEST_ID] level in TEMP_DIR: on, off", LogicalType::VARCHAR,
     TestConfiguration::SetTempDirTestIdOption},
    {"temp_dir_destroy", "Temp-dir destroy disposition: never, on-success, always", LogicalType::VARCHAR,
     TestConfiguration::SetTempDirDestroyOption},
    {"database_destroy", "Loaded-database destroy disposition: on, off, on-success", LogicalType::VARCHAR,
     TestConfiguration::SetDatabaseDestroyOption},
    {"env_passthrough",
     "Env var names pre-registered for {NAME} substitution in every test, read from the real process "
     "env; a named-but-absent var fails the whole invocation at startup",
     LogicalType::LIST(LogicalType::VARCHAR), TestConfiguration::AppendEnvPassthrough},
    {"max_threads", "Max threads to use during tests", LogicalType::BIGINT, nullptr},
    {"max_test_threads", "Max threads to be used by the test runner itself (for e.g. concurrent loop)",
     LogicalType::BIGINT, nullptr},
    {"base_config", "Config file to load and base initial settings on", LogicalType::VARCHAR,
     TestConfiguration::LoadBaseConfig},
    {"block_size", "Block Alloction Size; must be a power of 2", LogicalType::BIGINT, nullptr},
    {"checkpoint_wal_size", "Size in bytes after which to trigger automatic checkpointing", LogicalType::BIGINT,
     nullptr},
    {"checkpoint_on_shutdown", "Whether or not to checkpoint on database shutdown", LogicalType::BOOLEAN, nullptr},
    {"force_restart", "Force restart the database between runs", LogicalType::BOOLEAN, nullptr},
    {"summarize_failures", "Print a summary of all test failures after running", LogicalType::BOOLEAN, nullptr},
    {"test_memory_leaks", "Run memory leak tests", LogicalType::BOOLEAN, nullptr},
    {"storage_fuzzer", "Run storage fuzzer tests", LogicalType::BOOLEAN, nullptr},
    {"verify_vector", "Run vector verification for a specific vector type", LogicalType::VARCHAR, nullptr},
    {"debug_initialize", "Initialize buffers with all 0 or all 1", LogicalType::VARCHAR, nullptr},
    {"autoloading", "Loading strategy for extensions not bundled in", LogicalType::VARCHAR, nullptr},
    {"init_script", "Script to execute on init", LogicalType::VARCHAR, TestConfiguration::ParseConnectScript},
    {"init_sqllogic",
     "Path to a sqllogic script run (through the full test runner) after the database is "
     "created, before the test body",
     LogicalType::VARCHAR, nullptr},
    {"cleanup_sqllogic", "Path to a sqllogic script run (through the full test runner) after the test body",
     LogicalType::VARCHAR, nullptr},
    {"on_cleanup", "SQL statements to execute on test end", LogicalType::VARCHAR, nullptr},
    {"on_init", "SQL statements to execute on init", LogicalType::VARCHAR, nullptr},
    {"on_load", "SQL statements to execute on explicit load", LogicalType::VARCHAR, nullptr},
    {"on_new_connection", "SQL statements to execute on connection", LogicalType::VARCHAR, nullptr},
    {"test_env", "The test variables",
     LogicalType::LIST(LogicalType::STRUCT({{"env_name", LogicalType::VARCHAR}, {"env_value", LogicalType::VARCHAR}})),
     nullptr},
    {"inherit_skip_tests", "Path of config to inherit 'skip_tests' from", LogicalType::VARCHAR},
    {"skip_tests", "Tests to be skipped",
     LogicalType::LIST(
         LogicalType::STRUCT({{"reason", LogicalType::VARCHAR}, {"paths", LogicalType::LIST(LogicalType::VARCHAR)}})),
     nullptr},
    {"skip_compiled", "Skip compiled tests", LogicalType::BOOLEAN, nullptr},
    {"skip_error_messages", "Skip compiled tests", LogicalType::LIST(LogicalType::VARCHAR), nullptr},
    {"sort_style", "Default sort style if none is configured in the test (none, rowsort, valuesort)",
     LogicalType::VARCHAR, TestConfiguration::CheckSortStyle},
    {"statically_loaded_extensions", "Extensions to be loaded (from the statically available one)",
     LogicalType::LIST(LogicalType::VARCHAR), nullptr},
    {"storage_version", "Database storage version to use by default", LogicalType::VARCHAR, nullptr},
    {"select_tag", "Select tests which match named tag (as singleton set; multiple sets are OR'd)",
     LogicalType::VARCHAR, TestConfiguration::AppendSelectTagSet},
    {"select_tag_set", "Select tests which match _all_ named tags (multiple sets are OR'd)",
     LogicalType::LIST(LogicalType::VARCHAR), TestConfiguration::AppendSelectTagSet},
    {"skip_tag", "Skip tests which match named tag (as singleton set; multiple sets are OR'd)", LogicalType::VARCHAR,
     TestConfiguration::AppendSkipTagSet},
    {"skip_tag_set", "Skip tests which match _all_ named tags (multiple sets are OR'd)",
     LogicalType::LIST(LogicalType::VARCHAR), TestConfiguration::AppendSkipTagSet},
    {"settings", "Configuration settings to apply",
     LogicalType::LIST(LogicalType::STRUCT({{"name", LogicalType::VARCHAR}, {"value", LogicalType::VARCHAR}})),
     nullptr},
    {"attach_options", "ATTACH-style options for the main database (e.g. IO_MODE)",
     LogicalType::LIST(LogicalType::STRUCT({{"name", LogicalType::VARCHAR}, {"value", LogicalType::VARCHAR}})),
     nullptr},
    {"extends", "List of config files to extend from", LogicalType::LIST(LogicalType::VARCHAR), nullptr},
    {nullptr, nullptr, LogicalType::INVALID, nullptr},
};

TestConfiguration &TestConfiguration::Get() {
	static TestConfiguration instance;
	return instance;
}

void TestConfiguration::Initialize() {
	// load config file
	auto config_file = std::getenv("DUCKDB_TEST_CONFIG");
	if (config_file) {
		LoadConfig(config_file);
	}

	// load configuration options from the environment
	for (idx_t index = 0; test_config_options[index].name != nullptr; index++) {
		auto &config = test_config_options[index];
		string env_name = "DUCKDB_TEST_" + StringUtil::Upper(config.name);
		auto env_arg = std::getenv(env_name.c_str());
		if (!env_arg) {
			continue;
		}
		ParseOption(config.name, Value(env_arg));
	}

	// load summarize failures
	const char *summarize = std::getenv("SUMMARIZE_FAILURES");
	if (summarize) {
		if (std::string(summarize) == "1") {
			ParseOption("summarize_failures", Value(true));
		}
	} else {
		// SUMMARIZE_FAILURES not passed in explicitly - enable by default on CI
		const char *ci = std::getenv("CI");
		if (ci) {
			ParseOption("summarize_failures", Value(true));
		}
	}

	working_dir = FileSystem::GetWorkingDirectory();
	test_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	// NOTE: UpdateEnvironment() is intentionally NOT called here. It runs once in main()
	// after PrepareTempDir(), so TEMP_DIR/TEMP_DIR_ROOT reflect the final --temp-dir-*
	// context. test_env is consumed only at test-run time, so deferring is safe.
	// (ChangeWorkingDirectory still refreshes it on an actual cwd change.)
}

void TestConfiguration::UpdateEnvironment() {
	// Setup standard vars

	// XXX: UUID used by ducklake to avoid collisions, is there a better way?
	test_env["TEST_UUID"] = test_uuid;
	test_env["BUILD_DIR"] = TestBuildDirectory();
	test_env["WORKING_DIR"] = working_dir; // can be overridden per runner
	// DATA_DIR: an explicit override (--data-dir / DUCKDB_TEST_DATA_DIR / config data_dir) is read fresh
	// here, so it survives cwd changes -- an absolute/remote value stays put, a relative value is resolved
	// per test cwd. Absent an override, default to <cwd>/data, which re-anchors on an extension chdir.
	// Every name set here is reserved against every external input (IsReservedEnvName, test_helpers.cpp).
	auto data_dir_override = GetOptionOrDefault("data_dir", string());
	string default_data_dir = working_dir + "/data";
	test_env["DATA_DIR"] = data_dir_override.empty() ? default_data_dir : data_dir_override;

	string temp_dir = TestDirectoryPath();
	string temp_dir_absolute = TestMakeAbsolute(temp_dir, working_dir);
	// TEMP_DIR here is the run-id root ($ROOT/[RUN_ID]); the per-test path overrides it per
	// runner with the full $ROOT/[RUN_ID]/[TEST_ID] once a test name is known.
	test_env["TEMP_DIR"] = temp_dir;                      // run-root ($ROOT/$RUN_ID); per-test gets +=$TEST_ID
	test_env["TEMP_DIR_ABSOLUTE"] = temp_dir_absolute;    // absolute form of TEMP_DIR
	test_env["TEMP_DIR_ROOT"] = GetTempDirRoot();         // $ROOT; default duckdb_unittest_tempdir
	test_env["RUN_ID"] = GetTempDirRunId();               // RUN_ID (--run-id, or generated); always set
	test_env["CATALOG_DIR"] = temp_dir + "/" + test_uuid; // _not_ guaranteed to exist

	// Local-FS variants of DATA_DIR / TEMP_DIR. Computed, never overlaid: these are reserved names
	// (IsReservedEnvName), so an option is the only way to influence them and there is no later
	// `test_env` overlay to reconcile against.
	auto local_data_dir_override = GetOptionOrDefault("local_data_dir", string());
	if (!local_data_dir_override.empty()) {
		test_env["LOCAL_DATA_DIR"] = local_data_dir_override;
	} else {
		test_env["LOCAL_DATA_DIR"] =
		    Path::FromString(test_env["DATA_DIR"]).IsRemote() ? default_data_dir : test_env["DATA_DIR"];
	}
	test_env["LOCAL_TEMP_DIR"] = LocalTestDirectoryPath();

	// Passthrough configured env vars as test env (no `require-env` required.)
	// main() already validated, but null check defensively.
	for (auto &name : GetEnvPassthroughNames()) {
		auto value = std::getenv(name.c_str());
		if (value) {
			test_env[name] = value;
		}
	}

	// Re-overlay caller `test_env` config entries so they win over the standard defaults just recomputed
	// above (important when a cwd change re-runs UpdateEnvironment).
	LoadTestEnvFromConfig();
}

string TestConfiguration::GetWorkingDirectory() {
	return working_dir;
}

bool TestConfiguration::ChangeWorkingDirectory(const string &dir) {
	bool rv = false;
	// set CWD first, then get it -- this gets us normalized absolute path for free
	// making the comparison below meaningful
	FileSystem::SetWorkingDirectory(dir);
	const auto &normalized = FileSystem::GetWorkingDirectory();
	if (working_dir != normalized) {
		rv = true;
		working_dir = normalized;
		UpdateEnvironment();
	}
	return rv;
}

bool TestConfiguration::ParseArgument(const string &arg, idx_t argc, char **argv, idx_t &i) {
	if (arg == "--test-config") {
		if (i >= argc) {
			throw std::runtime_error("--test-config expected a path to a configuration file");
		}
		auto config_path = string(argv[++i]);
		LoadConfig(config_path);
		return true;
	}
	if (arg == "--force-storage") {
		ParseOption("initial_db", Value("{TEST_DIR}/{BASE_TEST_NAME}__test__config__force_storage.db"));
		return true;
	}
	if (arg == "--force-reload") {
		ParseOption("force_restart", Value(true));
		return true;
	}
	if (arg == "--single-threaded") {
		ParseOption("max_threads", Value::BIGINT(1));
		return true;
	}
	if (arg == "--zero-initialize") {
		ParseOption("debug_initialize", Value("DEBUG_ZERO_INITIALIZE"));
		return true;
	}
	if (arg == "--one-initialize") {
		ParseOption("debug_initialize", Value("DEBUG_ONE_INITIALIZE"));
		return true;
	}
	if (StringUtil::StartsWith(arg, "--memory-leak") || StringUtil::StartsWith(arg, "--test-memory-leak")) {
		ParseOption("test_memory_leaks", Value(true));
		return true;
	}

	for (idx_t index = 0; test_config_options[index].name != nullptr; index++) {
		auto &config_option = test_config_options[index];
		string option_name = "--" + StringUtil::Replace(config_option.name, "_", "-");
		if (config_option.type == LogicalTypeId::BOOLEAN) {
			// for booleans we allow "--[option]" and "--no-[option]"
			string no_option_name = "--no-" + StringUtil::Replace(config_option.name, "_", "-");
			if (arg == option_name) {
				ParseOption(config_option.name, Value(true));
				return true;
			} else if (arg == no_option_name) {
				ParseOption(config_option.name, Value(false));
				return true;
			}
		} else if (arg == option_name) {
			if (i >= argc) {
				throw std::runtime_error(option_name + " expected an argument");
			}
			auto option_value = string(argv[++i]);
			ParseOption(config_option.name, option_value);
			return true;
		}
	}
	return false;
}

bool TestConfiguration::TryParseOption(const string &name, const Value &value) {
	// find the option
	optional_idx config_index;
	for (idx_t index = 0; test_config_options[index].name != nullptr; index++) {
		if (StringUtil::CIEquals(test_config_options[index].name, name)) {
			config_index = index;
			break;
		}
	}
	if (!config_index.IsValid()) {
		return false;
	}
	auto &test_config = test_config_options[config_index.GetIndex()];
	// Config values arrive as strings, parsed as SQL types. Lists in particular are
	// user-unfriendly, requiring [] wrappers instead of just-plain-commas.
	// Help out here, and add [] for list params. This intentionally also
	// allows a meaningful '' empty arg to work for list-type params,
	// eg --skip-error-messages '' -> [], and 'HTTP' -> [HTTP]
	Value to_cast = value;
	if (test_config.type.id() == LogicalTypeId::LIST && value.type().id() == LogicalTypeId::VARCHAR &&
	    !value.IsNull()) {
		auto str = value.GetValue<string>();
		if (str.empty() || str[0] != '[') {
			to_cast = Value("[" + str + "]");
		}
	}
	auto parameter = to_cast.DefaultCastAs(test_config.type);
	if (test_config.on_set_option && !ignore_option_side_effects) {
		test_config.on_set_option(parameter);
	}
	options[test_config.name] = parameter;
	if (StringUtil::CIEquals(test_config.name, "test_env")) {
		test_env_from_config_loaded = false;
		test_env_from_config_keys.clear();
	}
	return true;
}

void TestConfiguration::ParseOption(const string &name, const Value &value) {
	if (!TryParseOption(name, value)) {
		throw std::runtime_error("Failed to find option " + name + " - it does not exist");
	}
}

void TestConfiguration::SetLocalExtensionRepository(const string &repo) {
	local_extension_repo = repo;
}

string TestConfiguration::GetLocalExtensionRepository() const {
	return local_extension_repo;
}

TestConfiguration::ExtensionAutoLoadingMode TestConfiguration::GetExtensionAutoLoadingMode() {
	string res = StringUtil::Lower(GetOptionOrDefault("autoloading", string("default")));
	if (res == "none" || res == "default") {
		return TestConfiguration::ExtensionAutoLoadingMode::NONE;
	} else if (res == "available") {
		return TestConfiguration::ExtensionAutoLoadingMode::AVAILABLE;
	} else if (res == "all") {
		return TestConfiguration::ExtensionAutoLoadingMode::ALL;
	}
	throw std::runtime_error("Unknown autoloading mode");
}

bool TestConfiguration::ShouldSkipTest(const string &test_name) {
	if (test_name.find('/') == 0) {
		// Full path specified, strip down to base path so the extension config lookup still works
		auto pos = test_name.find("test/sql");
		if (pos != string::npos) {
			const string stripped_test_name = test_name.c_str() + pos;
			return tests_to_be_skipped.count(stripped_test_name);
		}
	}
	return tests_to_be_skipped.count(test_name);
}

string TestConfiguration::OnInitCommand() {
	return GetOptionOrDefault("on_init", string());
}

string TestConfiguration::GetInitSqllogic() {
	return GetOptionOrDefault("init_sqllogic", string());
}

string TestConfiguration::GetCleanupSqllogic() {
	return GetOptionOrDefault("cleanup_sqllogic", string());
}

string TestConfiguration::OnLoadCommand() {
	auto res = GetOptionOrDefault("on_load", string(""));
	if (res != "" && res != "skip") {
		throw std::runtime_error("Unsupported parameter to on_load");
	}
	return res;
}

string TestConfiguration::OnConnectionCommand() {
	return GetOptionOrDefault("on_new_connection", string());
}

string TestConfiguration::OnCleanupCommand() {
	return GetOptionOrDefault("on_cleanup", string());
}

SortStyle TestConfiguration::GetDefaultSortStyle() {
	SortStyle default_sort_style_enum = SortStyle::NO_SORT;
	if (!TryParseSortStyle(GetOptionOrDefault<string>("sort_style", "none"), default_sort_style_enum)) {
		throw std::runtime_error("eek: unknown sort style in TestConfig");
	}
	return default_sort_style_enum;
}

vector<string> TestConfiguration::ExtensionToBeLoadedOnLoad() {
	vector<string> res;
	auto entry = options.find("statically_loaded_extensions");
	if (entry != options.end()) {
		vector<Value> ext_list = ListValue::GetChildren(entry->second);

		for (auto ext : ext_list) {
			res.push_back(ext.GetValue<string>());
		}
	} else {
		res.push_back("core_functions");
	}
	return res;
}

vector<string> TestConfiguration::ErrorMessagesToBeSkipped() {
	vector<string> res;
	auto entry = options.find("skip_error_messages");
	if (entry != options.end()) {
		vector<Value> ext_list = ListValue::GetChildren(entry->second);

		for (auto ext : ext_list) {
			res.push_back(ext.GetValue<string>());
		}
	} else {
		res.push_back("HTTP");
		res.push_back("Unable to connect");
#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
#ifndef AVOID_DUCKDB_DEBUG_ASYNC_THROW
		// The first of those it's throw as a test of what would happen if a task were to be throwing
		// It's OK that PositionalTableScanner react re-throwing a different exception, so they need to be removed in
		// this specific test setup
		res.push_back("ThrowAsyncTask: Test error handling when throwing mid-task");
		res.push_back("Unexpected interrupt from table Source in PositionalTableScanner refill");
#endif
#endif
	}
	return res;
}

void TestConfiguration::LoadBaseConfig(const Value &input) {
	auto &test_config = TestConfiguration::Get();
	test_config.LoadConfig(input.ToString());
}

void TestConfiguration::ParseConnectScript(const Value &input) {
	auto init_cmd = ReadFileToString(input.ToString());

	auto &test_config = TestConfiguration::Get();
	test_config.ParseOption("on_init", Value(init_cmd));
}

// Each defers to the test_helpers setter of record and turns its bool return into the error, so a
// knob's valid-value list lives in exactly one place.
static void SetTempDirOption(const string &option, bool (*setter)(const string &), const Value &input,
                             const char *expects) {
	if (!setter(input.ToString())) {
		throw std::runtime_error(option + " expects one of: " + expects);
	}
}

void TestConfiguration::SetTempDirRootOption(const Value &input) {
	SetTempDirRoot(input.ToString());
}

void TestConfiguration::SetLocalTempDirRootOption(const Value &input) {
	SetLocalTempDirRoot(input.ToString());
}

void TestConfiguration::SetRunIdOption(const Value &input) {
	SetRunId(input.ToString());
}

void TestConfiguration::SetTempDirRunIdOption(const Value &input) {
	SetTempDirOption("--temp-dir-run-id", SetTempDirRunIdInPath, input, "on, off");
}

void TestConfiguration::SetTempDirTestIdOption(const Value &input) {
	SetTempDirOption("--temp-dir-test-id", SetTempDirTestId, input, "on, off");
}

void TestConfiguration::SetTempDirDestroyOption(const Value &input) {
	SetTempDirOption("--temp-dir-destroy", SetTempDirDestroy, input, "never, on-success, always");
}

void TestConfiguration::SetDatabaseDestroyOption(const Value &input) {
	SetTempDirOption("--database-destroy", SetDatabaseDestroy, input, "on, off, on-success");
}

void TestConfiguration::AppendEnvPassthrough(const Value &input) {
	// Accumulates rather than replaces; repeated uses of flag extends
	for (auto &name : ListValue::GetChildren(input)) {
		AddEnvPassthrough(name.ToString());
	}
}

void TestConfiguration::CheckLocalDataDir(const Value &input) {
	if (Path::FromString(input.ToString()).IsRemote()) {
		throw std::runtime_error("--local-data-dir must be a local path, got: " + input.ToString());
	}
}

bool TestConfiguration::ValidateTestEnv(string &error) {
	LoadTestEnvFromConfig();
	string reserved;
	for (auto &name : test_env_from_config_keys) {
		if (IsReservedEnvName(name)) {
			reserved += (reserved.empty() ? "" : ", ") + name;
		}
	}
	if (reserved.empty()) {
		return true;
	}
	error = "config `test_env`: reserved by the test runner, cannot be overridden: " + reserved +
	        "\nThese are resolved per invocation (or per test) by the runner itself; use the matching "
	        "option instead (e.g. --data-dir, --temp-dir-root, --local-data-dir).";
	return false;
}

bool TestConfiguration::ValidateDataDirs(string &error) {
	// Only an explicit LOCAL_DATA_DIR can trip this: the derived value IS DATA_DIR whenever that is local.
	const auto &data_dir = test_env["DATA_DIR"];
	const auto &local_data_dir = test_env["LOCAL_DATA_DIR"];
	if (Path::FromString(data_dir).IsRemote() || Path::Normalize(data_dir) == Path::Normalize(local_data_dir)) {
		return true;
	}
	error = "local-data-dir must equal data-dir when that is local, which is already its own LOCAL_DATA_DIR "
	        "(data-dir: " +
	        data_dir + ", local-data-dir: " + local_data_dir + ")";
	return false;
}

void TestConfiguration::CheckSortStyle(const Value &input) {
	SortStyle sort_style;
	if (!TryParseSortStyle(input.ToString(), sort_style)) {
		throw std::runtime_error(StringUtil::Format("Invalid parameter for sort style %s", input.ToString()));
	}
}

bool TestConfiguration::TryParseSortStyle(const string &sort_style, SortStyle &result) {
	if (sort_style == "nosort" || sort_style == "none") {
		/* Do no sorting */
		result = SortStyle::NO_SORT;
	} else if (sort_style == "rowsort" || sort_style == "sort") {
		/* Row-oriented sorting */
		result = SortStyle::ROW_SORT;
	} else if (sort_style == "valuesort") {
		/* Sort all values independently */
		result = SortStyle::VALUE_SORT;
	} else {
		// if this is not a known sort style
		return false;
	}
	return true;
}

string TestConfiguration::ReadFileToString(const string &path) {
	std::ifstream infile(path);
	if (infile.bad() || infile.fail()) {
		throw std::runtime_error("Failed to open configuration file " + path);
	}
	std::stringstream buffer;
	buffer << infile.rdbuf();
	return buffer.str();
}

void TestConfiguration::LoadConfig(const string &config_path) {
	try {
		// read the config file
		auto buffer = ReadFileToString(config_path);
		// parse json
		auto json_values = StringUtil::ParseJSONMap(buffer);

		auto extends_it = json_values.find("extends");
		if (extends_it != json_values.end()) {
			auto config_dir = StringUtil::GetFilePath(config_path);
			auto extends_list = Value(extends_it->second).DefaultCastAs(LogicalType::LIST(LogicalType::VARCHAR));
			for (auto &child : ListValue::GetChildren(extends_list)) {
				auto path = child.GetValue<string>();
				if (!config_dir.empty() && !path.empty() && path[0] != '/') {
					path = config_dir + "/" + path;
				}
				LoadConfig(path);
			}
		}

		// load the base config (if any) before processing the rest, so that this config's options
		// and skip_tests are layered on top of the base instead of depending on map iteration order
		auto base_it = json_values.find("base_config");
		if (base_it != json_values.end()) {
			LoadConfig(base_it->second);
			json_values.erase(base_it);
		}

		for (auto &entry : json_values) {
			ParseOption(entry.first, Value(entry.second));
		}
		auto inherit_entry = options.find("inherit_skip_tests");
		if (inherit_entry != options.end()) {
			auto path_value = inherit_entry->second;
			D_ASSERT(path_value.type().id() == LogicalTypeId::VARCHAR);
			D_ASSERT(!path_value.IsNull());
			auto cwd = TestGetCurrentDirectory();
			auto path = TestJoinPath(cwd, path_value.ToString());
			TestConfiguration inherit_config;
			// Harvesting skip_tests only: the option hooks below write process-global lifecycle state
			// (temp-dir roots, run id, dispositions, env-passthrough), so an inherited config would
			// otherwise replace what the actual invocation selected.
			inherit_config.ignore_option_side_effects = true;
			inherit_config.LoadConfig(path);

			tests_to_be_skipped.insert(inherit_config.tests_to_be_skipped.begin(),
			                           inherit_config.tests_to_be_skipped.end());
		}
		bool found_duplicate_tests = false;
		// Convert to unordered_set<string> the list of tests to be skipped
		auto entry = options.find("skip_tests");
		if (entry != options.end()) {
			auto skip_list_entry = ListValue::GetChildren(entry->second);
			for (const auto &value : skip_list_entry) {
				auto children = StructValue::GetChildren(value);
				auto skip_list = ListValue::GetChildren(children[1]);
				for (const auto &skipped_test : skip_list) {
					auto skipped_test_str = skipped_test.GetValue<string>();
					auto insert_result = tests_to_be_skipped.insert(skipped_test_str);
					if (!insert_result.second) {
						Printer::PrintF("* Test \"%s\" was already present in skipped test list", skipped_test_str);
						found_duplicate_tests = true;
					}
				}
			}
			options.erase("skip_tests");
		}
		if (found_duplicate_tests) {
			Printer::PrintF("Run python3 scripts/cleanup_config_skip_tests.py to clean up");
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw std::runtime_error(
		    StringUtil::Format("Failed to parse config file \"%s\": %s", config_path, error.Message()));
	}
}

void TestConfiguration::LoadTestEnvFromConfig() {
	if (!test_env_from_config_loaded) {
		test_env_from_config_loaded = true;
		test_env_from_config_keys.clear();
		config_test_env.clear();
		auto entry = options.find("test_env");
		if (entry != options.end()) {
			auto list_children = ListValue::GetChildren(entry->second);
			for (const auto &value : list_children) {
				auto &struct_children = StructValue::GetChildren(value);
				auto &env = StringValue::Get(struct_children[0]);
				auto &env_value = StringValue::Get(struct_children[1]);
				test_env_from_config_keys.insert(env);
				config_test_env[env] = env_value;
			}
		}
	}
	// Caller-provided test_env entries are authoritative: (re)overlay them onto test_env each call, so an
	// explicit override survives UpdateEnvironment recomputing the standard defaults on a cwd change.
	for (auto &kv : config_test_env) {
		test_env[kv.first] = kv.second;
	}
}

void TestConfiguration::SetTestDirOverride(const string &absolute_test_dir) {
	test_dir_override = absolute_test_dir;
}

void TestConfiguration::ClearTestDirOverride() {
	test_dir_override.clear();
}

// Every {KEYWORD} substituted here is reserved against every external input (IsReservedEnvName,
// test_helpers.cpp): ReplaceKeywords substitutes env vars first, so a passthrough would shadow them.
void TestConfiguration::ProcessPath(string &path, const string &test_name) {
	// {TEST_DIR} normally follows the current cwd (TestDirectoryPath()); an active override pins it to
	// the absolute, main-cwd-anchored temp dir instead (extension runner, post-chdir -- see the header).
	// See also test/README.md for context on this switch.
	const string test_dir = test_dir_override.empty() ? TestDirectoryPath() : test_dir_override;
	path = StringUtil::Replace(path, "{TEST_DIR}", test_dir);
	path = StringUtil::Replace(path, "{WORKING_DIRECTORY}", FileSystem::GetWorkingDirectory());
	path = StringUtil::Replace(path, "{UUID}", UUID::ToString(UUID::GenerateRandomUUID()));
	path = StringUtil::Replace(path, "{TEST_NAME}", test_name);

	auto base_test_name = StringUtil::Replace(test_name, "/", "_");
	path = StringUtil::Replace(path, "{BASE_TEST_NAME}", base_test_name);
	if (StringUtil::Contains(path, "__TEST_DIR__")) {
		Printer::PrintF("Replacing deprecated string __TEST_DIR__ in path \"%s\" - please replace with {TEST_DIR}",
		                path);
		path = StringUtil::Replace(path, "__TEST_DIR__", test_dir);
	}
	if (StringUtil::Contains(path, "__WORKING_DIRECTORY__")) {
		Printer::PrintF("Replacing deprecated string __WORKING_DIRECTORY__ in path \"%s\" - please replace with "
		                "{WORKING_DIRECTORY}",
		                path);
		path = StringUtil::Replace(path, "__WORKING_DIRECTORY__", FileSystem::GetWorkingDirectory());
	}
}

template <class T, class VAL_T>
T TestConfiguration::GetOptionOrDefault(const string &name, T default_val) {
	auto entry = options.find(name);
	if (entry == options.end()) {
		return default_val;
	}
	return entry->second.GetValue<VAL_T>();
}

string TestConfiguration::GetDescription() {
	return GetOptionOrDefault("description", string());
}

string TestConfiguration::GetInitialDBPath() {
	return GetOptionOrDefault("initial_db", string());
}

optional_idx TestConfiguration::GetMaxThreads() {
	return GetOptionOrDefault<optional_idx, idx_t>("max_threads", optional_idx());
}

optional_idx TestConfiguration::GetMaxTestThreads() {
	return GetOptionOrDefault<optional_idx, idx_t>("max_test_threads", optional_idx());
}

optional_idx TestConfiguration::GetBlockAllocSize() {
	return GetOptionOrDefault<optional_idx, idx_t>("block_size", DEFAULT_BLOCK_ALLOC_SIZE);
}

idx_t TestConfiguration::GetCheckpointWALSize() {
	return GetOptionOrDefault<idx_t>("checkpoint_wal_size", 0);
}

bool TestConfiguration::GetForceRestart() {
	return GetOptionOrDefault("force_restart", false);
}

bool TestConfiguration::GetCheckpointOnShutdown() {
	return GetOptionOrDefault("checkpoint_on_shutdown", true);
}

bool TestConfiguration::GetTestMemoryLeaks() {
	return GetOptionOrDefault("test_memory_leaks", false);
}

bool TestConfiguration::RunStorageFuzzer() {
	return GetOptionOrDefault("storage_fuzzer", false);
}

bool TestConfiguration::GetSummarizeFailures() {
	return GetOptionOrDefault("summarize_failures", false);
}

bool TestConfiguration::GetSkipCompiledTests() {
	return GetOptionOrDefault("skip_compiled", false);
}

string TestConfiguration::GetStorageVersion() {
	return GetOptionOrDefault("storage_version", string());
}

vector<ConfigSetting> TestConfiguration::GetConfigSettings() {
	vector<ConfigSetting> result;
	if (options.find("settings") != options.end()) {
		auto entry = options["settings"];
		auto list_children = ListValue::GetChildren(entry);
		for (const auto &value : list_children) {
			auto &struct_children = StructValue::GetChildren(value);
			ConfigSetting config_setting;
			config_setting.name = struct_children[0].GetValue<Identifier>();
			config_setting.value = StringValue::Get(struct_children[1]);
			result.push_back(std::move(config_setting));
		}
	}
	return result;
}

unordered_map<string, Value> TestConfiguration::GetMainDatabaseOptions() {
	unordered_map<string, Value> result;
	if (options.find("attach_options") != options.end()) {
		auto entry = options["attach_options"];
		auto list_children = ListValue::GetChildren(entry);
		for (const auto &value : list_children) {
			auto &struct_children = StructValue::GetChildren(value);
			result[StringValue::Get(struct_children[0])] = struct_children[1];
		}
	}
	return result;
}

string TestConfiguration::GetTestEnv(const string &key, const string &default_value) {
	LoadTestEnvFromConfig();
	if (test_env.find(key) == test_env.end()) {
		return default_value;
	}
	return test_env[key];
}

bool TestConfiguration::HasTestEnv(const string &key) {
	LoadTestEnvFromConfig();
	return test_env_from_config_keys.find(key) != test_env_from_config_keys.end();
}

const unordered_map<string, string> &TestConfiguration::GetTestEnvMap() {
	LoadTestEnvFromConfig();
	return test_env;
}

DebugVectorVerification TestConfiguration::GetVectorVerification() {
	return EnumUtil::FromString<DebugVectorVerification>(GetOptionOrDefault<string>("verify_vector", "NONE"));
}

DebugInitialize TestConfiguration::GetDebugInitialize() {
	return EnumUtil::FromString<DebugInitialize>(GetOptionOrDefault<string>("debug_initialize", "NO_INITIALIZE"));
}

vector<unordered_set<string>> TestConfiguration::GetSelectTagSets() {
	return select_tag_sets;
}

vector<unordered_set<string>> TestConfiguration::GetSkipTagSets() {
	return skip_tag_sets;
}

std::unordered_set<string> make_tag_set(const Value &src_val) {
	// handle both cases -- singleton VARCHAR/string, and set of strings
	auto dst_set = std::unordered_set<string>();
	if (src_val.type() == LogicalType::VARCHAR) {
		dst_set.insert(src_val.GetValue<string>());
	} else /* LIST(VARCHAR) */ {
		for (auto &tag : ListValue::GetChildren(src_val)) {
			dst_set.insert(tag.GetValue<string>());
		}
	}
	return dst_set;
}

void TestConfiguration::AppendSelectTagSet(const Value &tag_set) {
	TestConfiguration::Get().select_tag_sets.push_back(make_tag_set(tag_set));
}

void TestConfiguration::AppendSkipTagSet(const Value &tag_set) {
	TestConfiguration::Get().skip_tag_sets.push_back(make_tag_set(tag_set));
}

bool is_subset(const unordered_set<string> &sub, const vector<string> &super) {
	for (const auto &elt : sub) {
		if (std::find(super.begin(), super.end(), elt) == super.end()) {
			return false;
		}
	}
	return true;
}

// NOTE: this model of policy assumes simply that all selects are applied to the All set, then
// all skips are applied to that result. (Typical alternative: CLI ordering where each
// select/skip operation is applied in sequence.)
TestConfiguration::SelectPolicy TestConfiguration::GetPolicyForTagSet(const vector<string> &subject_tag_set) {
	// Apply select_tag_set first then skip_tag_set; if both empty always NONE
	auto policy = TestConfiguration::SelectPolicy::NONE;
	// select: if >= 1 select_tag_set is subset of subject_tag_set
	// if count(select_tag_sets) > 0 && no matches, SKIP
	for (const auto &select_tag_set : select_tag_sets) {
		policy = TestConfiguration::SelectPolicy::SKIP; // >=1 sets => SKIP || SELECT
		if (is_subset(select_tag_set, subject_tag_set)) {
			policy = TestConfiguration::SelectPolicy::SELECT;
			break;
		}
	}
	// skip: if >=1 skip_tag_set is subset of subject_tag_set, else passthrough
	for (const auto &skip_tag_set : skip_tag_sets) {
		if (is_subset(skip_tag_set, subject_tag_set)) {
			return TestConfiguration::SelectPolicy::SKIP;
		}
	}
	return policy;
}

bool TestConfiguration::TestForceStorage() {
	auto &test_config = TestConfiguration::Get();
	return !test_config.GetInitialDBPath().empty();
}

bool TestConfiguration::TestForceReload() {
	auto &test_config = TestConfiguration::Get();
	return test_config.GetForceRestart();
}

bool TestConfiguration::TestMemoryLeaks() {
	auto &test_config = TestConfiguration::Get();
	return test_config.GetTestMemoryLeaks();
}

bool TestConfiguration::TestRunStorageFuzzer() {
	auto &test_config = TestConfiguration::Get();
	return test_config.RunStorageFuzzer();
}

FailureSummary::FailureSummary() : failures_summary_counter(0) {
}

FailureSummary &FailureSummary::Instance() {
	static FailureSummary instance;
	return instance;
}

string FailureSummary::GetFailureSummary() {
	auto &test_config = TestConfiguration::Get();
	if (!test_config.GetSummarizeFailures()) {
		return string();
	}
	auto &summary = FailureSummary::Instance();
	lock_guard<mutex> guard(summary.failures_lock);
	std::ostringstream oss;
	for (auto &line : summary.failures_summary) {
		oss << line;
	}
	return oss.str();
}

void FailureSummary::Log(string log_message) {
	auto &summary = FailureSummary::Instance();
	lock_guard<mutex> lock(summary.failures_lock);
	summary.failures_summary.push_back(std::move(log_message));
}

idx_t FailureSummary::GetSummaryCounter() {
	auto &summary = FailureSummary::Instance();
	return ++summary.failures_summary_counter;
}

// don't log the same test failure many times:
// e.g. log only the first failure in
// `./build/debug/test/unittest --on-init "SET max_memory='400kb';"
// test/fuzzer/pedro/concurrent_catalog_usage.test`
bool FailureSummary::SkipLoggingSameError(const string &file_name) {
	return Instance().SkipLoggingSameErrorInternal(file_name);
}

bool FailureSummary::SkipLoggingSameErrorInternal(const string &file_name) {
	if (file_name.empty()) {
		return false;
	}
	lock_guard<mutex> lock(failures_lock);
	if (reported_files.count(file_name) > 0) {
		return true;
	}
	reported_files.insert(file_name);
	return false;
}

} // namespace duckdb
