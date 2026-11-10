#include "core_functions/scalar/date_functions.hpp"

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

namespace duckdb {

namespace {

struct EpochSecOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE sec) {
		int64_t result;
		if (!TryCast::Operation(sec * Interval::MICROS_PER_SEC, result)) {
			throw ConversionException("Epoch seconds out of range for TIMESTAMP WITH TIME ZONE");
		}
		return timestamp_t(result);
	}
};

void EpochSecFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);

	UnaryExecutor::Execute<double, timestamp_t, EpochSecOperator>(input.data[0], result);
}

struct NormalizedIntervalOperator {
	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		return input.Normalize();
	}
};

void NormalizedIntervalFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);

	UnaryExecutor::Execute<interval_t, interval_t, NormalizedIntervalOperator>(input.data[0], result);
}

struct TimeTZSortKeyOperator {
	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		return input.sort_key();
	}
};

void TimeTZSortKeyFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);

	UnaryExecutor::Execute<dtime_tz_t, uint64_t, TimeTZSortKeyOperator>(input.data[0], result);
}

} // namespace

ScalarFunction ToTimestampFun::GetFunction() {
	// to_timestamp is an alias from Postgres that converts the time in seconds to a timestamp
	ScalarFunction func({}, LogicalType::TIMESTAMP_TZ, EpochSecFunction);
	func.GetSignature().AddParameter("sec", LogicalType::DOUBLE);
	// throws if the epoch seconds are out of range for a timestamp
	func.SetFallible();
	func.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return func;
}

ScalarFunction NormalizedIntervalFun::GetFunction() {
	ScalarFunction function({}, LogicalType::INTERVAL, NormalizedIntervalFunction);
	function.GetSignature().AddParameter("interval", LogicalType::INTERVAL);
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

ScalarFunction TimeTZSortKeyFun::GetFunction() {
	ScalarFunction func({}, LogicalType::UBIGINT, TimeTZSortKeyFunction);
	func.GetSignature().AddParameter("time_tz", LogicalType::TIME_TZ);
	return func;
}
} // namespace duckdb
