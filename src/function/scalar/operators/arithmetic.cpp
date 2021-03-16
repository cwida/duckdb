#include "duckdb/function/scalar/operators.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/operator/numeric_binary_operators.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

template <class OP>
static scalar_function_t GetScalarIntegerFunction(PhysicalType type) {
	scalar_function_t function;
	switch (type) {
	case PhysicalType::INT8:
		function = &ScalarFunction::BinaryFunction<int8_t, int8_t, int8_t, OP>;
		break;
	case PhysicalType::INT16:
		function = &ScalarFunction::BinaryFunction<int16_t, int16_t, int16_t, OP>;
		break;
	case PhysicalType::INT32:
		function = &ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, OP>;
		break;
	case PhysicalType::INT64:
		function = &ScalarFunction::BinaryFunction<int64_t, int64_t, int64_t, OP>;
		break;
	case PhysicalType::UINT8:
		function = &ScalarFunction::BinaryFunction<uint8_t, uint8_t, uint8_t, OP>;
		break;
	case PhysicalType::UINT16:
		function = &ScalarFunction::BinaryFunction<uint16_t, uint16_t, uint16_t, OP>;
		break;
	case PhysicalType::UINT32:
		function = &ScalarFunction::BinaryFunction<uint32_t, uint32_t, uint32_t, OP>;
		break;
	case PhysicalType::UINT64:
		function = &ScalarFunction::BinaryFunction<uint64_t, uint64_t, uint64_t, OP>;
		break;
	default:
		throw NotImplementedException("Unimplemented type for GetScalarBinaryFunction");
	}
	return function;
}

template <class OP>
static scalar_function_t GetScalarBinaryFunction(PhysicalType type) {
	scalar_function_t function;
	switch (type) {
	case PhysicalType::INT128:
		function = &ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, OP>;
		break;
	case PhysicalType::FLOAT:
		function = &ScalarFunction::BinaryFunction<float, float, float, OP>;
		break;
	case PhysicalType::DOUBLE:
		function = &ScalarFunction::BinaryFunction<double, double, double, OP>;
		break;
	default:
		function = GetScalarIntegerFunction<OP>(type);
		break;
	}
	return function;
}

//===--------------------------------------------------------------------===//
// + [add]
//===--------------------------------------------------------------------===//
struct AddPropagateStatistics {
	template <class T, class OP>
	static bool Operation(LogicalType type, NumericStatistics &lstats, NumericStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		T min, max;
		// new min is min+min
		if (!OP::Operation(lstats.min.GetValueUnsafe<T>(), rstats.min.GetValueUnsafe<T>(), min)) {
			return true;
		}
		// new max is max+max
		if (!OP::Operation(lstats.max.GetValueUnsafe<T>(), rstats.max.GetValueUnsafe<T>(), max)) {
			return true;
		}
		new_min = Value::Numeric(type, min);
		new_max = Value::Numeric(type, max);
		return false;
	}
};

struct SubtractPropagateStatistics {
	template <class T, class OP>
	static bool Operation(LogicalType type, NumericStatistics &lstats, NumericStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		T min, max;
		if (!OP::Operation(lstats.min.GetValueUnsafe<T>(), rstats.max.GetValueUnsafe<T>(), min)) {
			return true;
		}
		if (!OP::Operation(lstats.max.GetValueUnsafe<T>(), rstats.min.GetValueUnsafe<T>(), max)) {
			return true;
		}
		new_min = Value::Numeric(type, min);
		new_max = Value::Numeric(type, max);
		return false;
	}
};

