#include "catch_test_reporter.hpp"

#include "catch.hpp"
#include "sqllogic_test_logger.hpp"

namespace duckdb {

void CatchTestReporter::Fail(const string &message, const string &file, idx_t line) {
	const auto &test_file = *failure_files.insert(file.empty() ? CurrentTestName() : file).first;
	// Statement failures already log their diagnostics before reporting an empty message.
	if (!message.empty()) {
		SQLLogicTestLogger::PrintErrorHeader(test_file, line, message);
	}
	FAIL_LINE(test_file, NumericCast<int>(line), message);
}

void CatchTestReporter::Skip(const string &reason) {
	TestReporter::Skip(reason);
	Catch::getResultCapture().skipTestDuringRun(reason);
}

void CatchTestReporter::Assertion() {
	TestReporter::Assertion();
	// keep Catch's assertion tally moving
	REQUIRE(true);
}

string CatchTestReporter::CurrentTestName() {
	return Catch::getResultCapture().getCurrentTestName();
}

} // namespace duckdb
