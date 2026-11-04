#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb_test/test_bundle.hpp"
#include "test_config.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <sstream>

#ifdef DUCKDB_TEST_BUNDLE_DEBUG_FS
#include "debug_fs_extension.hpp"
#endif

#ifndef DUCKDB_TEST_BUNDLE_NAME
#error DUCKDB_TEST_BUNDLE_NAME must be defined
#endif

namespace {

using namespace duckdb;
using namespace duckdb_test;

struct CapturedFailure {
	string file;
	uint64_t line = 0;
	string expression;
	string message;
};

struct CaptureState {
	vector<CapturedFailure> failures;
	idx_t assertions = 0;
	bool skipped = false;
	string skip_reason;
};

struct StreamCapture {
	StreamCapture() : old_stdout(std::cout.rdbuf(output.rdbuf())), old_stderr(std::cerr.rdbuf(output.rdbuf())) {
	}

	~StreamCapture() {
		std::cout.rdbuf(old_stdout);
		std::cerr.rdbuf(old_stderr);
	}

	std::ostringstream output;
	std::streambuf *old_stdout;
	std::streambuf *old_stderr;
};

static thread_local optional_ptr<CaptureState> current_capture;

class TestBundleReporter : public Catch::StreamingReporterBase<TestBundleReporter> {
public:
	using StreamingReporterBase::StreamingReporterBase;

	static std::string getDescription() {
		return "DuckDB test bundle reporter";
	}

	void assertionStarting(Catch::AssertionInfo const &) override {
	}

	bool assertionEnded(Catch::AssertionStats const &stats) override {
		if (!current_capture) {
			return true;
		}
		current_capture->assertions++;
		auto &result = stats.assertionResult;
		if (!result.isOk()) {
			auto source = result.getSourceInfo();
			CapturedFailure failure;
			failure.file = source.file;
			failure.line = NumericCast<uint64_t>(source.line);
			failure.expression =
			    result.hasExpandedExpression() ? result.getExpandedExpression() : result.getExpression();
			failure.message = result.getMessage();
			current_capture->failures.push_back(std::move(failure));
		}
		return true;
	}

	void testCaseEnded(Catch::TestCaseStats const &stats) override {
		if (current_capture && stats.totals.skippedTests > 0) {
			current_capture->skipped = true;
		}
		StreamingReporterBase::testCaseEnded(stats);
	}

	void skipTest(Catch::TestCaseInfo const &test_info) override {
		if (current_capture) {
			current_capture->skipped = true;
			current_capture->skip_reason = test_info.name;
		}
	}
};

CATCH_REGISTER_REPORTER("duckdb_test_bundle", TestBundleReporter)

static string QuoteCatchTest(const string &name) {
	string result = "\"";
	for (auto character : name) {
		if (character == '\\' || character == '"' || character == ',') {
			result += '\\';
		}
		result += character;
	}
	result += '"';
	return result;
}

static void InitializeTestEnvironment() {
	static bool initialized = false;
	if (initialized) {
		return;
	}
	auto &config = TestConfiguration::Get();
	config.Initialize();
	config.ChangeWorkingDirectory(TestRootDirectory());
	string error;
	if (!PrepareTempDir(error)) {
		throw InvalidInputException("Failed to prepare test directory: %s", error);
	}
	config.UpdateEnvironment();
	if (!config.ValidateDataDirs(error)) {
		throw InvalidInputException("%s", error);
	}
#ifdef DUCKDB_TEST_BUNDLE_DEBUG_FS
	SetStaticExtensionLoader([](DuckDB &db) { db.LoadStaticExtension<DebugFsExtension>(); });
#endif
	initialized = true;
}

static void EnumerateTests(void *, test_case_callback_t callback, void *callback_data) {
	for (auto &test : Catch::getRegistryHub().getTestCaseRegistry().getAllTests()) {
		auto &info = test.getTestCaseInfo();
		auto tags = info.tagsAsString();
		TestCase result {info.name.c_str(), tags.c_str(), info.lineInfo.file,
		                 NumericCast<uint64_t>(info.lineInfo.line)};
		callback(callback_data, &result);
	}
}

static void RunTests(void *, const char *const *test_names, uint64_t test_count, test_result_callback_t callback,
                     void *callback_data) {
	InitializeTestEnvironment();
	for (uint64_t test_idx = 0; test_idx < test_count; test_idx++) {
		string test_name(test_names[test_idx]);
		CaptureState capture;
		current_capture = capture;

		Catch::Session session;
		auto &config = session.configData();
		config.name = DUCKDB_TEST_BUNDLE_NAME;
		config.reporterName = "duckdb_test_bundle";
		config.useColour = Catch::UseColour::No;
		config.testsOrTags.push_back(QuoteCatchTest(test_name));

		auto start = std::chrono::steady_clock::now();
		int return_code;
		string run_output;
		{
			StreamCapture stream_capture;
			return_code = session.run();
			run_output = stream_capture.output.str();
		}
		auto end = std::chrono::steady_clock::now();
		current_capture = nullptr;

		vector<TestFailure> failures;
		failures.reserve(capture.failures.size());
		for (auto &failure : capture.failures) {
			failures.push_back(
			    {failure.file.c_str(), failure.line, failure.expression.c_str(), failure.message.c_str()});
		}
		auto status = return_code == 0 ? TestStatus::PASSED : TestStatus::FAILED;
		if (capture.skipped) {
			status = TestStatus::SKIPPED;
		}
		auto duration = std::chrono::duration<double, std::milli>(end - start).count();
		auto message = capture.skip_reason;
		if (return_code != 0 && message.empty()) {
			message = std::move(run_output);
		}
		TestResult result {test_name.c_str(), status,
		                   duration,          NumericCast<uint64_t>(capture.assertions),
		                   failures.data(),   NumericCast<uint64_t>(failures.size()),
		                   message.c_str()};
		callback(callback_data, &result);
	}
}

static TestBundle bundle {DUCKDB_TEST_BUNDLE_NAME, nullptr, EnumerateTests, RunTests};

} // namespace

#define DUCKDB_TEST_BUNDLE_ENTRY(name) DUCKDB_CPP_EXTENSION_ENTRY(name, loader)

extern "C" {

DUCKDB_TEST_BUNDLE_ENTRY(DUCKDB_TEST_BUNDLE_IDENTIFIER) {
	auto service = static_cast<duckdb_test::TestRegistry *>(loader.GetService(duckdb_test::SERVICE_NAME));
	if (!service) {
		throw duckdb::InvalidInputException("The test extension must be loaded before '%s'", DUCKDB_TEST_BUNDLE_NAME);
	}
	service->register_bundle(service->data, &bundle);
}
}