template <class OP, class PROPAGATE, class BASEOP>
static unique_ptr<BaseStatistics> PropagateNumericStats(ClientContext &context, BoundFunctionExpression &expr,
                                                        FunctionData *bind_data,
                                                        vector<unique_ptr<BaseStatistics>> &child_stats) {
	D_ASSERT(child_stats.size() == 2);
	// can only propagate stats if the children have stats
	if (!child_stats[0] || !child_stats[1]) {
		return nullptr;
	}
	auto &lstats = (NumericStatistics &)*child_stats[0];
	auto &rstats = (NumericStatistics &)*child_stats[1];
	Value new_min, new_max;
	bool potential_overflow = true;
	if (!lstats.min.is_null && !lstats.max.is_null && !rstats.min.is_null && !rstats.max.is_null) {
		switch (expr.return_type.InternalType()) {
		case PhysicalType::INT8:
			potential_overflow =
			    PROPAGATE::template Operation<int8_t, OP>(expr.return_type, lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT16:
			potential_overflow =
			    PROPAGATE::template Operation<int16_t, OP>(expr.return_type, lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT32:
			potential_overflow =
			    PROPAGATE::template Operation<int32_t, OP>(expr.return_type, lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT64:
			potential_overflow =
			    PROPAGATE::template Operation<int64_t, OP>(expr.return_type, lstats, rstats, new_min, new_max);
			break;
		default:
			return nullptr;
		}
	}
	if (potential_overflow) {
		new_min = Value(expr.return_type);
		new_max = Value(expr.return_type);
	} else {
		// no potential overflow: replace with non-overflowing operator
		expr.function.function = GetScalarIntegerFunction<BASEOP>(expr.return_type.InternalType());
	}
	auto stats = make_unique<NumericStatistics>(expr.return_type, move(new_min), move(new_max));
	stats->has_null = lstats.has_null || rstats.has_null;
	return move(stats);
}

template <class OP, class OPOVERFLOWCHECK, bool IS_SUBTRACT = false>
unique_ptr<FunctionData> SET_ARCH(BindDecimalAddSubtract)(ClientContext &context, ScalarFunction &bound_function,
                                                          vector<unique_ptr<Expression>> &arguments) {
	// get the max width and scale of the input arguments
	uint8_t max_width = 0, max_scale = 0, max_width_over_scale = 0;
	for (idx_t i = 0; i < arguments.size(); i++) {
		uint8_t width, scale;
		auto can_convert = arguments[i]->return_type.GetDecimalProperties(width, scale);
		if (!can_convert) {
			throw InternalException("Could not convert type %s to a decimal.", arguments[i]->return_type.ToString());
		}
		max_width = MaxValue<uint8_t>(width, max_width);
		max_scale = MaxValue<uint8_t>(scale, max_scale);
		max_width_over_scale = MaxValue<uint8_t>(width - scale, max_width_over_scale);
	}
	// for addition/subtraction, we add 1 to the width to ensure we don't overflow
	bool check_overflow = false;
	auto required_width = MaxValue<uint8_t>(max_scale + max_width_over_scale, max_width) + 1;
	if (required_width > Decimal::MAX_WIDTH_INT64 && max_width <= Decimal::MAX_WIDTH_INT64) {
		// we don't automatically promote past the hugeint boundary to avoid the large hugeint performance penalty
		check_overflow = true;
		required_width = Decimal::MAX_WIDTH_INT64;
	}
	if (required_width > Decimal::MAX_WIDTH_DECIMAL) {
		// target width does not fit in decimal at all: truncate the scale and perform overflow detection
		check_overflow = true;
		required_width = Decimal::MAX_WIDTH_DECIMAL;
	}
	// arithmetic between two decimal arguments: check the types of the input arguments
	LogicalType result_type = LogicalType(LogicalTypeId::DECIMAL, required_width, max_scale);
	// we cast all input types to the specified type
	for (idx_t i = 0; i < arguments.size(); i++) {
		// first check if the cast is necessary
		// if the argument has a matching scale and internal type as the output type, no casting is necessary
		auto &argument_type = arguments[i]->return_type;
		if (argument_type.scale() == result_type.scale() &&
		    argument_type.InternalType() == result_type.InternalType()) {
			bound_function.arguments[i] = argument_type;
		} else {
			bound_function.arguments[i] = result_type;
		}
	}
	bound_function.return_type = result_type;
	// now select the physical function to execute
	if (check_overflow) {
		bound_function.function = GetScalarBinaryFunction<OPOVERFLOWCHECK>(result_type.InternalType());
	} else {
		bound_function.function = GetScalarBinaryFunction<OP>(result_type.InternalType());
	}
	if (result_type.InternalType() != PhysicalType::INT128) {
		if (IS_SUBTRACT) {
			bound_function.statistics = PropagateNumericStats<SET_ARCH(TryDecimalSubtract), SubtractPropagateStatistics,
			                                                  SET_ARCH(SubtractOperator)>;
		} else {
			bound_function.statistics =
			    PropagateNumericStats<SET_ARCH(TryDecimalAdd), AddPropagateStatistics, SET_ARCH(AddOperator)>;
		}
	}
	return nullptr;
}

unique_ptr<FunctionData> SET_ARCH(NopDecimalBind)(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	bound_function.return_type = arguments[0]->return_type;
	bound_function.arguments[0] = arguments[0]->return_type;
	return nullptr;
}

void SET_ARCH(AddFun)::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet functions("+");
	// binary add function adds two numbers together
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(ScalarFunction({type, type}, type, nullptr, false,
			                                     SET_ARCH(BindDecimalAddSubtract) < SET_ARCH(AddOperator),
			                                     SET_ARCH(DecimalAddOverflowCheck) >));
		} else if (TypeIsIntegral(type.InternalType()) && type.id() != LogicalTypeId::HUGEINT) {
			functions.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarIntegerFunction<AddOperatorOverflowCheck>(type.InternalType()), false,
			    nullptr, nullptr,
			    PropagateNumericStats<SET_ARCH(TryAddOperator), AddPropagateStatistics, SET_ARCH(AddOperator)>));
		} else {
			functions.AddFunction(ScalarFunction({type, type}, type,
			                                     GetScalarBinaryFunction<SET_ARCH(AddOperator)>(type.InternalType())));
		}
	}
	// we can add integers to dates
	functions.AddFunction(ScalarFunction({LogicalType::DATE, LogicalType::INTEGER}, LogicalType::DATE,
	                                     GetScalarBinaryFunction<SET_ARCH(AddOperator)>(PhysicalType::INT32)));
	functions.AddFunction(ScalarFunction({LogicalType::INTEGER, LogicalType::DATE}, LogicalType::DATE,
	                                     GetScalarBinaryFunction<SET_ARCH(AddOperator)>(PhysicalType::INT32)));
	// we can add intervals together
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<interval_t, interval_t, interval_t, SET_ARCH(AddOperator)>));
	// we can add intervals to dates/times/timestamps
	functions.AddFunction(
	    ScalarFunction({LogicalType::DATE, LogicalType::INTERVAL}, LogicalType::DATE,
	                   ScalarFunction::BinaryFunction<date_t, interval_t, date_t, SET_ARCH(AddOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::DATE}, LogicalType::DATE,
	                   ScalarFunction::BinaryFunction<interval_t, date_t, date_t, SET_ARCH(AddOperator)>));

	functions.AddFunction(
	    ScalarFunction({LogicalType::TIME, LogicalType::INTERVAL}, LogicalType::TIME,
	                   ScalarFunction::BinaryFunction<dtime_t, interval_t, dtime_t, SET_ARCH(AddTimeOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::TIME}, LogicalType::TIME,
	                   ScalarFunction::BinaryFunction<interval_t, dtime_t, dtime_t, SET_ARCH(AddTimeOperator)>));

	functions.AddFunction(
	    ScalarFunction({LogicalType::TIMESTAMP, LogicalType::INTERVAL}, LogicalType::TIMESTAMP,
	                   ScalarFunction::BinaryFunction<timestamp_t, interval_t, timestamp_t, SET_ARCH(AddOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP,
	                   ScalarFunction::BinaryFunction<interval_t, timestamp_t, timestamp_t, SET_ARCH(AddOperator)>));
	// unary add function is a nop, but only exists for numeric types
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(
			    ScalarFunction({type}, type, ScalarFunction::NopFunction, false, SET_ARCH(NopDecimalBind)));
		} else {
			functions.AddFunction(ScalarFunction({type}, type, ScalarFunction::NopFunction));
		}
	}
	set.AddFunction(functions);
}

