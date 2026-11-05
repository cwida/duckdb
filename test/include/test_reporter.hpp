//===----------------------------------------------------------------------===//
//                         DuckDB
//
// test_reporter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! Thrown by TestReporter::Fail to abort the running test. Deliberately NOT derived from
//! std::exception: the test runner catches std::exception in many places to turn a failing query
//! into a test result, and a verdict must never be swallowed there. Mirrors how Catch's own
//! TestFailureException escapes such handlers.
struct TestFailureException {
	TestFailureException(string message_p, string file_p, idx_t line_p)
	    : message(std::move(message_p)), file(std::move(file_p)), line(line_p) {
	}

	string message;
	//! Location of the failing command in the .test script ("" when the failure has no script line)
	string file;
	idx_t line;
};

//! Sink for the verdicts the sqllogictest runner produces. The runner reports through this
//! interface instead of calling a test framework directly, so the same runner can be driven by the
//! Catch-based unittest binary or in-process (the sqllogictest extension).
class TestReporter {
public:
	virtual ~TestReporter() = default;

	//! Abort the currently running test with a failure. Never returns. `file`/`line` locate the
	//! failing command in the .test script; they are empty when the failure has no script line.
	virtual void Fail(const string &message, const string &file, idx_t line);
	void Fail(const string &message) {
		Fail(message, string(), 0);
	}
	//! Record why the current test is skipped. Does NOT abort - callers stop on their own.
	virtual void Skip(const string &reason);
	//! Record one passed assertion.
	virtual void Assertion();
	//! Name of the test being executed, or an empty string if the driver tracks none.
	virtual string CurrentTestName();

	//! Fail unless the condition holds.
	void Require(bool condition, const char *expression);

	//! The reporter driving the current process.
	static TestReporter &Get();
	//! Install a reporter; passing nullptr restores the default one.
	static void Set(optional_ptr<TestReporter> reporter);

public:
	//! Assertions recorded so far. Written from the worker threads of a concurrentloop.
	atomic<idx_t> assertion_count {0};
	//! Reason passed to the last Skip call.
	string skip_reason;
};

//! Installs a reporter for as long as it is in scope, restoring the previous one afterwards (a test
//! run nested inside another one must not leave the outer driver without its reporter).
class TestReporterScope {
public:
	explicit TestReporterScope(TestReporter &reporter) : previous(TestReporter::Get()) {
		TestReporter::Set(reporter);
	}
	~TestReporterScope() {
		TestReporter::Set(previous);
	}

private:
	TestReporter &previous;
};

} // namespace duckdb

#define TEST_FAIL(message) duckdb::TestReporter::Get().Fail(message)
#define TEST_FAIL_LINE(file, line, message)                                                                            \
	duckdb::TestReporter::Get().Fail(message, file, duckdb::NumericCast<duckdb::idx_t>(line))
#define TEST_REQUIRE(condition) duckdb::TestReporter::Get().Require((condition) ? true : false, #condition)
#define TEST_ASSERTION()        duckdb::TestReporter::Get().Assertion()
