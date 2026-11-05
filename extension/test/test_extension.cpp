#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_load_options.hpp"
#include "duckdb_test/test_bundle.hpp"
#include "debug_fs_extension.hpp"
#include "sqllogic_test_executor.hpp"
#include "test_config.hpp"
#include "test_helpers.hpp"
#include "test_reporter.hpp"

#include <chrono>
#include <cstdlib>

namespace duckdb {

namespace {

struct RegisteredBundle {
	string name;
	const duckdb_test::TestBundle *bundle;
};

struct TestRegistryState {
	mutex lock;
	vector<RegisteredBundle> bundles;
};

struct TestDescription {
	string test_id;
	string name;
	string kind;
	string bundle;
	string tags;
	string source;
	bool is_slow;
	bool auto_switch_test_dir = false;
};

struct TestResult {
	string test_id;
	string status;
	double duration_ms = 0;
	idx_t assertion_count = 0;
	vector<string> failures;
	string message;
};

struct TestScanData : public TableFunctionData {
	vector<TestDescription> tests;
};

struct TestScanState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

struct TestRunData : public TableFunctionData {
	vector<TestDescription> tests;
	vector<string> test_flags;
};

struct TestRunState : public GlobalTableFunctionState {
	~TestRunState() override {
		if (environment_initialized) {
			DestroyTempDir(success);
		}
	}

	vector<TestResult> results;
	idx_t offset = 0;
	bool environment_initialized = false;
	bool success = false;
};

struct SQLTestReporter : public TestReporter {
	explicit SQLTestReporter(string test_name_p) : test_name(std::move(test_name_p)) {
	}

	string CurrentTestName() override {
		return test_name;
	}

	string test_name;
};

static TestRegistryState &GetTestRegistry(ClientContext &context) {
	auto service =
	    static_cast<duckdb_test::TestRegistry *>(ExtensionManager::Get(context).GetService(duckdb_test::SERVICE_NAME));
	if (!service) {
		throw InternalException("DuckDB test registry is unavailable");
	}
	return *static_cast<TestRegistryState *>(service->data);
}

static void RegisterBundle(void *data, const duckdb_test::TestBundle *bundle) {
	if (!bundle || !bundle->name || !bundle->enumerate || !bundle->run) {
		throw InvalidInputException("Invalid DuckDB test bundle registration");
	}
	auto &registry = *static_cast<TestRegistryState *>(data);
	lock_guard<mutex> guard(registry.lock);
	for (auto &entry : registry.bundles) {
		if (entry.name == bundle->name) {
			throw InvalidInputException("DuckDB test bundle '%s' is already registered", bundle->name);
		}
	}
	registry.bundles.push_back({bundle->name, bundle});
}

static vector<RegisteredBundle> GetRegisteredBundles(TestRegistryState &registry) {
	lock_guard<mutex> guard(registry.lock);
	return registry.bundles;
}

static vector<string> ReadStringList(const Value &value, const string &argument_name) {
	if (value.IsNull()) {
		throw InvalidInputException("%s cannot be NULL", argument_name);
	}
	vector<string> result;
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw InvalidInputException("%s cannot contain NULL values", argument_name);
		}
		result.push_back(StringValue::Get(child));
	}
	return result;
}

static void ConfigureTestFlags(const vector<string> &test_flags) {
	static mutex config_lock;
	static bool initialized = false;
	static vector<string> configured_flags;
	lock_guard<mutex> guard(config_lock);
	if (initialized) {
		if (test_flags != configured_flags) {
			throw InvalidInputException("The test extension cannot be reconfigured with different test flags");
		}
		return;
	}

	auto &config = TestConfiguration::Get();
	config.Initialize();
	vector<string> arguments {"test"};
	arguments.insert(arguments.end(), test_flags.begin(), test_flags.end());
	vector<char *> argv;
	for (auto &argument : arguments) {
		argv.push_back(&argument[0]);
	}
	for (idx_t index = 1; index < argv.size(); index++) {
		auto argument = string(argv[index]);
		try {
			if (!config.ParseArgument(argument, NumericCast<idx_t>(argv.size()), argv.data(), index)) {
				throw InvalidInputException("Unsupported test flag '%s'", argument);
			}
		} catch (std::exception &ex) {
			throw InvalidInputException("Invalid test flag '%s': %s", argument, ex.what());
		}
	}
	configured_flags = test_flags;
	initialized = true;
}