ScalarFunctionSet SET_ARCH_FRONT(Functions)::GetAddFunctions() {
	ScalarFunctionSet functions("+");
	// binary add function adds two numbers together
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(ScalarFunction({type, type}, type, nullptr, false,
			                                     SET_ARCH(BindDecimalAddSubtract) < SET_ARCH(AddOperator),
			                                     SET_ARCH(DecimalAddOverflowCheck) >));
		} else if (TypeIsIntegral(type.InternalType()) && type.id() != LogicalTypeId::HUGEINT) {
			functions.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarIntegerFunction<AddOperatorOverflowCheck>(type.InternalType()), false,
			    nullptr, nullptr,
			    PropagateNumericStats<SET_ARCH(TryAddOperator), AddPropagateStatistics, SET_ARCH(AddOperator)>));
		} else {
			functions.AddFunction(ScalarFunction({type, type}, type,
			                                     GetScalarBinaryFunction<SET_ARCH(AddOperator)>(type.InternalType())));
		}
	}
	// we can add integers to dates
	functions.AddFunction(ScalarFunction({LogicalType::DATE, LogicalType::INTEGER}, LogicalType::DATE,
	                                     GetScalarBinaryFunction<SET_ARCH(AddOperator)>(PhysicalType::INT32)));
	functions.AddFunction(ScalarFunction({LogicalType::INTEGER, LogicalType::DATE}, LogicalType::DATE,
	                                     GetScalarBinaryFunction<SET_ARCH(AddOperator)>(PhysicalType::INT32)));
	// we can add intervals together
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<interval_t, interval_t, interval_t, SET_ARCH(AddOperator)>));
	// we can add intervals to dates/times/timestamps
	functions.AddFunction(
	    ScalarFunction({LogicalType::DATE, LogicalType::INTERVAL}, LogicalType::DATE,
	                   ScalarFunction::BinaryFunction<date_t, interval_t, date_t, SET_ARCH(AddOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::DATE}, LogicalType::DATE,
	                   ScalarFunction::BinaryFunction<interval_t, date_t, date_t, SET_ARCH(AddOperator)>));

	functions.AddFunction(
	    ScalarFunction({LogicalType::TIME, LogicalType::INTERVAL}, LogicalType::TIME,
	                   ScalarFunction::BinaryFunction<dtime_t, interval_t, dtime_t, SET_ARCH(AddTimeOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::TIME}, LogicalType::TIME,
	                   ScalarFunction::BinaryFunction<interval_t, dtime_t, dtime_t, SET_ARCH(AddTimeOperator)>));

	functions.AddFunction(
	    ScalarFunction({LogicalType::TIMESTAMP, LogicalType::INTERVAL}, LogicalType::TIMESTAMP,
	                   ScalarFunction::BinaryFunction<timestamp_t, interval_t, timestamp_t, SET_ARCH(AddOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP,
	                   ScalarFunction::BinaryFunction<interval_t, timestamp_t, timestamp_t, SET_ARCH(AddOperator)>));
	// unary add function is a nop, but only exists for numeric types
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(
			    ScalarFunction({type}, type, ScalarFunction::NopFunction, false, SET_ARCH(NopDecimalBind)));
		} else {
			functions.AddFunction(ScalarFunction({type}, type, ScalarFunction::NopFunction));
		}
	}
	return functions;
}

