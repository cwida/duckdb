//===----------------------------------------------------------------------===//
//                         DuckDB
//
// catch_test_reporter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "test_reporter.hpp"

namespace duckdb {

//! Routes the sqllogictest runner's verdicts into the Catch session that the unittest binary runs.
//! Only the unittest binary links this; the sqllogictest extension uses the default reporter.
class CatchTestReporter : public TestReporter {
public:
	void Fail(const string &message, const string &file, idx_t line) override;
	using TestReporter::Fail;
	void Skip(const string &reason) override;
	void Assertion() override;
	string CurrentTestName() override;
};

} // namespace duckdb