static string NormalizePath(const string &path) {
	return StringUtil::Replace(path, "\\", "/");
}

static string TestPathForDisplay(const string &path) {
	auto normalized_path = NormalizePath(path);
	auto working_directory = NormalizePath(FileSystem::GetWorkingDirectory());
	if (!StringUtil::EndsWith(working_directory, "/")) {
		working_directory += "/";
	}
	if (StringUtil::StartsWith(normalized_path, working_directory)) {
		return normalized_path.substr(working_directory.size());
	}
	return normalized_path;
}

static void ListFilesRecursive(FileSystem &fs, const string &path,
                               const std::function<void(const string &)> &callback) {
	fs.ListFiles(path, [&](const string &name, bool is_directory) {
		auto full_path = fs.JoinPath(path, name);
		if (is_directory) {
			ListFilesRecursive(fs, full_path, callback);
		} else {
			callback(full_path);
		}
	});
}

static bool MatchesSelector(const TestDescription &test, const vector<string> &selectors) {
	if (selectors.empty()) {
		return true;
	}
	for (auto &selector : selectors) {
		if (selector.empty()) {
			continue;
		}
		if (Glob(test.test_id.c_str(), test.test_id.size(), selector.c_str(), selector.size()) ||
		    Glob(test.name.c_str(), test.name.size(), selector.c_str(), selector.size())) {
			return true;
		}
	}
	return false;
}

static bool IsSQLLogicTest(const string &path) {
	return StringUtil::EndsWith(path, ".test") || StringUtil::EndsWith(path, ".test_slow") ||
	       StringUtil::EndsWith(path, ".test_coverage");
}

static string SQLTestTags(const string &path) {
	string tags;
	auto normalized = NormalizePath(path);
	auto components = StringUtil::Split(normalized, "/");
	if (components.size() >= 2) {
		tags = "[" + components[components.size() - 2] + "]";
	}
	if (StringUtil::EndsWith(path, ".test_slow")) {
		tags += "[.]";
	}
	if (StringUtil::EndsWith(path, ".test_coverage")) {
		tags += "[coverage][.]";
	}
	return tags;
}

static bool IsSQLiteTestRoot(const string &root) {
	return StringUtil::EndsWith(root, "/third_party/sqllogictest/test");
}

