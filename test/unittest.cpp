#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#include <stdlib.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_set>

#if defined(__linux__)
#include <features.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "debug_fs_extension.hpp"
#include "sqlite/catch_test_reporter.hpp"
#include "sqlite/sqllogic_test_logger.hpp"
#include "sqlite/sqllogic_test_runner.hpp"
#include "test_helpers.hpp"
#include "test_config.hpp"

using namespace duckdb;

namespace {

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 18)
#define DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
#endif
#endif

static bool TryParseThreadStackSize(const string &value, size_t &result) {
	if (value.empty()) {
		return false;
	}
	size_t parsed_value = 0;
	for (auto character : value) {
		if (character < '0' || character > '9') {
			return false;
		}
		auto digit = static_cast<size_t>(character - '0');
		if (parsed_value > (std::numeric_limits<size_t>::max() - digit) / 10) {
			return false;
		}
		parsed_value = parsed_value * 10 + digit;
	}
	if (parsed_value == 0) {
		return false;
	}
	result = parsed_value;
	return true;
}

#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
static string PthreadError(const char *function, int error) {
	return string(function) + " failed: " + std::strerror(error);
}

enum class ThreadStackProbeResult : uint8_t { SUCCESS, TOO_SMALL, ERROR };

// glibc's fixed Linux minimum excludes the binary-specific static TLS overhead.
static constexpr size_t GLIBC_PTHREAD_STACK_MIN = 16384;

static void *MinimumThreadStackProbe(void *) {
	return nullptr;
}

// Probes run on a stack this function maps itself to not pollute the glibc stack cache with stacks created during these
// probes.
static ThreadStackProbeResult TryThreadStackSize(size_t stack_size, string &error) {
	pthread_attr_t attributes;
	auto result = pthread_attr_init(&attributes);
	if (result != 0) {
		error = PthreadError("pthread_attr_init", result);
		return ThreadStackProbeResult::ERROR;
	}

	auto stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		pthread_attr_destroy(&attributes);
		error = "Failed to map a " + to_string(stack_size) + " byte probe stack";
		return ThreadStackProbeResult::ERROR;
	}

	result = pthread_attr_setstack(&attributes, stack, stack_size);
	if (result != 0) {
		pthread_attr_destroy(&attributes);
		munmap(stack, stack_size);
		if (result == EINVAL) {
			return ThreadStackProbeResult::TOO_SMALL;
		}
		error = PthreadError("pthread_attr_setstack", result);
		return ThreadStackProbeResult::ERROR;
	}

	pthread_t probe;
	result = pthread_create(&probe, &attributes, MinimumThreadStackProbe, nullptr);
	auto destroy_result = pthread_attr_destroy(&attributes);
	if (result != 0) {
		munmap(stack, stack_size);
		if (result == EINVAL) {
			return ThreadStackProbeResult::TOO_SMALL;
		}
		error = PthreadError("pthread_create", result);
		return ThreadStackProbeResult::ERROR;
	}
	auto join_result = pthread_join(probe, nullptr);
	// only once the thread is gone is the stack free to unmap
	munmap(stack, stack_size);
	if (join_result != 0) {
		error = PthreadError("pthread_join", join_result);
		return ThreadStackProbeResult::ERROR;
	}
	if (destroy_result != 0) {
		error = PthreadError("pthread_attr_destroy", destroy_result);
		return ThreadStackProbeResult::ERROR;
	}
	return ThreadStackProbeResult::SUCCESS;
}

static bool GetMinimumThreadStackSize(size_t default_stack_size, size_t page_size, size_t &minimum_stack_size,
                                      string &error) {
	auto lower_page = (GLIBC_PTHREAD_STACK_MIN + page_size - 1) / page_size;
	auto upper_page = default_stack_size / page_size;
	if (upper_page < lower_page) {
		error = "Default pthread stack size is below the glibc minimum";
		return false;
	}
	auto default_probe_result = TryThreadStackSize(upper_page * page_size, error);
	if (default_probe_result != ThreadStackProbeResult::SUCCESS) {
		if (default_probe_result == ThreadStackProbeResult::TOO_SMALL) {
			error = "Unable to create a thread with the default pthread stack size";
		} else {
			error = "Unable to create a thread with the default pthread stack size: " + error;
		}
		return false;
	}

	while (lower_page < upper_page) {
		auto middle_page = lower_page + (upper_page - lower_page) / 2;
		auto probe_result = TryThreadStackSize(middle_page * page_size, error);
		if (probe_result == ThreadStackProbeResult::SUCCESS) {
			upper_page = middle_page;
		} else if (probe_result == ThreadStackProbeResult::TOO_SMALL) {
			lower_page = middle_page + 1;
		} else {
			return false;
		}
	}
	minimum_stack_size = lower_page * page_size;
	return true;
}