//===--------------------------------------------------------------------===//
// - [subtract]
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> SET_ARCH(DecimalNegateBind)(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	auto decimal_type = arguments[0]->return_type;
	if (decimal_type.width() <= Decimal::MAX_WIDTH_INT16) {
		bound_function.function =
		    ScalarFunction::GetScalarUnaryFunction<SET_ARCH(NegateOperator)>(LogicalTypeId::SMALLINT);
	} else if (decimal_type.width() <= Decimal::MAX_WIDTH_INT32) {
		bound_function.function =
		    ScalarFunction::GetScalarUnaryFunction<SET_ARCH(NegateOperator)>(LogicalTypeId::INTEGER);
	} else if (decimal_type.width() <= Decimal::MAX_WIDTH_INT64) {
		bound_function.function =
		    ScalarFunction::GetScalarUnaryFunction<SET_ARCH(NegateOperator)>(LogicalTypeId::BIGINT);
	} else {
		D_ASSERT(decimal_type.width() <= Decimal::MAX_WIDTH_INT128);
		bound_function.function =
		    ScalarFunction::GetScalarUnaryFunction<SET_ARCH(NegateOperator)>(LogicalTypeId::HUGEINT);
	}
	bound_function.arguments[0] = decimal_type;
	bound_function.return_type = decimal_type;
	return nullptr;
}

