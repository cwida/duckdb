#include "core_functions/scalar/date_functions.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

static void AgeFunctionStandard(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);
	//	Subtract argument from current_date (at midnight)
	//	Theoretically, this should be TZ-sensitive, but since we have to be able to handle
	//	plain TZ when ICU is not loaded, we implement this in UTC (like everything else)
	//	To get the PG behaviour, we overload these functions in ICU for TSTZ arguments.
	auto current_date = Timestamp::FromDatetime(
	    Timestamp::GetDate(MetaTransaction::Get(state.GetContext()).start_timestamp), dtime_t(0));

	UnaryExecutor::Execute<timestamp_t, interval_t>(input.data[0], result,
	                                                [&](timestamp_t input) -> optional<interval_t> {
		                                                if (input.IsFinite()) {
			                                                return Interval::GetAge(current_date, input);
		                                                } else {
			                                                return nullopt;
		                                                }
	                                                });
}

static void AgeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 2);

	BinaryExecutor::Execute<timestamp_t, timestamp_t, interval_t>(
	    input.data[0], input.data[1], result, [&](timestamp_t input1, timestamp_t input2) -> optional<interval_t> {
		    if (input1.IsFinite() && input2.IsFinite()) {
			    return Interval::GetAge(input1, input2);
		    } else {
			    return nullopt;
		    }
	    });
}

ScalarFunctionSet AgeFun::GetFunctions() {
	ScalarFunctionSet age("age");
	ScalarFunction standard_fun({}, LogicalType::INTERVAL, AgeFunctionStandard);
	standard_fun.GetSignature().AddParameter("timestamp", LogicalType::TIMESTAMP);
	age.AddFunction(standard_fun);
	ScalarFunction binary_fun({}, LogicalType::INTERVAL, AgeFunction);
	binary_fun.GetSignature()
	    .AddParameter("timestamp1", LogicalType::TIMESTAMP)
	    .AddParameter("timestamp2", LogicalType::TIMESTAMP);
	age.AddFunction(binary_fun);
	return age;
}

} // namespace duckdb
