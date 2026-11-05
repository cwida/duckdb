#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "core_functions/aggregate/regression_functions.hpp"
#include "core_functions/aggregate/regression/regr_count.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

AggregateFunction RegrCountFun::GetFunction() {
	auto regr_count = AggregateFunction::BinaryAggregate<uint64_t, double, double, uint32_t, RegrCountFunction>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::UINTEGER);
	regr_count.GetSignature().GetParameter(0).SetName("y");
	regr_count.GetSignature().GetParameter(1).SetName("x");
	regr_count.SetName("regr_count");
	regr_count.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return regr_count;
}

} // namespace duckdb