struct NegatePropagateStatistics {
	template <class T>
	static void Operation(LogicalType type, NumericStatistics &istats, Value &new_min, Value &new_max) {
		// new min is -max
		new_min = Value::Numeric(type, SET_ARCH(NegateOperator)::Operation<T, T>(istats.max.GetValueUnsafe<T>()));
		// new max is -min
		new_max = Value::Numeric(type, SET_ARCH(NegateOperator)::Operation<T, T>(istats.min.GetValueUnsafe<T>()));
	}
};

static unique_ptr<BaseStatistics> NegateBindStatistics(ClientContext &context, BoundFunctionExpression &expr,
                                                       FunctionData *bind_data,
                                                       vector<unique_ptr<BaseStatistics>> &child_stats) {
	D_ASSERT(child_stats.size() == 1);
	// can only propagate stats if the children have stats
	if (!child_stats[0]) {
		return nullptr;
	}
	auto &istats = (NumericStatistics &)*child_stats[0];
	Value new_min, new_max;
	if (!istats.min.is_null && !istats.max.is_null) {
		switch (expr.return_type.InternalType()) {
		case PhysicalType::INT8:
			NegatePropagateStatistics::Operation<int8_t>(expr.return_type, istats, new_min, new_max);
			break;
		case PhysicalType::INT16:
			NegatePropagateStatistics::Operation<int16_t>(expr.return_type, istats, new_min, new_max);
			break;
		case PhysicalType::INT32:
			NegatePropagateStatistics::Operation<int32_t>(expr.return_type, istats, new_min, new_max);
			break;
		case PhysicalType::INT64:
			NegatePropagateStatistics::Operation<int64_t>(expr.return_type, istats, new_min, new_max);
			break;
		default:
			return nullptr;
		}
	}
	auto stats = make_unique<NumericStatistics>(expr.return_type, move(new_min), move(new_max));
	stats->has_null = istats.has_null;
	return move(stats);
}

void SET_ARCH(SubtractFun)::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet functions("-");
	// binary subtract function "a - b", subtracts b from a
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(ScalarFunction({type, type}, type, nullptr, false,
			                                     SET_ARCH(BindDecimalAddSubtract) < SET_ARCH(SubtractOperator),
			                                     SET_ARCH(DecimalSubtractOverflowCheck), true >));
		} else if (TypeIsIntegral(type.InternalType()) && type.id() != LogicalTypeId::HUGEINT) {
			functions.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarIntegerFunction<SubtractOperatorOverflowCheck>(type.InternalType()), false,
			    nullptr, nullptr,
			    PropagateNumericStats<SET_ARCH(TrySubtractOperator), SubtractPropagateStatistics,
			                          SET_ARCH(SubtractOperator)>));
		} else {
			functions.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarBinaryFunction<SET_ARCH(SubtractOperator)>(type.InternalType())));
		}
	}
	// we can subtract dates from each other
	functions.AddFunction(ScalarFunction({LogicalType::DATE, LogicalType::DATE}, LogicalType::INTEGER,
	                                     GetScalarBinaryFunction<SET_ARCH(SubtractOperator)>(PhysicalType::INT32)));
	functions.AddFunction(ScalarFunction({LogicalType::DATE, LogicalType::INTEGER}, LogicalType::DATE,
	                                     GetScalarBinaryFunction<SET_ARCH(SubtractOperator)>(PhysicalType::INT32)));
	// we can subtract timestamps from each other
	functions.AddFunction(ScalarFunction(
	    {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP}, LogicalType::INTERVAL,
	    ScalarFunction::BinaryFunction<timestamp_t, timestamp_t, interval_t, SET_ARCH(SubtractOperator)>));
	// we can subtract intervals from each other
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<interval_t, interval_t, interval_t, SET_ARCH(SubtractOperator)>));
	// we can subtract intervals from dates/times/timestamps, but not the other way around
	functions.AddFunction(
	    ScalarFunction({LogicalType::DATE, LogicalType::INTERVAL}, LogicalType::DATE,
	                   ScalarFunction::BinaryFunction<date_t, interval_t, date_t, SET_ARCH(SubtractOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::TIME, LogicalType::INTERVAL}, LogicalType::TIME,
	                   ScalarFunction::BinaryFunction<dtime_t, interval_t, dtime_t, SET_ARCH(SubtractTimeOperator)>));
	functions.AddFunction(ScalarFunction(
	    {LogicalType::TIMESTAMP, LogicalType::INTERVAL}, LogicalType::TIMESTAMP,
	    ScalarFunction::BinaryFunction<timestamp_t, interval_t, timestamp_t, SET_ARCH(SubtractOperator)>));

	// unary subtract function, negates the input (i.e. multiplies by -1)
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(ScalarFunction({type}, type, nullptr, false, SET_ARCH(DecimalNegateBind), nullptr,
			                                     NegateBindStatistics));
		} else {
			functions.AddFunction(ScalarFunction({type}, type,
			                                     ScalarFunction::GetScalarUnaryFunction<SET_ARCH(NegateOperator)>(type),
			                                     false, nullptr, nullptr, NegateBindStatistics));
		}
	}
	set.AddFunction(functions);
}

