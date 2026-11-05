// REGR_SLOPE(y, x)
// Returns the slope of the linear regression line for non-null pairs in a group.
// It is computed for non-null pairs using the following formula:
// COVAR_POP(x,y) / VAR_POP(x)

//! Input : Any numeric type
//! Output : Double

#include "core_functions/aggregate/regression/regr_slope.hpp"
#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/regression_functions.hpp"

namespace duckdb {

AggregateFunction RegrSlopeFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<RegrSlopeState, double, double, double, RegrSlopeOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

} // namespace duckdb
