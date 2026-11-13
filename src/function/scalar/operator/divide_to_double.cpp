#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar/operator_functions.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

namespace {

template <bool NULL_ON_ZERO>
void DivideToDoubleFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<hugeint_t, int64_t, double>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](hugeint_t numerator, int64_t denominator) -> optional<double> {
		    if (denominator == 0) {
			    if (!NULL_ON_ZERO) {
				    throw InvalidInputException("Division by zero in expression %s. Use TRY(...) to return NULL for "
				                                "this expression, or SET error_on_division_by_zero=false to return "
				                                "NULL for all divisions by zero.",
				                                state.expr.ToString());
			    }
			    return nullopt;
		    }
		    return Hugeint::DivideToDouble(numerator, denominator);
	    });
}

unique_ptr<FunctionData> BindDivideToDouble(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	if (Settings::Get<ErrorOnDivisionByZeroSetting>(input.GetClientContext())) {
		bound_function.SetFunctionCallback(DivideToDoubleFunction<false>);
	} else {
		bound_function.SetFunctionCallback(DivideToDoubleFunction<true>);
	}
	return nullptr;
}

} // namespace

ScalarFunction InternalDivideToDoubleFun::GetFunction() {
	ScalarFunction function(Name, {LogicalType::HUGEINT, LogicalType::BIGINT}, LogicalType::DOUBLE,
	                        DivideToDoubleFunction<false>, BindDivideToDouble);
	// the catalog reports the signature's parameter names, which are the ones in functions.json
	function.GetSignature().GetParameter(0).SetName("numerator");
	function.GetSignature().GetParameter(1).SetName("denominator");
	// division by zero
	function.SetFallible();
	return function;
}

} // namespace duckdb