//===--------------------------------------------------------------------===//
// * [multiply]
//===--------------------------------------------------------------------===//
struct MultiplyPropagateStatistics {
	template <class T, class OP>
	static bool Operation(LogicalType type, NumericStatistics &lstats, NumericStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		// statistics propagation on the multiplication is slightly less straightforward because of negative numbers
		// the new min/max depend on the signs of the input types
		// if both are positive the result is [lmin * rmin][lmax * rmax]
		// if lmin/lmax are negative the result is [lmin * rmax][lmax * rmin]
		// etc
		// rather than doing all this switcheroo we just multiply all combinations of lmin/lmax with rmin/rmax
		// and check what the minimum/maximum value is
		T lvals[] {lstats.min.GetValueUnsafe<T>(), lstats.max.GetValueUnsafe<T>()};
		T rvals[] {rstats.min.GetValueUnsafe<T>(), rstats.max.GetValueUnsafe<T>()};
		T min = NumericLimits<T>::Maximum();
		T max = NumericLimits<T>::Minimum();
		// multiplications
		for (idx_t l = 0; l < 2; l++) {
			for (idx_t r = 0; r < 2; r++) {
				T result;
				if (!OP::Operation(lvals[l], rvals[r], result)) {
					// potential overflow
					return true;
				}
				if (result < min) {
					min = result;
				}
				if (result > max) {
					max = result;
				}
			}
		}
		new_min = Value::Numeric(type, min);
		new_max = Value::Numeric(type, max);
		return false;
	}
};

unique_ptr<FunctionData> SET_ARCH(BindDecimalMultiply)(ClientContext &context, ScalarFunction &bound_function,
                                                       vector<unique_ptr<Expression>> &arguments) {
	uint8_t result_width = 0, result_scale = 0;
	uint8_t max_width = 0;
	for (idx_t i = 0; i < arguments.size(); i++) {
		uint8_t width, scale;
		auto can_convert = arguments[i]->return_type.GetDecimalProperties(width, scale);
		if (!can_convert) {
			throw InternalException("Could not convert type %s to a decimal?", arguments[i]->return_type.ToString());
		}
		if (width > max_width) {
			max_width = width;
		}
		result_width += width;
		result_scale += scale;
	}
	if (result_scale > Decimal::MAX_WIDTH_DECIMAL) {
		throw OutOfRangeException(
		    "Needed scale %d to accurately represent the multiplication result, but this is out of range of the "
		    "DECIMAL type. Max scale is %d; could not perform an accurate multiplication. Either add a cast to DOUBLE, "
		    "or add an explicit cast to a decimal with a lower scale.",
		    result_scale, Decimal::MAX_WIDTH_DECIMAL);
	}
	bool check_overflow = false;
	if (result_width > Decimal::MAX_WIDTH_INT64 && max_width <= Decimal::MAX_WIDTH_INT64) {
		check_overflow = true;
		result_width = Decimal::MAX_WIDTH_INT64;
	}
	if (result_width > Decimal::MAX_WIDTH_DECIMAL) {
		check_overflow = true;
		result_width = Decimal::MAX_WIDTH_DECIMAL;
	}
	LogicalType result_type = LogicalType(LogicalTypeId::DECIMAL, result_width, result_scale);
	// since our scale is the summation of our input scales, we do not need to cast to the result scale
	// however, we might need to cast to the correct internal type
	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &argument_type = arguments[i]->return_type;
		if (argument_type.InternalType() == result_type.InternalType()) {
			bound_function.arguments[i] = argument_type;
		} else {
			bound_function.arguments[i] = LogicalType(LogicalTypeId::DECIMAL, result_width, argument_type.scale());
		}
	}
	bound_function.return_type = result_type;
	// now select the physical function to execute
	if (check_overflow) {
		bound_function.function =
		    GetScalarBinaryFunction<SET_ARCH(DecimalMultiplyOverflowCheck)>(result_type.InternalType());
	} else {
		bound_function.function = GetScalarBinaryFunction<SET_ARCH(MultiplyOperator)>(result_type.InternalType());
	}
	if (result_type.InternalType() != PhysicalType::INT128) {
		bound_function.statistics = PropagateNumericStats<SET_ARCH(TryDecimalMultiply), MultiplyPropagateStatistics,
		                                                  SET_ARCH(MultiplyOperator)>;
	}
	return nullptr;
}