static bool SetThreadStackSize(size_t requested_stack_size, string &error) {
	pthread_attr_t default_attributes;
	auto result = pthread_getattr_default_np(&default_attributes);
	if (result != 0) {
		error = PthreadError("pthread_getattr_default_np", result);
		return false;
	}

	size_t default_stack_size;
	result = pthread_attr_getstacksize(&default_attributes, &default_stack_size);
	if (result != 0) {
		pthread_attr_destroy(&default_attributes);
		error = PthreadError("pthread_attr_getstacksize", result);
		return false;
	}
	auto page_size_result = sysconf(_SC_PAGESIZE);
	if (page_size_result <= 0) {
		pthread_attr_destroy(&default_attributes);
		error = "Failed to determine the system page size";
		return false;
	}
	auto page_size = static_cast<size_t>(page_size_result);
	size_t minimum_stack_size;
	if (!GetMinimumThreadStackSize(default_stack_size, page_size, minimum_stack_size, error)) {
		pthread_attr_destroy(&default_attributes);
		return false;
	}
	auto stack_overhead = minimum_stack_size - GLIBC_PTHREAD_STACK_MIN;
	if (requested_stack_size > std::numeric_limits<size_t>::max() - stack_overhead) {
		pthread_attr_destroy(&default_attributes);
		error = "--thread-stack-size is too large";
		return false;
	}
	auto configured_stack_size = requested_stack_size + stack_overhead;

	result = pthread_attr_setstacksize(&default_attributes, configured_stack_size);
	if (result == 0) {
		result = pthread_setattr_default_np(&default_attributes);
	}
	auto destroy_result = pthread_attr_destroy(&default_attributes);
	if (result != 0) {
		error = PthreadError("setting the default pthread stack size", result);
		return false;
	}
	if (destroy_result != 0) {
		error = PthreadError("pthread_attr_destroy", destroy_result);
		return false;
	}

	size_t observed_stack_size = 0;
	int probe_result = 0;
	try {
		std::thread probe([&]() {
			pthread_attr_t probe_attributes;
			probe_result = pthread_getattr_np(pthread_self(), &probe_attributes);
			if (probe_result != 0) {
				return;
			}
			probe_result = pthread_attr_getstacksize(&probe_attributes, &observed_stack_size);
			auto probe_destroy_result = pthread_attr_destroy(&probe_attributes);
			if (probe_result == 0) {
				probe_result = probe_destroy_result;
			}
		});
		probe.join();
	} catch (std::exception &ex) {
		error = string("Failed to create thread stack size probe: ") + ex.what();
		return false;
	}
	if (probe_result != 0) {
		error = PthreadError("reading the probe thread stack size", probe_result);
		return false;
	}

	auto effective_stack_size = observed_stack_size > stack_overhead ? observed_stack_size - stack_overhead : 0;
	auto difference = effective_stack_size > requested_stack_size ? effective_stack_size - requested_stack_size
	                                                              : requested_stack_size - effective_stack_size;
	if (difference > page_size) {
		error = "Thread stack size probe reported " + to_string(observed_stack_size) + " total bytes (" +
		        to_string(effective_stack_size) + " after glibc overhead) after requesting " +
		        to_string(requested_stack_size) + " bytes";
		return false;
	}
	return true;
}
#endif

static bool ConfigureThreadStackSize(int argc, char *argv[], string &error) {
	bool stack_size_specified = false;
#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
	size_t requested_stack_size = 0;
#endif
	for (int i = 1; i < argc; i++) {
		if (string(argv[i]) != "--thread-stack-size") {
			continue;
		}
		if (stack_size_specified) {
			error = "--thread-stack-size may only be specified once";
			return false;
		}
		if (++i >= argc) {
			error = "--thread-stack-size expected a size in bytes";
			return false;
		}
		size_t parsed_stack_size;
		if (!TryParseThreadStackSize(argv[i], parsed_stack_size)) {
			error = "--thread-stack-size expected a positive integer size in bytes";
			return false;
		}
#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
		requested_stack_size = parsed_stack_size;
#endif
		stack_size_specified = true;
	}
	if (!stack_size_specified) {
		return true;
	}

#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
	return SetThreadStackSize(requested_stack_size, error);
#else
	error = "--thread-stack-size is only supported on Linux with glibc 2.18 or newer";
	return false;
#endif
}

