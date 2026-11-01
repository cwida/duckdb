#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/execution/task_error_manager.hpp"

#include <new>

using namespace duckdb;

//! Like creating or recording an error when memory has run out
static ErrorData FailToAllocate() {
	throw std::bad_alloc();
}

TEST_CASE("A task error that cannot be recorded for lack of memory is reported as out of memory", "[api]") {
	TaskErrorManager errors;
	errors.PushTaskError(FailToAllocate);
	REQUIRE(errors.HasError());
	REQUIRE(errors.GetError().Type() == ExceptionType::OUT_OF_MEMORY);
	REQUIRE_THROWS_WITH(errors.ThrowException(),
	                    Catch::Contains("Failed to allocate memory to report the error of a task"));

	// the lost error was the first one, so it stays the reported one
	errors.PushTaskError([]() { return ErrorData(ExceptionType::INVALID_INPUT, "a later error"); });
	REQUIRE(errors.GetError().Type() == ExceptionType::OUT_OF_MEMORY);

	errors.Reset();
	REQUIRE(!errors.HasError());
	errors.PushTaskError([]() { return ErrorData(ExceptionType::INVALID_INPUT, "the first error"); });
	errors.PushTaskError(FailToAllocate);
	REQUIRE(errors.GetError().Type() == ExceptionType::INVALID_INPUT);
	REQUIRE(errors.GetError().RawMessage() == "the first error");
}

TEST_CASE("A task error that fails to be created for another reason is not reported as out of memory", "[api]") {
	TaskErrorManager errors;
	auto fail_to_parse = []() -> ErrorData {
		throw SerializationException("Failed to parse JSON string");
	};
	REQUIRE_THROWS_AS(errors.PushTaskError(fail_to_parse), SerializationException);
	REQUIRE(!errors.HasError());
}

TEST_CASE("Parsing the JSON message of an error throws std::bad_alloc when memory runs out", "[api]") {
	// yyjson reports an input this long as an allocation failure before reading it
	string json = "{}";
	JSONParseError error;
	REQUIRE_THROWS_AS(JSONDocument::TryParse(json.c_str(), NumericLimits<idx_t>::Maximum(), error), std::bad_alloc);
}