static bool IsExcludedSQLiteLogicTest(const string &path) {
	static const vector<string> excludes = {
	    "test/select1.test",
	    "test/select2.test",
	    "test/select3.test",
	    "test/select4.test",
	    "evidence/slt_lang_replace.test",
	    "evidence/slt_lang_reindex.test",
	    "evidence/slt_lang_update.test",
	    "evidence/slt_lang_createtrigger.test",
	    "evidence/slt_lang_droptrigger.test",
	    "test/index/random/10/slt_good_14.test",
	    "test/index/random/10/slt_good_1.test",
	    "test/index/random/10/slt_good_0.test",
	    "test/index/random/10/slt_good_12.test",
	    "test/index/random/10/slt_good_6.test",
	    "test/index/random/10/slt_good_13.test",
	    "test/index/random/10/slt_good_5.test",
	    "test/index/random/10/slt_good_10.test",
	    "test/index/random/10/slt_good_11.test",
	    "test/index/random/10/slt_good_4.test",
	    "test/index/random/10/slt_good_8.test",
	    "test/index/random/10/slt_good_3.test",
	    "test/index/random/10/slt_good_2.test",
	    "test/index/random/100/slt_good_1.test",
	    "test/index/random/100/slt_good_0.test",
	    "test/index/random/1000/slt_good_0.test",
	    "test/index/random/1000/slt_good_7.test",
	    "test/index/random/1000/slt_good_6.test",
	    "test/index/random/1000/slt_good_5.test",
	    "test/index/random/1000/slt_good_8.test",
	    "test/random/aggregates/slt_good_96.test",
	    "test/random/aggregates/slt_good_75.test",
	    "test/random/aggregates/slt_good_64.test",
	    "test/random/aggregates/slt_good_9.test",
	    "test/random/aggregates/slt_good_110.test",
	    "test/random/aggregates/slt_good_101.test",
	    "test/random/expr/slt_good_55.test",
	    "test/random/expr/slt_good_115.test",
	    "test/random/expr/slt_good_103.test",
	    "test/random/expr/slt_good_80.test",
	    "test/random/expr/slt_good_75.test",
	    "test/random/expr/slt_good_42.test",
	    "test/random/expr/slt_good_49.test",
	    "test/random/expr/slt_good_24.test",
	    "test/random/expr/slt_good_30.test",
	    "test/random/expr/slt_good_8.test",
	    "test/random/expr/slt_good_61.test",
	    "test/index/view/1000/slt_good_0.test",
	    "test/index/view/100/slt_good_0.test",
	    "test/index/view/100/slt_good_5.test",
	    "test/index/view/100/slt_good_1.test",
	    "test/index/view/100/slt_good_3.test",
	    "test/index/view/100/slt_good_4.test",
	    "test/index/view/100/slt_good_2.test",
	    "test/index/view/10000/slt_good_0.test",
	    "test/index/view/10/slt_good_5.test",
	    "test/index/view/10/slt_good_7.test",
	    "test/index/view/10/slt_good_1.test",
	    "test/index/view/10/slt_good_3.test",
	    "test/index/view/10/slt_good_4.test",
	    "test/index/view/10/slt_good_6.test",
	    "test/index/view/10/slt_good_2.test",
	    "test/index/random/10/slt_good_7.test",
	    "test/index/random/10/slt_good_9.test",
	};
	for (auto &exclude : excludes) {
		if (StringUtil::EndsWith(path, exclude)) {
			return true;
		}
	}
	return false;
}

static void DiscoverBundles(ClientContext &context, const vector<string> &extension_roots) {
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> bundle_paths;
	for (auto &root : extension_roots) {
		if (!fs.DirectoryExists(root)) {
			continue;
		}
		ListFilesRecursive(fs, root, [&](const string &path) {
			if (StringUtil::EndsWith(path, "_tests.duckdb_extension")) {
				bundle_paths.push_back(path);
			}
		});
	}
	std::sort(bundle_paths.begin(), bundle_paths.end());
	bundle_paths.erase(std::unique(bundle_paths.begin(), bundle_paths.end()), bundle_paths.end());
	for (auto &path : bundle_paths) {
		ExtensionHelper::LoadExternalExtension(context, ExtensionLoadOptions(path));
	}
}

struct EnumerateContext {
	vector<TestDescription> &tests;
	const string &bundle_name;
};

static void AddBundleTest(void *data, const duckdb_test::TestCase *test_case) {
	auto &context = *static_cast<EnumerateContext *>(data);
	auto test_name = string(test_case->name);
	TestDescription test;
	test.test_id = "cpp:" + context.bundle_name + ":" + test_name;
	test.name = std::move(test_name);
	test.kind = "cpp";
	test.bundle = context.bundle_name;
	test.tags = test_case->tags ? test_case->tags : "";
	test.source = test_case->source ? test_case->source : "";
	test.is_slow = test.tags.find("[.]") != string::npos;
	context.tests.push_back(std::move(test));
}

