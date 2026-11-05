//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb_test/test_bundle.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace duckdb_test {

static constexpr const char *SERVICE_NAME = "duckdb_test_registry";

struct TestCase {
	const char *name;
	const char *tags;
	const char *source;
	uint64_t line;
};

struct TestFailure {
	const char *file;
	uint64_t line;
	const char *expression;
	const char *message;
};

enum class TestStatus : uint8_t { PASSED, FAILED, SKIPPED };

struct TestResult {
	const char *name;
	TestStatus status;
	double duration_ms;
	uint64_t assertion_count;
	const TestFailure *failures;
	uint64_t failure_count;
	const char *message;
};

using test_case_callback_t = void (*)(void *data, const TestCase *test_case);
using test_result_callback_t = void (*)(void *data, const TestResult *result);

struct TestBundle {
	const char *name;
	void *data;
	void (*enumerate)(void *data, test_case_callback_t callback, void *callback_data);
	void (*run)(void *data, const char *const *test_names, uint64_t test_count, const char *const *test_flags,
	            uint64_t test_flag_count, test_result_callback_t callback, void *callback_data);
};

struct TestRegistry {
	void *data;
	void (*register_bundle)(void *data, const TestBundle *bundle);
};

} // namespace duckdb_test