void SET_ARCH(MultiplyFun)::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet functions("*");
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			functions.AddFunction(ScalarFunction({type, type}, type, nullptr, false, SET_ARCH(BindDecimalMultiply)));
		} else if (TypeIsIntegral(type.InternalType()) && type.id() != LogicalTypeId::HUGEINT) {
			functions.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarIntegerFunction<MultiplyOperatorOverflowCheck>(type.InternalType()), false,
			    nullptr, nullptr,
			    PropagateNumericStats<SET_ARCH(TryMultiplyOperator), MultiplyPropagateStatistics,
			                          SET_ARCH(MultiplyOperator)>));
		} else {
			functions.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarBinaryFunction<SET_ARCH(MultiplyOperator)>(type.InternalType())));
		}
	}
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::BIGINT}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<interval_t, int64_t, interval_t, SET_ARCH(MultiplyOperator)>));
	functions.AddFunction(
	    ScalarFunction({LogicalType::BIGINT, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<int64_t, interval_t, interval_t, SET_ARCH(MultiplyOperator)>));
	set.AddFunction(functions);
}

//===--------------------------------------------------------------------===//
// / [divide]
//===--------------------------------------------------------------------===//
template <>
float SET_ARCH(DivideOperator)::Operation(float left, float right) {
	auto result = left / right;
	if (!Value::FloatIsValid(result)) {
		throw OutOfRangeException("Overflow in division of float!");
	}
	return result;
}

template <>
double SET_ARCH(DivideOperator)::Operation(double left, double right) {
	auto result = left / right;
	if (!Value::DoubleIsValid(result)) {
		throw OutOfRangeException("Overflow in division of double!");
	}
	return result;
}

template <>
hugeint_t SET_ARCH(DivideOperator)::Operation(hugeint_t left, hugeint_t right) {
	if (right.lower == 0 && right.upper == 0) {
		throw InternalException("Hugeint division by zero!");
	}
	return left / right;
}

template <>
interval_t SET_ARCH(DivideOperator)::Operation(interval_t left, int64_t right) {
	left.days /= right;
	left.months /= right;
	left.micros /= right;
	return left;
}

struct BinaryZeroIsNullWrapper {
	template <class FUNC, class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static inline RESULT_TYPE Operation(FUNC fun, LEFT_TYPE left, RIGHT_TYPE right, ValidityMask &mask, idx_t idx) {
		if (right == 0) {
			mask.SetInvalid(idx);
			return left;
		} else {
			return OP::template Operation<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(left, right);
		}
	}

	static bool AddsNulls() {
		return true;
	}
};