static vector<TestDescription> DiscoverTests(ClientContext &context, TestRegistryState &registry,
                                             const vector<string> &test_roots, const vector<string> &extension_roots,
                                             const vector<string> &test_flags) {
	ConfigureTestFlags(test_flags);
	auto &config = TestConfiguration::Get();
	DiscoverBundles(context, extension_roots);
	vector<TestDescription> result;
	auto &fs = FileSystem::GetFileSystem(context);
	for (auto &root : test_roots) {
		if (!fs.DirectoryExists(root)) {
			continue;
		}
		auto normalized_root = NormalizePath(root);
		auto is_sqlite_root = IsSQLiteTestRoot(normalized_root);
		ListFilesRecursive(fs, root, [&](const string &path) {
			if (!IsSQLLogicTest(path)) {
				return;
			}
			auto normalized_path = TestPathForDisplay(path);
			if (is_sqlite_root && IsExcludedSQLiteLogicTest(normalized_path)) {
				return;
			}
			TestDescription test;
			test.test_id = "sql:" + normalized_path;
			test.name = normalized_path;
			test.kind = "sql";
			test.tags = is_sqlite_root ? "[sqlitelogic][.]" : SQLTestTags(normalized_path);
			test.is_slow = test.tags.find("[.]") != string::npos;
			test.auto_switch_test_dir =
			    StringUtil::EndsWith(normalized_root, "/test/sql") && normalized_path.find("/test/sql") != string::npos;
			test.source = test.auto_switch_test_dir ? NormalizePath(path) : normalized_path;
			if (config.ShouldSkipTest(test.name)) {
				return;
			}
			result.push_back(std::move(test));
		});
	}
	for (auto &entry : GetRegisteredBundles(registry)) {
		EnumerateContext enumerate_context {result, entry.name};
		entry.bundle->enumerate(entry.bundle->data, AddBundleTest, &enumerate_context);
	}
	std::sort(result.begin(), result.end(),
	          [](const TestDescription &left, const TestDescription &right) { return left.test_id < right.test_id; });
	for (idx_t index = 1; index < result.size(); index++) {
		if (result[index - 1].test_id == result[index].test_id) {
			throw InvalidInputException("Duplicate DuckDB test id '%s'", result[index].test_id);
		}
	}
	return result;
}

static unique_ptr<FunctionData> TestScanBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto selectors = ReadStringList(input.inputs[0], "selectors");
	auto test_roots = ReadStringList(input.inputs[1], "test_roots");
	auto extension_roots = ReadStringList(input.inputs[2], "extension_roots");
	auto include_slow = BooleanValue::Get(input.inputs[3]);
	auto test_flags = ReadStringList(input.inputs[4], "test_flags");
	auto &registry = GetTestRegistry(context);

	auto result = make_uniq<TestScanData>();
	for (auto &test : DiscoverTests(context, registry, test_roots, extension_roots, test_flags)) {
		if ((!test.is_slow || include_slow) && MatchesSelector(test, selectors)) {
			result->tests.push_back(std::move(test));
		}
	}
	names = {"test_id", "name", "kind", "bundle", "tags", "source", "is_slow"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> TestScanInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<TestScanState>();
}

static void TestScanFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<TestScanData>();
	auto &state = input.global_state->Cast<TestScanState>();
	idx_t count = 0;
	while (state.offset < data.tests.size() && count < STANDARD_VECTOR_SIZE) {
		auto &test = data.tests[state.offset++];
		output.data[0].Append(Value(test.test_id));
		output.data[1].Append(Value(test.name));
		output.data[2].Append(Value(test.kind));
		output.data[3].Append(test.bundle.empty() ? Value() : Value(test.bundle));
		output.data[4].Append(Value(test.tags));
		output.data[5].Append(Value(test.source));
		output.data[6].Append(Value::BOOLEAN(test.is_slow));
		count++;
	}
	output.SetChildCardinality(count);
}

