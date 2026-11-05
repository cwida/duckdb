#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/algebraic/covar.hpp"

namespace duckdb {

AggregateFunction CovarPopFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<CovarState, double, double, double, CovarPopOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

AggregateFunction CovarSampFun::GetFunction() {
	auto fun = AggregateFunction::BinaryAggregate<CovarState, double, double, double, CovarSampOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("y");
	fun.GetSignature().GetParameter(1).SetName("x");
	return fun;
}

} // namespace duckdb
