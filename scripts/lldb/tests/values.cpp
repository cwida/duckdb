#include "duckdb/common/optional_ptr.hpp"

#include <cstdint>

struct DebugValue {
	int64_t number = 42;
};

int main() {
	DebugValue value;
	duckdb::optional_ptr<DebugValue> optional(value);
	duckdb::optional_ptr<DebugValue> null_optional;
	duckdb::unique_ptr<DebugValue> owned(new DebugValue);
	duckdb::shared_ptr<DebugValue> shared(new DebugValue);
	int64_t data[] = {11, 22, 33, 44};
	auto data_ptr = data;
	int64_t count = 4;
	int64_t evaluations = 0;
	auto optional_raw = optional.get(); // LLDB_STEP_START
	// Stop here while all test values are in scope.
	return static_cast<int>(data_ptr[count - 1] + evaluations + optional_raw->number); // LLDB_TEST_STOP
}
