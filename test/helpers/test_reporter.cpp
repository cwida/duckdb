#include "test_reporter.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

//! Reporter used when no driver installed one: failures become exceptions, everything else is
//! tallied on the reporter itself.
static TestReporter default_reporter;
//! Per thread, so that test files running concurrently each report into their own reporter. A
//! thread that spawns workers mid-test (concurrentloop) hands them its reporter explicitly.
static thread_local TestReporter *active_reporter = nullptr;

void TestReporter::Fail(const string &message, const string &file, idx_t line) {
	throw TestFailureException(message, file, line);
}

void TestReporter::Skip(const string &reason) {
	skip_reason = reason;
}

void TestReporter::Assertion() {
	assertion_count++;
}

string TestReporter::CurrentTestName() {
	return string();
}

void TestReporter::Require(bool condition, const char *expression) {
	if (!condition) {
		Fail(StringUtil::Format("REQUIRE(%s) failed", expression));
		return;
	}
	Assertion();
}

TestReporter &TestReporter::Get() {
	return active_reporter ? *active_reporter : default_reporter;
}

void TestReporter::Set(optional_ptr<TestReporter> reporter) {
	active_reporter = reporter ? reporter.get() : &default_reporter;
}

} // namespace duckdb