static void InitializeSQLTestEnvironment() {
	static bool initialized = false;
	if (initialized) {
		return;
	}
	auto &config = TestConfiguration::Get();
	string error;
	if (!ValidateEnvPassthrough(error) || !config.ValidateTestEnv(error)) {
		throw InvalidInputException("%s", error);
	}
	if (!PrepareTempDir(error)) {
		throw InvalidInputException("Failed to prepare test directory: %s", error);
	}
	config.UpdateEnvironment();
	if (!config.ValidateDataDirs(error)) {
		throw InvalidInputException("%s", error);
	}
	auto home_dir = TestMakeAbsolute(GetTempDirHome(), TestGetCurrentDirectory());
#ifdef DUCKDB_WINDOWS
	if (_putenv_s("USERPROFILE", home_dir.c_str()) != 0) {
		throw IOException("Failed to set USERPROFILE environment variable");
	}
#else
	if (setenv("HOME", home_dir.c_str(), 1) != 0) {
		throw IOException("Failed to set HOME environment variable");
	}
#endif
	SetStaticExtensionLoader([](DuckDB &db) { db.LoadStaticExtension<DebugFsExtension>(); });
	initialized = true;
}

static TestResult RunSQLTest(const TestDescription &test) {
	SQLTestReporter reporter(test.name);
	TestReporterScope reporter_scope(reporter);
	auto start = std::chrono::steady_clock::now();
	TestResult result;
	result.test_id = test.test_id;
	try {
		RunSQLLogicTestFile(test.source, test.auto_switch_test_dir, false);
		result.status = reporter.skip_reason.empty() ? "passed" : "skipped";
		result.message = reporter.skip_reason;
	} catch (TestFailureException &ex) {
		result.status = "failed";
		result.message = ex.message;
		result.failures.push_back(ex.file + ":" + to_string(ex.line) + ": " + ex.message);
	} catch (std::exception &ex) {
		result.status = "failed";
		result.message = ex.what();
		result.failures.push_back(ex.what());
	}
	auto end = std::chrono::steady_clock::now();
	result.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
	result.assertion_count = reporter.assertion_count.load();
	return result;
}

struct BundleResultContext {
	vector<TestResult> &results;
	const string &bundle_name;
};

static void AddBundleResult(void *data, const duckdb_test::TestResult *bundle_result) {
	auto &context = *static_cast<BundleResultContext *>(data);
	TestResult result;
	result.test_id = "cpp:" + context.bundle_name + ":" + bundle_result->name;
	switch (bundle_result->status) {
	case duckdb_test::TestStatus::PASSED:
		result.status = "passed";
		break;
	case duckdb_test::TestStatus::FAILED:
		result.status = "failed";
		break;
	case duckdb_test::TestStatus::SKIPPED:
		result.status = "skipped";
		break;
	}
	result.duration_ms = bundle_result->duration_ms;
	result.assertion_count = NumericCast<idx_t>(bundle_result->assertion_count);
	result.message = bundle_result->message ? bundle_result->message : "";
	for (uint64_t index = 0; index < bundle_result->failure_count; index++) {
		auto &failure = bundle_result->failures[index];
		string text;
		if (failure.file && failure.file[0]) {
			text = string(failure.file) + ":" + to_string(failure.line) + ": ";
		}
		if (failure.expression && failure.expression[0]) {
			text += string(failure.expression) + ": ";
		}
		if (failure.message) {
			text += failure.message;
		}
		result.failures.push_back(std::move(text));
	}
	context.results.push_back(std::move(result));
}