struct TempDirReclaimer {
	~TempDirReclaimer() {
		DestroyTempDir(success);
	}

	bool success = false;
};

} // namespace

static bool IsSQLLogicTestFile(const string &path) {
	return StringUtil::EndsWith(path, ".test") || StringUtil::EndsWith(path, ".test_slow") ||
	       StringUtil::EndsWith(path, ".test_coverage");
}

static bool TryReadExactSQLLogicTestFilter(const vector<string> &input_files, vector<string> &test_paths,
                                           string &error) {
	if (input_files.empty()) {
		return false;
	}
	auto fs = FileSystem::CreateLocal();
	unordered_set<string> seen_paths;
	for (auto &input_file : input_files) {
		std::ifstream file(input_file.c_str());
		if (!file.is_open()) {
			return false;
		}
		string line;
		while (std::getline(file, line)) {
			StringUtil::Trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (!IsSQLLogicTestFile(line)) {
				return false;
			}
			if (!fs->FileExists(line)) {
				error = "Unable to find sqllogictest file from -f/--input-file: " + line;
				return true;
			}
			if (seen_paths.insert(line).second) {
				test_paths.push_back(line);
			}
		}
	}
	return !test_paths.empty();
}

int main(int argc_in, char *argv[]) {
	string thread_stack_error;
	if (!ConfigureThreadStackSize(argc_in, argv, thread_stack_error)) {
		fprintf(stderr, "%s\n", thread_stack_error.c_str());
		return 1;
	}

	string test_directory = TestRootDirectory();

	// route the sqllogictest runner's verdicts into the Catch session
	static CatchTestReporter catch_reporter;
	TestReporter::Set(catch_reporter);
	// debug_fs is linked into this binary; hand it to every database the runner creates
	SetStaticExtensionLoader([](DuckDB &db) { db.LoadStaticExtension<DebugFsExtension>(); });

	auto &test_config = TestConfiguration::Get();
	try {
		// Applies every DUCKDB_TEST_<NAME> fallback, so a bad env value surfaces here.
		test_config.Initialize();
	} catch (std::exception &ex) {
		fprintf(stderr, "%s\n", ex.what());
		return 1;
	}
	bool keep_home = false;
	bool use_stdin = false;
	vector<string> input_files;
	unordered_set<idx_t> input_file_arg_indices;

	// The --temp-dir-* family, --run-id and --env-passthrough live in TestConfiguration's option table,
	// so ParseArgument handles them below and Initialize() already applied their env fallbacks. What
	// remains here is what that table cannot express.
	idx_t argc = NumericCast<idx_t>(argc_in);
	int new_argc = 0;
	auto new_argv = duckdb::unique_ptr<char *[]>(new char *[argc]);
	for (idx_t i = 0; i < argc; i++) {
		string argument(argv[i]);
		if (argument == "--test-dir") {
			test_directory = string(argv[++i]);
		} else if (argument == "--require") {
			AddRequire(string(argv[++i]));
		} else if (argument == "--emit-on-skip") {
			SetEmitOnSkip(true);
		} else if (argument == "--keep-home") {
			keep_home = true;
		} else if (argument == "--stdin") {
			use_stdin = true;
		} else if (argument == "--emit-test-events") {
			SetEmitTestEvents(true);
		} else if (argument == "--thread-stack-size") {
			i++;
		} else {
			try {
				if (!test_config.ParseArgument(argument, argc, argv, i)) {
					if ((argument == "-f" || argument == "--input-file") && i + 1 < argc) {
						input_files.push_back(TestMakeAbsolute(argv[i + 1], TestGetCurrentDirectory()));
						input_file_arg_indices.insert(new_argc);
						input_file_arg_indices.insert(new_argc + 1);
					}
					new_argv[new_argc] = argv[i];
					new_argc++;
				}
			} catch (std::exception &ex) {
				fprintf(stderr, "%s\n", ex.what());
				return 1;
			}
		}
	}

	// Before any temp-dir prep or test runs: a named-but-absent var kills the whole invocation, unlike
	// require-env's per-test skip.
	string env_error;
	if (!ValidateEnvPassthrough(env_error) || !test_config.ValidateTestEnv(env_error)) {
		fprintf(stderr, "%s\n", env_error.c_str());
		return 1;
	}

	// Keep input filenames anchored to the invocation directory, including Catch's filter fallback.
	idx_t input_file_index = 0;
	for (int i = 0; i < new_argc; i++) {
		if (input_file_arg_indices.find(i) != input_file_arg_indices.end()) {
			new_argv[++i] = &input_files[input_file_index++][0];
		}
	}
	test_config.ChangeWorkingDirectory(test_directory);

	vector<string> exact_sqllogic_tests;
	string exact_sqllogic_error;
	bool exact_sqllogic_filter =
	    TryReadExactSQLLogicTestFilter(input_files, exact_sqllogic_tests, exact_sqllogic_error);
	if (!exact_sqllogic_error.empty()) {
		fprintf(stderr, "%s\n", exact_sqllogic_error.c_str());
		return 1;
	}
	string exact_sqllogic_test_filter = "*";
	if (exact_sqllogic_filter) {
		int filtered_argc = 0;
		for (int i = 0; i < new_argc; i++) {
			if (input_file_arg_indices.find(i) != input_file_arg_indices.end()) {
				continue;
			}
			new_argv[filtered_argc++] = new_argv[i];
		}
		new_argv[filtered_argc++] = const_cast<char *>(exact_sqllogic_test_filter.c_str());
		new_argc = filtered_argc;
	}

	// Resolve + provision $BASE/[RUN_ID] (the TEST_ID level is materialized later, on the
	// per-test path, once a test name is known).
	string prep_error;
	if (!PrepareTempDir(prep_error)) {
		fprintf(stderr, "Failed to prepare temp directory: %s\n", prep_error.c_str());
		return 1;
	}
	TempDirReclaimer temp_dir_reclaimer;

	// Capture env now that all --temp-dir-* context (base/run-id/create) is final; must run
	// after PrepareTempDir so TEMP_DIR reflects the materialized run root.
	test_config.UpdateEnvironment();

	string data_dir_error;
	if (!test_config.ValidateDataDirs(data_dir_error)) {
		fprintf(stderr, "%s\n", data_dir_error.c_str());
		return 1;
	}

	// Set ONCE per invocation, never per-test, so ~/.duckdb (extensions, secrets) is isolated without
	// landing inside a {TEST_DIR} a test whitelists. Absolute because a relative home would shift under
	// any test chdir.
	string home_dir = TestMakeAbsolute(GetTempDirHome(), TestGetCurrentDirectory());

	if (!keep_home) {
#ifdef DUCKDB_WINDOWS
		if (_putenv_s("USERPROFILE", home_dir.c_str()) != 0) {
			fprintf(stderr, "Failed to set USERPROFILE environment variable\n");
			return 1;
		}
#else
		if (setenv("HOME", home_dir.c_str(), 1) != 0) {
			fprintf(stderr, "Failed to set HOME environment variable\n");
			return 1;
		}
#endif
	}

	if (use_stdin || exact_sqllogic_filter || test_config.GetSkipCompiledTests()) {
		Catch::getMutableRegistryHub().clearTests();
	}
	if (use_stdin) {
		RegisterSqllogictestStdin();
	} else if (exact_sqllogic_filter) {
		RegisterSqllogictests(exact_sqllogic_tests);
	} else {
		RegisterSqllogictests();
	}
	int result = Catch::Session().run(new_argc, new_argv.get());

	std::string failures_summary = FailureSummary::GetFailureSummary();
	if (!failures_summary.empty()) {
		auto description = test_config.GetDescription();
		if (!description.empty()) {
			std::cerr << "\n====================================================" << std::endl;
			std::cerr << "====================  TEST INFO  ===================" << std::endl;
			std::cerr << "====================================================\n" << std::endl;
			std::cerr << description << std::endl;
		}
		std::cerr << "\n====================================================" << std::endl;
		std::cerr << "================  FAILURES SUMMARY  ================" << std::endl;
		std::cerr << "====================================================\n" << std::endl;
		std::cerr << failures_summary;
	}
	std::string skip_reason_summary = SQLLogicTestRunner::GetSkipReasonSummary();
	if (!skip_reason_summary.empty()) {
		std::cerr << "\n"
		          << "Skipped tests for the following reasons:" << std::endl;
		std::cerr << skip_reason_summary;
	}

	// Execute the run-id-level destroy disposition ($BASE/[RUN_ID]); pass/fail-aware, recursive.
	temp_dir_reclaimer.success = result == 0;

	return result;
}
