#include "core_functions/scalar/debug_functions.hpp"

#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/common/thread.hpp"

namespace duckdb {

struct NullResultType {
	using STRUCT_STATE = PrimitiveTypeState;

	static void AssignResult(Vector &result, idx_t i, NullResultType) {
		FlatVector::SetNull(result, i, true);
	}
};

static void SleepFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	input.Flatten();
	auto &context = state.GetContext();
	GenericExecutor::ExecuteUnary<PrimitiveType<int64_t>, NullResultType>(input.data[0], result,
	                                                                      [&context](PrimitiveType<int64_t> input) {
		                                                                      // Sleep for the specified number of
		                                                                      // milliseconds (clamp negative values to
		                                                                      // 0)
		                                                                      int64_t sleep_ms = input.val;
		                                                                      if (sleep_ms < 0) {
			                                                                      sleep_ms = 0;
		                                                                      }
		                                                                      ThreadUtil::SleepMs(sleep_ms, context);
		                                                                      return NullResultType();
	                                                                      });
}

ScalarFunction SleepMsFun::GetFunction() {
	auto sleep_fun = ScalarFunction({}, LogicalType::SQLNULL, SleepFunction, nullptr);
	sleep_fun.GetSignature().AddParameter("milliseconds", LogicalType::BIGINT);
	sleep_fun.SetVolatile();
	sleep_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return sleep_fun;
}

} // namespace duckdb