static unique_ptr<FunctionData> TestRunBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto test_ids = ReadStringList(input.inputs[0], "test_ids");
	auto test_roots = ReadStringList(input.inputs[1], "test_roots");
	auto extension_roots = ReadStringList(input.inputs[2], "extension_roots");
	auto test_flags = ReadStringList(input.inputs[3], "test_flags");
	auto &registry = GetTestRegistry(context);
	auto available = DiscoverTests(context, registry, test_roots, extension_roots, test_flags);
	unordered_map<string, TestDescription> tests_by_id;
	for (auto &test : available) {
		tests_by_id.emplace(test.test_id, std::move(test));
	}

	auto result = make_uniq<TestRunData>();
	for (auto &test_id : test_ids) {
		auto entry = tests_by_id.find(test_id);
		if (entry == tests_by_id.end()) {
			throw InvalidInputException("Unknown DuckDB test id '%s'", test_id);
		}
		result->tests.push_back(entry->second);
	}
	result->test_flags = std::move(test_flags);
	names = {"test_id", "status", "duration_ms", "assertion_count", "failures", "message"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::DOUBLE,
	                LogicalType::UBIGINT,
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::VARCHAR};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> TestRunInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<TestRunState>();
	auto &data = input.bind_data->Cast<TestRunData>();
	InitializeSQLTestEnvironment();
	result->environment_initialized = true;

	unordered_map<string, vector<string>> bundle_tests;
	for (auto &test : data.tests) {
		if (test.kind == "sql") {
			result->results.push_back(RunSQLTest(test));
		} else {
			bundle_tests[test.bundle].push_back(test.name);
		}
	}

	auto &registry = GetTestRegistry(context);
	for (auto &entry : GetRegisteredBundles(registry)) {
		auto selected = bundle_tests.find(entry.name);
		if (selected == bundle_tests.end()) {
			continue;
		}
		vector<const char *> names;
		for (auto &name : selected->second) {
			names.push_back(name.c_str());
		}
		vector<const char *> test_flags;
		for (auto &test_flag : data.test_flags) {
			test_flags.push_back(test_flag.c_str());
		}
		BundleResultContext callback_context {result->results, entry.name};
		entry.bundle->run(entry.bundle->data, names.data(), names.size(), test_flags.data(), test_flags.size(),
		                  AddBundleResult, &callback_context);
	}

	unordered_map<string, idx_t> result_order;
	for (idx_t index = 0; index < data.tests.size(); index++) {
		result_order[data.tests[index].test_id] = index;
	}
	std::sort(result->results.begin(), result->results.end(), [&](const TestResult &left, const TestResult &right) {
		return result_order[left.test_id] < result_order[right.test_id];
	});
	if (result->results.size() != data.tests.size()) {
		throw InternalException("A DuckDB test bundle did not return a result for every requested test");
	}
	result->success = std::all_of(result->results.begin(), result->results.end(),
	                              [](const TestResult &test) { return test.status != "failed"; });
	return std::move(result);
}

static void TestRunFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<TestRunState>();
	idx_t count = 0;
	while (state.offset < state.results.size() && count < STANDARD_VECTOR_SIZE) {
		auto &result = state.results[state.offset++];
		output.data[0].Append(Value(result.test_id));
		output.data[1].Append(Value(result.status));
		output.data[2].Append(Value::DOUBLE(result.duration_ms));
		output.data[3].Append(Value::UBIGINT(result.assertion_count));
		vector<Value> failures;
		for (auto &failure : result.failures) {
			failures.emplace_back(failure);
		}
		output.data[4].Append(Value::LIST(LogicalType::VARCHAR, std::move(failures)));
		output.data[5].Append(result.message.empty() ? Value() : Value(result.message));
		count++;
	}
	output.SetChildCardinality(count);
}

} // namespace

void LoadTestExtension(ExtensionLoader &loader) {
	auto registry = new TestRegistryState();
	auto service = new duckdb_test::TestRegistry {registry, RegisterBundle};
	loader.RegisterService(duckdb_test::SERVICE_NAME, service);

	TableFunction scan("test_scan",
	                   {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                    LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BOOLEAN,
	                    LogicalType::LIST(LogicalType::VARCHAR)},
	                   TestScanFunction, TestScanBind, TestScanInit);
	loader.RegisterFunction(std::move(scan));

	TableFunction run("test_run",
	                  {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                   LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)},
	                  TestRunFunction, TestRunBind, TestRunInit);
	loader.RegisterFunction(std::move(run));
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(test, loader) {
	duckdb::LoadTestExtension(loader);
}
}