struct BinaryZeroIsNullHugeintWrapper {
	template <class FUNC, class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static inline RESULT_TYPE Operation(FUNC fun, LEFT_TYPE left, RIGHT_TYPE right, ValidityMask &mask, idx_t idx) {
		if (right.upper == 0 && right.lower == 0) {
			mask.SetInvalid(idx);
			return left;
		} else {
			return OP::template Operation<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(left, right);
		}
	}

	static bool AddsNulls() {
		return true;
	}
};

template <class TA, class TB, class TC, class OP, class ZWRAPPER = BinaryZeroIsNullWrapper>
static void BinaryScalarFunctionIgnoreZero(DataChunk &input, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<TA, TB, TC, OP, ZWRAPPER>(input.data[0], input.data[1], result, input.size());
}

template <class OP>
static scalar_function_t GetBinaryFunctionIgnoreZero(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return BinaryScalarFunctionIgnoreZero<int8_t, int8_t, int8_t, OP>;
	case LogicalTypeId::SMALLINT:
		return BinaryScalarFunctionIgnoreZero<int16_t, int16_t, int16_t, OP>;
	case LogicalTypeId::INTEGER:
		return BinaryScalarFunctionIgnoreZero<int32_t, int32_t, int32_t, OP>;
	case LogicalTypeId::BIGINT:
		return BinaryScalarFunctionIgnoreZero<int64_t, int64_t, int64_t, OP>;
	case LogicalTypeId::UTINYINT:
		return BinaryScalarFunctionIgnoreZero<uint8_t, uint8_t, uint8_t, OP>;
	case LogicalTypeId::USMALLINT:
		return BinaryScalarFunctionIgnoreZero<uint16_t, uint16_t, uint16_t, OP>;
	case LogicalTypeId::UINTEGER:
		return BinaryScalarFunctionIgnoreZero<uint32_t, uint32_t, uint32_t, OP>;
	case LogicalTypeId::UBIGINT:
		return BinaryScalarFunctionIgnoreZero<uint64_t, uint64_t, uint64_t, OP>;
	case LogicalTypeId::HUGEINT:
		return BinaryScalarFunctionIgnoreZero<hugeint_t, hugeint_t, hugeint_t, OP, BinaryZeroIsNullHugeintWrapper>;
	case LogicalTypeId::FLOAT:
		return BinaryScalarFunctionIgnoreZero<float, float, float, OP>;
	case LogicalTypeId::DOUBLE:
		return BinaryScalarFunctionIgnoreZero<double, double, double, OP>;
	default:
		throw NotImplementedException("Unimplemented type for GetScalarUnaryFunction");
	}
}

void SET_ARCH(DivideFun)::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet functions("/");
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			continue;
		} else {
			functions.AddFunction(
			    ScalarFunction({type, type}, type, GetBinaryFunctionIgnoreZero<SET_ARCH(DivideOperator)>(type)));
		}
	}
	functions.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::BIGINT}, LogicalType::INTERVAL,
	                   BinaryScalarFunctionIgnoreZero<interval_t, int64_t, interval_t, SET_ARCH(DivideOperator)>));

	set.AddFunction(functions);
}

//===--------------------------------------------------------------------===//
// % [modulo]
//===--------------------------------------------------------------------===//
template <>
float SET_ARCH(ModuloOperator)::Operation(float left, float right) {
	D_ASSERT(right != 0);
	return std::fmod(left, right);
}

template <>
double SET_ARCH(ModuloOperator)::Operation(double left, double right) {
	D_ASSERT(right != 0);
	return std::fmod(left, right);
}

template <>
hugeint_t SET_ARCH(ModuloOperator)::Operation(hugeint_t left, hugeint_t right) {
	if (right.lower == 0 && right.upper == 0) {
		throw InternalException("Hugeint division by zero!");
	}
	return left % right;
}

void SET_ARCH(ModFun)::RegisterFunction(BuiltinFunctions &set) {
	ScalarFunctionSet functions("%");
	for (auto &type : LogicalType::NUMERIC) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			continue;
		} else {
			functions.AddFunction(
			    ScalarFunction({type, type}, type, GetBinaryFunctionIgnoreZero<SET_ARCH(ModuloOperator)>(type)));
		}
	}
	set.AddFunction(functions);
	functions.name = "mod";
	set.AddFunction(functions);
}

} // namespace duckdb
