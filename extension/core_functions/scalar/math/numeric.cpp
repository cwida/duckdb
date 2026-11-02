#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/abs.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "core_functions/scalar/math_functions.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace duckdb {

template <class ERROR_OP, class IEEE_OP>
static unique_ptr<FunctionData> BindIEEEFloatingUnary(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	if (Settings::Get<IeeeFloatingPointOpsSetting>(input.GetClientContext())) {
		bound_function.SetFunctionCallback(ScalarFunction::UnaryFunction<double, double, IEEE_OP>);
	} else {
		bound_function.SetFunctionCallback(ScalarFunction::UnaryFunction<double, double, ERROR_OP>);
	}
	return nullptr;
}

template <class ERROR_OP, class IEEE_OP>
static unique_ptr<FunctionData> BindIEEEFloatingBinary(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	if (Settings::Get<IeeeFloatingPointOpsSetting>(input.GetClientContext())) {
		bound_function.SetFunctionCallback(ScalarFunction::BinaryFunction<double, double, double, IEEE_OP>);
	} else {
		bound_function.SetFunctionCallback(ScalarFunction::BinaryFunction<double, double, double, ERROR_OP>);
	}
	return nullptr;
}

// Names the unary argument "x", shared by most math functions/operators in this file.
static ScalarFunction NameXArgument(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature().AddParameter("x", type);
	return fun;
}

// Names the "x,precision" pair shared by round/round_even/trunc's binary overloads.
static ScalarFunction NameXPrecisionArguments(ScalarFunction fun, const LogicalType &type) {
	fun.GetSignature().AddParameter("x", type).AddParameter("precision", LogicalType::INTEGER);
	return fun;
}

template <class TR, class OP>
static scalar_function_t GetScalarIntegerUnaryFunctionFixedReturn(const LogicalType &type) {
	scalar_function_t function;
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		function = &ScalarFunction::UnaryFunction<int8_t, TR, OP>;
		break;
	case LogicalTypeId::SMALLINT:
		function = &ScalarFunction::UnaryFunction<int16_t, TR, OP>;
		break;
	case LogicalTypeId::INTEGER:
		function = &ScalarFunction::UnaryFunction<int32_t, TR, OP>;
		break;
	case LogicalTypeId::BIGINT:
		function = &ScalarFunction::UnaryFunction<int64_t, TR, OP>;
		break;
	case LogicalTypeId::HUGEINT:
		function = &ScalarFunction::UnaryFunction<hugeint_t, TR, OP>;
		break;
	default:
		throw NotImplementedException("Unimplemented type for GetScalarIntegerUnaryFunctionFixedReturn");
	}
	return function;
}

//===--------------------------------------------------------------------===//
// nextafter
//===--------------------------------------------------------------------===//

namespace {

struct NextAfterOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA base, TB exponent) {
		throw NotImplementedException("Unimplemented type for NextAfter Function");
	}

	template <class TA, class TB, class TR>
	static inline double Operation(double input, double approximate_to) {
		return nextafter(input, approximate_to);
	}
	template <class TA, class TB, class TR>
	static inline float Operation(float input, float approximate_to) {
		return nextafterf(input, approximate_to);
	}
};

} // namespace

ScalarFunctionSet NextAfterFun::GetFunctions() {
	ScalarFunctionSet next_after_fun;
	ScalarFunction double_fun({}, LogicalType::DOUBLE,
	                          ScalarFunction::BinaryFunction<double, double, double, NextAfterOperator>);
	double_fun.GetSignature().AddParameter("x", LogicalType::DOUBLE).AddParameter("y", LogicalType::DOUBLE);
	next_after_fun.AddFunction(double_fun);
	ScalarFunction float_fun({}, LogicalType::FLOAT,
	                         ScalarFunction::BinaryFunction<float, float, float, NextAfterOperator>);
	float_fun.GetSignature().AddParameter("x", LogicalType::FLOAT).AddParameter("y", LogicalType::FLOAT);
	next_after_fun.AddFunction(float_fun);
	return next_after_fun;
}

//===--------------------------------------------------------------------===//
// abs
//===--------------------------------------------------------------------===//
static unique_ptr<BaseStatistics> PropagateAbsStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 1);
	// can only propagate stats if the children have stats
	auto &lstats = child_stats[0];
	Value new_min, new_max;
	bool potential_overflow = true;
	if (NumericStats::HasMinMax(lstats)) {
		switch (expr.GetReturnType().InternalType()) {
		case PhysicalType::FLOAT:
		case PhysicalType::DOUBLE:
			potential_overflow = false;
			break;
		case PhysicalType::INT8:
			potential_overflow = NumericStats::Min(lstats).GetValue<int8_t>() == NumericLimits<int8_t>::Minimum();
			break;
		case PhysicalType::INT16:
			potential_overflow = NumericStats::Min(lstats).GetValue<int16_t>() == NumericLimits<int16_t>::Minimum();
			break;
		case PhysicalType::INT32:
			potential_overflow = NumericStats::Min(lstats).GetValue<int32_t>() == NumericLimits<int32_t>::Minimum();
			break;
		case PhysicalType::INT64:
			potential_overflow = NumericStats::Min(lstats).GetValue<int64_t>() == NumericLimits<int64_t>::Minimum();
			break;
		default:
			return nullptr;
		}
	}
	if (potential_overflow) {
		new_min = Value(expr.GetReturnType());
		new_max = Value(expr.GetReturnType());
	} else {
		// no potential overflow

		// compute stats
		switch (expr.GetReturnType().InternalType()) {
		case PhysicalType::FLOAT:
		case PhysicalType::DOUBLE: {
			auto current_min = NumericStats::Min(lstats).GetValue<double>();
			auto current_max = NumericStats::Max(lstats).GetValue<double>();
			if (Value::IsNan(current_min) || Value::IsNan(current_max)) {
				return nullptr;
			}

			double min_val, max_val;
			if (current_min == 0 || current_max == 0) {
				// Unlike integers, floating point abs cannot be removed for zero: abs(-0.0) clears the sign bit.
				min_val = AbsOperator::Operation<double, double>(current_min);
				max_val = AbsOperator::Operation<double, double>(current_max);
			} else if (current_min < 0 && current_max < 0) {
				min_val = AbsOperator::Operation<double, double>(current_max);
				max_val = AbsOperator::Operation<double, double>(current_min);
			} else if (current_min < 0) {
				D_ASSERT(current_max >= 0);
				min_val = 0;
				max_val = MaxValue(AbsOperator::Operation<double, double>(current_min), current_max);
			} else {
				*input.expr_ptr = std::move(input.expr.GetChildrenMutable()[0]);
				return child_stats[0].ToUnique();
			}
			new_min = expr.GetReturnType().InternalType() == PhysicalType::FLOAT
			              ? Value::FLOAT(static_cast<float>(min_val))
			              : Value::DOUBLE(min_val);
			new_max = expr.GetReturnType().InternalType() == PhysicalType::FLOAT
			              ? Value::FLOAT(static_cast<float>(max_val))
			              : Value::DOUBLE(max_val);
			break;
		}
		default: {
			auto current_min = NumericStats::Min(lstats).GetValue<int64_t>();
			auto current_max = NumericStats::Max(lstats).GetValue<int64_t>();

			int64_t min_val, max_val;
			if (current_min < 0 && current_max < 0) {
				// if both min and max are below zero, then min=abs(cur_max) and max=abs(cur_min)
				min_val = AbsValue(current_max);
				max_val = AbsValue(current_min);
			} else if (current_min < 0) {
				D_ASSERT(current_max >= 0);
				// if min is below zero and max is above 0, then min=0 and max=max(cur_max, abs(cur_min))
				min_val = 0;
				max_val = MaxValue(AbsValue(current_min), current_max);
			} else {
				// if both current_min and current_max are > 0, then the abs is a no-op and can be removed entirely
				*input.expr_ptr = std::move(input.expr.GetChildrenMutable()[0]);
				return child_stats[0].ToUnique();
			}
			new_min = Value::Numeric(expr.GetReturnType(), min_val);
			new_max = Value::Numeric(expr.GetReturnType(), max_val);
			break;
		}
		}
		expr.FunctionMutable().SetFunctionCallback(
		    ScalarFunction::GetScalarUnaryFunction<AbsOperator>(expr.GetReturnType()));
	}
	auto stats = NumericStats::CreateEmpty(expr.GetReturnType());
	NumericStats::SetMin(stats, new_min);
	NumericStats::SetMax(stats, new_max);
	stats.CopyValidity(lstats);
	return stats.ToUnique();
}

template <class OP>
static void DecimalUnaryOpResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &decimal_type = input.GetArgumentType(0);
	switch (decimal_type.InternalType()) {
	case PhysicalType::INT16:
		bound_function.SetFunctionCallback(ScalarFunction::GetScalarUnaryFunction<OP>(LogicalTypeId::SMALLINT));
		break;
	case PhysicalType::INT32:
		bound_function.SetFunctionCallback(ScalarFunction::GetScalarUnaryFunction<OP>(LogicalTypeId::INTEGER));
		break;
	case PhysicalType::INT64:
		bound_function.SetFunctionCallback(ScalarFunction::GetScalarUnaryFunction<OP>(LogicalTypeId::BIGINT));
		break;
	default:
		bound_function.SetFunctionCallback(ScalarFunction::GetScalarUnaryFunction<OP>(LogicalTypeId::HUGEINT));
		break;
	}
	bound_function.GetArguments()[0] = decimal_type;
	bound_function.SetReturnType(decimal_type);
}

ScalarFunctionSet AbsOperatorFun::GetFunctions() {
	ScalarFunctionSet abs;
	for (auto &type : LogicalType::Numeric()) {
		switch (type.id()) {
		case LogicalTypeId::DECIMAL: {
			ScalarFunction function({}, type, nullptr);
			function.SetResolveTypesCallback(DecimalUnaryOpResolveTypes<AbsOperator>);
			abs.AddFunction(NameXArgument(std::move(function), type));
			break;
		}
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::HUGEINT: {
			auto function = NameXArgument(
			    ScalarFunction({}, type, ScalarFunction::GetScalarUnaryFunction<TryAbsOperator>(type)), type);
			function.SetStatisticsCallback(PropagateAbsStats);
			abs.AddFunction(function);
			break;
		}
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE: {
			auto function = NameXArgument(
			    ScalarFunction({}, type, ScalarFunction::GetScalarUnaryFunction<AbsOperator>(type)), type);
			function.SetStatisticsCallback(PropagateAbsStats);
			abs.AddFunction(function);
			break;
		}
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
			abs.AddFunction(NameXArgument(ScalarFunction({}, type, ScalarFunction::NopFunction), type));
			break;
		default:
			abs.AddFunction(NameXArgument(
			    ScalarFunction({}, type, ScalarFunction::GetScalarUnaryFunction<AbsOperator>(type)), type));
			break;
		}
	}
	abs.SetFallible();
	return abs;
}

//===--------------------------------------------------------------------===//
// bit_count
//===--------------------------------------------------------------------===//

namespace {

struct BitCntOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		using TU = typename std::make_unsigned<TA>::type;
		TR count = 0;
		for (auto value = TU(input); value; ++count) {
			value &= (value - 1);
		}
		return count;
	}
};

struct HugeIntBitCntOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		using TU = typename std::make_unsigned<int64_t>::type;
		TR count = 0;

		for (auto value = TU(input.upper); value; ++count) {
			value &= (value - 1);
		}
		for (auto value = TU(input.lower); value; ++count) {
			value &= (value - 1);
		}
		return count;
	}
};

struct BitStringBitCntOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		TR count = Bit::BitCount(input);
		return UnsafeNumericCast<TR>(count);
	}
};

} // namespace
ScalarFunctionSet BitCountFun::GetFunctions() {
	ScalarFunctionSet functions;
	functions.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::TINYINT, ScalarFunction::UnaryFunction<int8_t, int8_t, BitCntOperator>),
	    LogicalType::TINYINT));
	functions.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::TINYINT, ScalarFunction::UnaryFunction<int16_t, int8_t, BitCntOperator>),
	    LogicalType::SMALLINT));
	functions.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::TINYINT, ScalarFunction::UnaryFunction<int32_t, int8_t, BitCntOperator>),
	    LogicalType::INTEGER));
	functions.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::TINYINT, ScalarFunction::UnaryFunction<int64_t, int8_t, BitCntOperator>),
	    LogicalType::BIGINT));
	functions.AddFunction(
	    NameXArgument(ScalarFunction({}, LogicalType::SMALLINT,
	                                 ScalarFunction::UnaryFunction<hugeint_t, int16_t, HugeIntBitCntOperator>),
	                  LogicalType::HUGEINT));
	functions.AddFunction(
	    NameXArgument(ScalarFunction({}, LogicalType::BIGINT,
	                                 ScalarFunction::UnaryFunction<string_t, int64_t, BitStringBitCntOperator>),
	                  LogicalType::BIT));
	return functions;
}

//===--------------------------------------------------------------------===//
// sign
//===--------------------------------------------------------------------===//
namespace {

struct SignOperator {
	template <class TA, class TR>
	static TR Operation(TA input) {
		if (input == TA(0)) {
			return 0;
		} else if (input > TA(0)) {
			return 1;
		} else {
			return -1;
		}
	}
};

template <>
int8_t SignOperator::Operation(float input) {
	if (input == 0 || Value::IsNan(input)) {
		return 0;
	} else if (input > 0) {
		return 1;
	} else {
		return -1;
	}
}

template <>
int8_t SignOperator::Operation(double input) {
	if (input == 0 || Value::IsNan(input)) {
		return 0;
	} else if (input > 0) {
		return 1;
	} else {
		return -1;
	}
}

// Returns whether we could safely produce output bounds.
template <class T>
bool SignStatsBounds(const BaseStatistics &input_stats, int8_t &min_sign, int8_t &max_sign) {
	auto min = NumericStats::GetMin<T>(input_stats);
	auto max = NumericStats::GetMax<T>(input_stats);
	if constexpr (std::is_floating_point<T>::value) {
		if (Value::IsNan(min) || Value::IsNan(max)) {
			return false;
		}
	}
	min_sign = SignOperator::Operation<T, int8_t>(min);
	max_sign = SignOperator::Operation<T, int8_t>(max);
	return true;
}

unique_ptr<BaseStatistics> PropagateSignStats(ClientContext &context, FunctionStatisticsInput &input) {
	(void)context;
	auto &child_stats = input.child_stats;
	D_ASSERT(child_stats.size() == 1);
	auto &input_stats = child_stats[0];
	auto result = NumericStats::CreateEmpty(LogicalType::TINYINT);
	result.CopyValidity(input_stats);
	if (!input_stats.CanHaveNoNull()) {
		return result.ToUnique();
	}
	if (!NumericStats::HasMinMax(input_stats)) {
		return nullptr;
	}

	int8_t min_sign = 0;
	int8_t max_sign = 0;
	bool success = false;
	switch (input.expr.GetChildren()[0]->GetReturnType().InternalType()) {
	case PhysicalType::INT8:
		success = SignStatsBounds<int8_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::INT16:
		success = SignStatsBounds<int16_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::INT32:
		success = SignStatsBounds<int32_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::INT64:
		success = SignStatsBounds<int64_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::INT128:
		success = SignStatsBounds<hugeint_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::UINT8:
		success = SignStatsBounds<uint8_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::UINT16:
		success = SignStatsBounds<uint16_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::UINT32:
		success = SignStatsBounds<uint32_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::UINT64:
		success = SignStatsBounds<uint64_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::UINT128:
		success = SignStatsBounds<uhugeint_t>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::FLOAT:
		success = SignStatsBounds<float>(input_stats, min_sign, max_sign);
		break;
	case PhysicalType::DOUBLE:
		success = SignStatsBounds<double>(input_stats, min_sign, max_sign);
		break;
	default:
		return nullptr;
	}
	if (!success) {
		return nullptr;
	}

	NumericStats::SetMin(result, min_sign);
	NumericStats::SetMax(result, max_sign);
	return result.ToUnique();
}

} // namespace
ScalarFunctionSet SignFun::GetFunctions() {
	ScalarFunctionSet sign;
	for (auto &type : LogicalType::Numeric()) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			continue;
		}
		auto function =
		    NameXArgument(ScalarFunction({}, LogicalType::TINYINT,
		                                 ScalarFunction::GetScalarUnaryFunctionFixedReturn<int8_t, SignOperator>(type)),
		                  type);
		function.SetStatisticsCallback(PropagateSignStats);
		sign.AddFunction(function);
	}
	return sign;
}

//===--------------------------------------------------------------------===//
// ceil
//===--------------------------------------------------------------------===//
namespace {
struct CeilOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return std::ceil(left);
	}
};
} // namespace

template <class T, class POWERS_OF_TEN, class OP>
static void GenericRoundFunctionDecimal(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	OP::template Operation<T, POWERS_OF_TEN>(input, DecimalType::GetScale(func_expr.GetChildren()[0]->GetReturnType()),
	                                         result);
}

template <class OP>
static void GenericRoundFunctionDecimalResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	// ceil essentially removes the scale
	auto &decimal_type = input.GetArgumentType(0);
	auto scale = DecimalType::GetScale(decimal_type);
	auto width = DecimalType::GetWidth(decimal_type);
	if (scale == 0) {
		bound_function.SetFunctionCallback(ScalarFunction::NopFunction);
	} else {
		switch (decimal_type.InternalType()) {
		case PhysicalType::INT16:
			bound_function.SetFunctionCallback(GenericRoundFunctionDecimal<int16_t, NumericHelper, OP>);
			break;
		case PhysicalType::INT32:
			bound_function.SetFunctionCallback(GenericRoundFunctionDecimal<int32_t, NumericHelper, OP>);
			break;
		case PhysicalType::INT64:
			bound_function.SetFunctionCallback(GenericRoundFunctionDecimal<int64_t, NumericHelper, OP>);
			break;
		default:
			bound_function.SetFunctionCallback(GenericRoundFunctionDecimal<hugeint_t, Hugeint, OP>);
			break;
		}
	}
	bound_function.GetArguments()[0] = decimal_type;
	bound_function.SetReturnType(LogicalType::DECIMAL(width, 0));
}

namespace {
struct CeilDecimalOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, uint8_t scale, Vector &result) {
		T power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[scale]);
		UnaryExecutor::Execute<T, T>(input.data[0], result, [&](T input) {
			if (input <= 0) {
				// below 0 we floor the number (e.g. -10.5 -> -10)
				return UnsafeNumericCast<T>(input / power_of_ten);
			} else {
				// above 0 we ceil the number
				return UnsafeNumericCast<T>(((input - 1) / power_of_ten) + 1);
			}
		});
	}
};
} // namespace

ScalarFunctionSet CeilFun::GetFunctions() {
	ScalarFunctionSet ceil;
	for (auto &type : LogicalType::Numeric()) {
		scalar_function_t func = nullptr;
		resolve_scalar_types_t resolve_func = nullptr;
		if (type.IsIntegral()) {
			// no ceil for integral numbers
			continue;
		}
		switch (type.id()) {
		case LogicalTypeId::FLOAT:
			func = ScalarFunction::UnaryFunction<float, float, CeilOperator>;
			break;
		case LogicalTypeId::DOUBLE:
			func = ScalarFunction::UnaryFunction<double, double, CeilOperator>;
			break;
		case LogicalTypeId::DECIMAL:
			resolve_func = GenericRoundFunctionDecimalResolveTypes<CeilDecimalOperator>;
			break;
		default:
			throw InternalException("Unimplemented numeric type for function \"ceil\"");
		}
		ScalarFunction ceil_function({}, type, func);
		ceil_function.SetResolveTypesCallback(resolve_func);
		ceil.AddFunction(NameXArgument(std::move(ceil_function), type));
	}
	ceil.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return ceil;
}

//===--------------------------------------------------------------------===//
// floor
//===--------------------------------------------------------------------===//
namespace {
struct FloorOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return std::floor(left);
	}
};

struct FloorDecimalOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, uint8_t scale, Vector &result) {
		T power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[scale]);
		UnaryExecutor::Execute<T, T>(input.data[0], result, [&](T input) {
			if (input < 0) {
				// below 0 we ceil the number (e.g. -10.5 -> -11)
				return UnsafeNumericCast<T>(((input + 1) / power_of_ten) - 1);
			} else {
				// above 0 we floor the number
				return UnsafeNumericCast<T>(input / power_of_ten);
			}
		});
	}
};
} // namespace

ScalarFunctionSet FloorFun::GetFunctions() {
	ScalarFunctionSet floor;
	for (auto &type : LogicalType::Numeric()) {
		scalar_function_t func = nullptr;
		resolve_scalar_types_t resolve_func = nullptr;
		if (type.IsIntegral()) {
			// no floor for integral numbers
			continue;
		}
		switch (type.id()) {
		case LogicalTypeId::FLOAT:
			func = ScalarFunction::UnaryFunction<float, float, FloorOperator>;
			break;
		case LogicalTypeId::DOUBLE:
			func = ScalarFunction::UnaryFunction<double, double, FloorOperator>;
			break;
		case LogicalTypeId::DECIMAL:
			resolve_func = GenericRoundFunctionDecimalResolveTypes<FloorDecimalOperator>;
			break;
		default:
			throw InternalException("Unimplemented numeric type for function \"floor\"");
		}
		ScalarFunction floor_function({}, type, func);
		floor_function.SetResolveTypesCallback(resolve_func);
		floor.AddFunction(NameXArgument(std::move(floor_function), type));
	}
	floor.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return floor;
}

//===--------------------------------------------------------------------===//
// trunc
//===--------------------------------------------------------------------===//
namespace {

struct RoundPrecisionFunctionData : public FunctionData {
	RoundPrecisionFunctionData(int32_t target_scale, uint8_t source_width, bool check_overflow)
	    : target_scale(target_scale), source_width(source_width), check_overflow(check_overflow) {
	}

	int32_t target_scale;
	uint8_t source_width;
	bool check_overflow;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RoundPrecisionFunctionData>(target_scale, source_width, check_overflow);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RoundPrecisionFunctionData>();
		return target_scale == other.target_scale && source_width == other.source_width &&
		       check_overflow == other.check_overflow;
	}
};

template <class T, class POWERS_OF_TEN, class OP>
void GenericRoundPrecisionDecimal(DataChunk &input, ExpressionState &state, Vector &result) {
	OP::template Operation<T, POWERS_OF_TEN>(input, state, result);
}

//! Resolves the types and the function of a decimal round with a precision, given the original decimal type
template <typename NEGOP, typename POSOP, bool CAN_CARRY>
unique_ptr<FunctionData> ResolveDecimalRoundPrecision(BoundScalarFunction &bound_function,
                                                      const LogicalType &decimal_type, int32_t round_value) {
	// our new precision becomes the round value
	// e.g. ROUND(DECIMAL(18,3), 1) -> DECIMAL(18,1)
	// but ONLY if the round value is positive
	// if it is negative the scale becomes zero
	// i.e. ROUND(DECIMAL(18,3), -1) -> DECIMAL(18,0)
	uint8_t target_scale;
	auto width = DecimalType::GetWidth(decimal_type);
	auto scale = DecimalType::GetScale(decimal_type);
	auto result_width = width;
	auto argument_type = decimal_type;
	bool check_overflow = false;
	if (round_value < 0) {
		target_scale = 0;
		if (CAN_CARRY && scale == 0 && round_value >= -int32_t(width)) {
			if (result_width < Decimal::MAX_WIDTH_DECIMAL) {
				result_width++;
			} else {
				check_overflow = true;
			}
		}
		auto result_type = LogicalType::DECIMAL(result_width, target_scale);
		switch (result_type.InternalType()) {
		case PhysicalType::INT16:
			bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<int16_t, NumericHelper, NEGOP>);
			break;
		case PhysicalType::INT32:
			bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<int32_t, NumericHelper, NEGOP>);
			break;
		case PhysicalType::INT64:
			bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<int64_t, NumericHelper, NEGOP>);
			break;
		default:
			bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<hugeint_t, Hugeint, NEGOP>);
			break;
		}
		if (result_type.InternalType() != decimal_type.InternalType()) {
			argument_type = LogicalType::DECIMAL(result_width, scale);
		}
	} else {
		if (round_value >= (int32_t)scale) {
			// if round_value is bigger than or equal to scale we do nothing
			bound_function.SetFunctionCallback(ScalarFunction::NopFunction);
			target_scale = scale;
		} else {
			target_scale = NumericCast<uint8_t>(round_value);
			switch (decimal_type.InternalType()) {
			case PhysicalType::INT16:
				bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<int16_t, NumericHelper, POSOP>);
				break;
			case PhysicalType::INT32:
				bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<int32_t, NumericHelper, POSOP>);
				break;
			case PhysicalType::INT64:
				bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<int64_t, NumericHelper, POSOP>);
				break;
			default:
				bound_function.SetFunctionCallback(GenericRoundPrecisionDecimal<hugeint_t, Hugeint, POSOP>);
				break;
			}
		}
	}
	bound_function.GetArguments()[0] = argument_type;
	bound_function.SetReturnType(LogicalType::DECIMAL(result_width, target_scale));
	return make_uniq<RoundPrecisionFunctionData>(round_value, width, check_overflow);
}

template <typename NEGOP, typename POSOP, bool CAN_CARRY = false>
void DecimalRoundPrecisionResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto val = input.GetNonNullConstant(1).DefaultCastAs(LogicalType::INTEGER);
	ResolveDecimalRoundPrecision<NEGOP, POSOP, CAN_CARRY>(input.GetBoundFunction(), input.GetArgumentType(0),
	                                                      IntegerValue::Get(val));
}

//! The bind data is derived from the type the function was called with, before it was cast
template <typename NEGOP, typename POSOP, bool CAN_CARRY = false>
unique_ptr<FunctionData> BindDecimalRoundPrecision(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto val = input.GetNonNullConstant(1).DefaultCastAs(LogicalType::INTEGER);
	return ResolveDecimalRoundPrecision<NEGOP, POSOP, CAN_CARRY>(
	    bound_function, bound_function.GetLogicalArguments()[0], IntegerValue::Get(val));
}

struct TruncOperatorPrecision {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB precision) {
		double trunc_value;
		if (precision < 0) {
			double modifier = std::pow(10, -TA(precision));
			trunc_value = (std::trunc(input / modifier)) * modifier;
			if (std::isinf(trunc_value) || std::isnan(trunc_value)) {
				return input;
			}
		} else {
			double modifier = std::pow(10, TA(precision));
			trunc_value = (std::trunc(input * modifier)) / modifier;
			if (std::isinf(trunc_value) || std::isnan(trunc_value)) {
				return input;
			}
		}
		return LossyNumericCast<TR>(trunc_value);
	}
};

struct TruncOperator {
	// Integer truncation is a NOP
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return std::trunc(left);
	}
};

struct TruncDecimalOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, uint8_t scale, Vector &result) {
		T power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[scale]);
		UnaryExecutor::Execute<T, T>(input.data[0], result, [&](T input) {
			//	Always floor
			return UnsafeNumericCast<T>((input / power_of_ten));
		});
	}
};

struct TruncDecimalNegativePrecisionOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<RoundPrecisionFunctionData>();
		auto source_scale = DecimalType::GetScale(func_expr.GetChildren()[0]->GetReturnType());
		auto width = DecimalType::GetWidth(func_expr.GetChildren()[0]->GetReturnType());
		if (info.target_scale <= -int32_t(width - source_scale)) {
			// scale too big for width
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			result.SetValue(0, Value::INTEGER(0));
			return;
		}
		T divide_power_of_ten =
		    UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[-info.target_scale + source_scale]);
		T multiply_power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[-info.target_scale]);

		UnaryExecutor::Execute<T, T>(input.data[0], result, [&](T input) {
			return UnsafeNumericCast<T>(input / divide_power_of_ten * multiply_power_of_ten);
		});
	}
};

struct TruncDecimalPositivePrecisionOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<RoundPrecisionFunctionData>();
		auto source_scale = DecimalType::GetScale(func_expr.GetChildren()[0]->GetReturnType());
		T power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[source_scale - info.target_scale]);
		UnaryExecutor::Execute<T, T>(input.data[0], result,
		                             [&](T input) { return UnsafeNumericCast<T>(input / power_of_ten); });
	}
};

struct TruncIntegerOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB precision) {
		if (precision < 0) {
			//	Do all the arithmetic at higher precision
			using POWERS_OF_TEN_CLASS = typename DecimalCastTraits<TA>::POWERS_OF_TEN_CLASS;
			if (precision <= -POWERS_OF_TEN_CLASS::CACHED_POWERS_OF_TEN) {
				return 0;
			}
			const auto power_of_ten = POWERS_OF_TEN_CLASS::POWERS_OF_TEN[-precision];
			auto result = input;
			result /= power_of_ten;
			if (result) {
				return UnsafeNumericCast<TR>(result * power_of_ten);
			} else {
				return 0;
			}
		} else {
			//	Truncating integers to higher precision is a NOP
			return input;
		}
	}
};

} // namespace

ScalarFunctionSet TruncFun::GetFunctions() {
	ScalarFunctionSet trunc;
	for (auto &type : LogicalType::Numeric()) {
		scalar_function_t trunc_func = nullptr;
		scalar_function_t trunc_prec_func = nullptr;
		resolve_scalar_types_t resolve_func = nullptr;
		bind_scalar_function_t bind_prec_func = nullptr;
		resolve_scalar_types_t resolve_prec_func = nullptr;
		//	Truncation of integers gets generated by some tools (e.g., Tableau/JDBC:Postgres)
		switch (type.id()) {
		case LogicalTypeId::FLOAT:
			trunc_func = ScalarFunction::UnaryFunction<float, float, TruncOperator>;
			trunc_prec_func = ScalarFunction::BinaryFunction<float, int32_t, float, TruncOperatorPrecision>;
			break;
		case LogicalTypeId::DOUBLE:
			trunc_func = ScalarFunction::UnaryFunction<double, double, TruncOperator>;
			trunc_prec_func = ScalarFunction::BinaryFunction<double, int32_t, double, TruncOperatorPrecision>;
			break;
		case LogicalTypeId::DECIMAL:
			resolve_func = GenericRoundFunctionDecimalResolveTypes<TruncDecimalOperator>;
			bind_prec_func =
			    BindDecimalRoundPrecision<TruncDecimalNegativePrecisionOperator, TruncDecimalPositivePrecisionOperator>;
			resolve_prec_func = DecimalRoundPrecisionResolveTypes<TruncDecimalNegativePrecisionOperator,
			                                                      TruncDecimalPositivePrecisionOperator>;
			break;
		case LogicalTypeId::TINYINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<int8_t, int32_t, int8_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::SMALLINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<int16_t, int32_t, int16_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::INTEGER:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::BIGINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<int64_t, int32_t, int64_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::HUGEINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<hugeint_t, int32_t, hugeint_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::UTINYINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<uint8_t, int32_t, uint8_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::USMALLINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<uint16_t, int32_t, uint16_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::UINTEGER:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<uint32_t, int32_t, uint32_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::UBIGINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<uint64_t, int32_t, uint64_t, TruncIntegerOperator>;
			break;
		case LogicalTypeId::UHUGEINT:
			trunc_func = ScalarFunction::NopFunction;
			trunc_prec_func = ScalarFunction::BinaryFunction<uhugeint_t, int32_t, uhugeint_t, TruncIntegerOperator>;
			break;
		default:
			throw InternalException("Unimplemented numeric type for function \"trunc\"");
		}
		ScalarFunction trunc_function({}, type, trunc_func);
		trunc_function.SetResolveTypesCallback(resolve_func);
		trunc.AddFunction(NameXArgument(std::move(trunc_function), type));
		ScalarFunction trunc_prec_function({}, type, trunc_prec_func, bind_prec_func);
		trunc_prec_function.SetResolveTypesCallback(resolve_prec_func);
		trunc.AddFunction(NameXPrecisionArguments(std::move(trunc_prec_function), type));
	}
	trunc.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return trunc;
}

//===--------------------------------------------------------------------===//
// round
//===--------------------------------------------------------------------===//
namespace {

template <class T, class ROUND_POLICY>
inline T RoundDivide(T input, T power_of_ten) {
	// power_of_ten is ten raised to the digits being dropped and is at least ten because the binder replaces round
	// with ScalarFunction::NopFunction if the round would not drop digits
	D_ASSERT(power_of_ten >= 10);
	T quotient;
	T remainder;
	if constexpr (std::is_same<T, hugeint_t>::value) {
		// hugeint division and modulo both run a full DivMod and discard half of the result
		quotient = Hugeint::DivMod(input, power_of_ten, remainder);
	} else {
		quotient = UnsafeNumericCast<T>(input / power_of_ten);
		remainder = UnsafeNumericCast<T>(input % power_of_ten);
	}
	if (remainder < 0) {
		remainder = UnsafeNumericCast<T>(-remainder);
	}
	T half = UnsafeNumericCast<T>(power_of_ten / 2);
	if (remainder > half || (remainder == half && ROUND_POLICY::RoundsAway(quotient))) {
		quotient = UnsafeNumericCast<T>(input < 0 ? quotient - 1 : quotient + 1);
	}
	return quotient;
}

struct RoundHalfAwayFromZero {
	static constexpr const char *Name = "ROUND";

	static double Nearest(double value) {
		return std::round(value);
	}
	template <class T>
	static bool RoundsAway(T) {
		return true;
	}
};

struct RoundHalfToEven {
	static constexpr const char *Name = "ROUND_EVEN";

	static double Nearest(double value) {
		return RoundToNearestEven(value);
	}
	template <class T>
	static bool RoundsAway(T quotient) {
		return quotient % 2 != 0;
	}
};

template <class ROUND_POLICY>
struct RoundOperatorPrecision {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB precision) {
		double rounded_value;
		if (precision < 0) {
			double modifier = std::pow(10, -TA(precision));
			if (std::isinf(modifier)) {
				// every finite value rounds to zero
				return Value::IsFinite(input) ? 0 : input;
			}
			rounded_value = ROUND_POLICY::Nearest(input / modifier) * modifier;
			if (std::isinf(rounded_value) || std::isnan(rounded_value)) {
				// the input is too large to be rounded to this precision
				return input;
			}
		} else {
			double modifier = std::pow(10, TA(precision));
			rounded_value = ROUND_POLICY::Nearest(input * modifier) / modifier;
			if (std::isinf(rounded_value) || std::isnan(rounded_value)) {
				return input;
			}
		}
		return LossyNumericCast<TR>(rounded_value);
	}
};

struct RoundOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		double rounded_value = round(input);
		if (std::isinf(rounded_value) || std::isnan(rounded_value)) {
			return input;
		}
		return LossyNumericCast<TR>(rounded_value);
	}
};

struct RoundDecimalOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, uint8_t scale, Vector &result) {
		T power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[scale]);
		T addition = power_of_ten / 2;
		// regular round rounds towards the nearest number
		// in case of a tie we round away from zero
		// i.e. -10.5 -> -11, 10.5 -> 11
		// we implement this by adding (positive) or subtracting (negative) 0.5
		// and then flooring the number
		// e.g. 10.5 + 0.5 = 11, floor(11) = 11
		//      10.4 + 0.5 = 10.9, floor(10.9) = 10
		UnaryExecutor::Execute<T, T>(input.data[0], result, [&](T input) {
			if (input < 0) {
				input -= addition;
			} else {
				input += addition;
			}
			return UnsafeNumericCast<T>(input / power_of_ten);
		});
	}
};

template <class ROUND_POLICY>
struct RoundIntegerOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB precision) {
		if (precision >= 0) {
			// Rounding integers to higher precision is a NOP
			return input;
		}
		if (precision <= -Hugeint::CACHED_POWERS_OF_TEN) {
			return 0;
		}
		const auto power_of_ten = Hugeint::POWERS_OF_TEN[-precision];
		const auto half = power_of_ten / 2;
		hugeint_t wide_input = 0;
		if constexpr (std::is_same<TA, hugeint_t>::value) {
			wide_input = input;
		} else {
			wide_input = Hugeint::Convert(input);
		}
		auto rounded = wide_input / power_of_ten;
		const auto remainder = wide_input % power_of_ten;
		if (remainder > half || (remainder == half && ROUND_POLICY::RoundsAway(rounded))) {
			rounded = Hugeint::Add(rounded, 1);
		} else if (remainder < -half || (remainder == -half && ROUND_POLICY::RoundsAway(rounded))) {
			rounded = Hugeint::Subtract(rounded, 1);
		}
		if (rounded == 0) {
			return 0;
		}
		hugeint_t rounded_value = 0;
		if (!Hugeint::TryMultiply(rounded, power_of_ten, rounded_value)) {
			throw OutOfRangeException("Overflow in %s of integer", ROUND_POLICY::Name);
		}
		TR result;
		if (!TryCast::Operation(rounded_value, result)) {
			throw OutOfRangeException("Overflow in %s of integer", ROUND_POLICY::Name);
		}
		return result;
	}
};

} // namespace

template <class ROUND_POLICY>
struct DecimalRoundNegativePrecisionOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<RoundPrecisionFunctionData>();
		auto source_scale = DecimalType::GetScale(func_expr.GetChildren()[0]->GetReturnType());
		if (info.target_scale < -int32_t(info.source_width - source_scale)) {
			// scale too big for width
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			result.SetValue(0, Value::INTEGER(0));
			return;
		}
		T divide_power_of_ten =
		    UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[-info.target_scale + source_scale]);
		T multiply_power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[-info.target_scale]);
		const bool check_overflow = info.check_overflow;

		UnaryExecutor::Execute<T, T>(input.data[0], result, [&](T input) {
			auto rounded =
			    UnsafeNumericCast<T>(RoundDivide<T, ROUND_POLICY>(input, divide_power_of_ten) * multiply_power_of_ten);
			if (check_overflow) {
				if constexpr (std::is_same_v<T, hugeint_t>) {
					if (rounded <= -Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL] ||
					    rounded >= Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL]) {
						throw OutOfRangeException("Overflow in %s of DECIMAL(38)", ROUND_POLICY::Name);
					}
				}
			}
			return rounded;
		});
	}
};

template <class ROUND_POLICY>
struct DecimalRoundPositivePrecisionOperator {
	template <class T, class POWERS_OF_TEN_CLASS>
	static void Operation(DataChunk &input, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<RoundPrecisionFunctionData>();
		auto source_scale = DecimalType::GetScale(func_expr.GetChildren()[0]->GetReturnType());
		T power_of_ten = UnsafeNumericCast<T>(POWERS_OF_TEN_CLASS::POWERS_OF_TEN[source_scale - info.target_scale]);
		UnaryExecutor::Execute<T, T>(input.data[0], result,
		                             [&](T input) { return RoundDivide<T, ROUND_POLICY>(input, power_of_ten); });
	}
};

ScalarFunctionSet RoundFun::GetFunctions() {
	ScalarFunctionSet round;
	for (auto &type : LogicalType::Numeric()) {
		scalar_function_t round_prec_func = nullptr;
		scalar_function_t round_func = nullptr;
		resolve_scalar_types_t resolve_func = nullptr;
		bind_scalar_function_t bind_prec_func = nullptr;
		resolve_scalar_types_t resolve_prec_func = nullptr;
		switch (type.id()) {
		case LogicalTypeId::FLOAT:
			round_func = ScalarFunction::UnaryFunction<float, float, RoundOperator>;
			round_prec_func =
			    ScalarFunction::BinaryFunction<float, int32_t, float, RoundOperatorPrecision<RoundHalfAwayFromZero>>;
			break;
		case LogicalTypeId::DOUBLE:
			round_func = ScalarFunction::UnaryFunction<double, double, RoundOperator>;
			round_prec_func =
			    ScalarFunction::BinaryFunction<double, int32_t, double, RoundOperatorPrecision<RoundHalfAwayFromZero>>;
			break;
		case LogicalTypeId::DECIMAL:
			resolve_func = GenericRoundFunctionDecimalResolveTypes<RoundDecimalOperator>;
			bind_prec_func =
			    BindDecimalRoundPrecision<DecimalRoundNegativePrecisionOperator<RoundHalfAwayFromZero>,
			                              DecimalRoundPositivePrecisionOperator<RoundHalfAwayFromZero>, true>;
			resolve_prec_func =
			    DecimalRoundPrecisionResolveTypes<DecimalRoundNegativePrecisionOperator<RoundHalfAwayFromZero>,
			                                      DecimalRoundPositivePrecisionOperator<RoundHalfAwayFromZero>, true>;
			break;
		case LogicalTypeId::TINYINT:
			round_func = ScalarFunction::NopFunction;
			round_prec_func =
			    ScalarFunction::BinaryFunction<int8_t, int32_t, int8_t, RoundIntegerOperator<RoundHalfAwayFromZero>>;
			break;
		case LogicalTypeId::SMALLINT:
			round_func = ScalarFunction::NopFunction;
			round_prec_func =
			    ScalarFunction::BinaryFunction<int16_t, int32_t, int16_t, RoundIntegerOperator<RoundHalfAwayFromZero>>;
			break;
		case LogicalTypeId::INTEGER:
			round_func = ScalarFunction::NopFunction;
			round_prec_func =
			    ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, RoundIntegerOperator<RoundHalfAwayFromZero>>;
			break;
		case LogicalTypeId::BIGINT:
			round_func = ScalarFunction::NopFunction;
			round_prec_func =
			    ScalarFunction::BinaryFunction<int64_t, int32_t, int64_t, RoundIntegerOperator<RoundHalfAwayFromZero>>;
			break;
		case LogicalTypeId::HUGEINT:
			round_func = ScalarFunction::NopFunction;
			round_prec_func = ScalarFunction::BinaryFunction<hugeint_t, int32_t, hugeint_t,
			                                                 RoundIntegerOperator<RoundHalfAwayFromZero>>;
			break;
		default:
			if (type.IsIntegral()) {
				// no round for integral numbers
				continue;
			}
			throw InternalException("Unimplemented numeric type for function \"round\"");
		}
		ScalarFunction round_unary_function({}, type, round_func);
		round_unary_function.SetResolveTypesCallback(resolve_func);
		auto round_function = NameXArgument(std::move(round_unary_function), type);
		ScalarFunction round_binary_function({}, type, round_prec_func, bind_prec_func);
		round_binary_function.SetResolveTypesCallback(resolve_prec_func);
		auto round_prec_function = NameXPrecisionArguments(std::move(round_binary_function), type);
		if (type.id() == LogicalTypeId::DECIMAL) {
			// rounding a DECIMAL can overflow
			round_function.SetFallible();
			round_prec_function.SetFallible();
		} else if (type.IsIntegral()) {
			// rounding an integer to a negative precision can overflow
			round_prec_function.SetFallible();
		}
		round.AddFunction(std::move(round_function));
		round.AddFunction(std::move(round_prec_function));
	}
	round.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return round;
}

//===--------------------------------------------------------------------===//
// round_even
//===--------------------------------------------------------------------===//
ScalarFunctionSet RoundEvenFun::GetFunctions() {
	ScalarFunctionSet round_even;
	for (auto &type : LogicalType::Numeric()) {
		scalar_function_t round_prec_func = nullptr;
		bind_scalar_function_t bind_prec_func = nullptr;
		resolve_scalar_types_t resolve_prec_func = nullptr;
		switch (type.id()) {
		case LogicalTypeId::FLOAT:
			round_prec_func =
			    ScalarFunction::BinaryFunction<float, int32_t, float, RoundOperatorPrecision<RoundHalfToEven>>;
			break;
		case LogicalTypeId::DOUBLE:
			round_prec_func =
			    ScalarFunction::BinaryFunction<double, int32_t, double, RoundOperatorPrecision<RoundHalfToEven>>;
			break;
		case LogicalTypeId::DECIMAL:
			bind_prec_func = BindDecimalRoundPrecision<DecimalRoundNegativePrecisionOperator<RoundHalfToEven>,
			                                           DecimalRoundPositivePrecisionOperator<RoundHalfToEven>, true>;
			resolve_prec_func =
			    DecimalRoundPrecisionResolveTypes<DecimalRoundNegativePrecisionOperator<RoundHalfToEven>,
			                                      DecimalRoundPositivePrecisionOperator<RoundHalfToEven>, true>;
			break;
		case LogicalTypeId::TINYINT:
			round_prec_func =
			    ScalarFunction::BinaryFunction<int8_t, int32_t, int8_t, RoundIntegerOperator<RoundHalfToEven>>;
			break;
		case LogicalTypeId::SMALLINT:
			round_prec_func =
			    ScalarFunction::BinaryFunction<int16_t, int32_t, int16_t, RoundIntegerOperator<RoundHalfToEven>>;
			break;
		case LogicalTypeId::INTEGER:
			round_prec_func =
			    ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, RoundIntegerOperator<RoundHalfToEven>>;
			break;
		case LogicalTypeId::BIGINT:
			round_prec_func =
			    ScalarFunction::BinaryFunction<int64_t, int32_t, int64_t, RoundIntegerOperator<RoundHalfToEven>>;
			break;
		case LogicalTypeId::HUGEINT:
			round_prec_func =
			    ScalarFunction::BinaryFunction<hugeint_t, int32_t, hugeint_t, RoundIntegerOperator<RoundHalfToEven>>;
			break;
		default:
			if (type.IsIntegral()) {
				// no round for integral numbers
				continue;
			}
			throw InternalException("Unimplemented numeric type for function \"round_even\"");
		}
		ScalarFunction round_even_binary_function({}, type, round_prec_func, bind_prec_func);
		round_even_binary_function.SetResolveTypesCallback(resolve_prec_func);
		auto round_even_function = NameXPrecisionArguments(std::move(round_even_binary_function), type);
		if (type.id() == LogicalTypeId::DECIMAL) {
			// rounding a DECIMAL can overflow
			round_even_function.SetFallible();
		} else if (type.IsIntegral()) {
			// rounding an integer to a negative precision can overflow
			round_even_function.SetFallible();
		}
		round_even.AddFunction(std::move(round_even_function));
	}
	round_even.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return round_even;
}

//===--------------------------------------------------------------------===//
// exp
//===--------------------------------------------------------------------===//
namespace {

struct ExpOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return std::exp(left);
	}
};

} // namespace

ScalarFunction ExpFun::GetFunction() {
	// not annotated monotone: libm exp is not monotone on every platform (Apple libm inverts adjacent doubles)
	return NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, ExpOperator>),
	    LogicalType::DOUBLE);
}

//===--------------------------------------------------------------------===//
// pow
//===--------------------------------------------------------------------===//
namespace {

struct PowOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA base, TB exponent) {
		if (base == 0.0 && exponent < 0.0) {
			throw OutOfRangeException("zero raised to a negative power is undefined");
		}
		return std::pow(base, exponent);
	}
};

struct IEEEPowOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA base, TB exponent) {
		return std::pow(base, exponent);
	}
};

unique_ptr<BaseStatistics> PropagatePowStats(ClientContext &context, FunctionStatisticsInput &input) {
	D_ASSERT(input.child_stats.size() == 2);
	auto &base_stats = input.child_stats[0];
	auto &exponent_stats = input.child_stats[1];
	if (!NumericStats::HasMinMax(exponent_stats)) {
		return nullptr;
	}

	auto exponent_min = NumericStats::Min(exponent_stats).GetValue<double>();
	auto exponent_max = NumericStats::Max(exponent_stats).GetValue<double>();
	double result_min;
	double result_max;
	if (exponent_min == 0 && exponent_max == 0) {
		result_min = 1;
		result_max = 1;
	} else {
		if (!Value::IsFinite(exponent_min) || exponent_min != exponent_max || exponent_min < 0 ||
		    std::trunc(exponent_min) != exponent_min) {
			return nullptr;
		}
		if (!NumericStats::HasMinMax(base_stats)) {
			return nullptr;
		}
		auto base_min = NumericStats::Min(base_stats).GetValue<double>();
		auto base_max = NumericStats::Max(base_stats).GetValue<double>();
		// Positive integer exponents have safe bounds across the complete finite base domain
		if (!Value::IsFinite(base_min) || !Value::IsFinite(base_max)) {
			return nullptr;
		}
		auto power_min = std::pow(base_min, exponent_min);
		auto power_max = std::pow(base_max, exponent_min);
		if (!Value::IsFinite(power_min) || !Value::IsFinite(power_max)) {
			return nullptr;
		}
		// Odd powers preserve order; even powers decrease toward zero and increase away from zero
		if (std::fmod(exponent_min, 2) != 0) {
			result_min = power_min;
			result_max = power_max;
		} else if (base_min <= 0 && base_max >= 0) {
			result_min = 0;
			result_max = MaxValue(power_min, power_max);
		} else if (base_max < 0) {
			result_min = power_max;
			result_max = power_min;
		} else {
			result_min = power_min;
			result_max = power_max;
		}
	}

	auto result = NumericStats::CreateEmpty(input.expr.GetReturnType());
	NumericStats::SetMin(result, Value::DOUBLE(result_min));
	NumericStats::SetMax(result, Value::DOUBLE(result_max));
	result.CombineValidity(base_stats, exponent_stats);
	return result.ToUnique();
}

} // namespace
ScalarFunction PowOperatorFun::GetFunction() {
	ScalarFunction function({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingBinary<PowOperator, IEEEPowOperator>);
	function.GetSignature().AddParameter("x", LogicalType::DOUBLE).AddParameter("y", LogicalType::DOUBLE);
	function.SetStatisticsCallback(PropagatePowStats);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// sqrt
//===--------------------------------------------------------------------===//
namespace {
struct SqrtOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < 0) {
			throw OutOfRangeException("cannot take square root of a negative number");
		}
		return std::sqrt(input);
	}
};

struct IEEESqrtOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::sqrt(input);
	}
};
} // namespace

ScalarFunction SqrtFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<SqrtOperator, IEEESqrtOperator>),
	    LogicalType::DOUBLE);
	function.SetFallible();
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// cbrt
//===--------------------------------------------------------------------===//
namespace {

struct CbRtOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return std::cbrt(left);
	}
};

} // namespace

ScalarFunction CbrtFun::GetFunction() {
	auto func = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, CbRtOperator>),
	    LogicalType::DOUBLE);
	func.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return func;
}

//===--------------------------------------------------------------------===//
// ln
//===--------------------------------------------------------------------===//
namespace {

struct LnOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < 0) {
			throw OutOfRangeException("cannot take logarithm of a negative number");
		}
		if (input == 0) {
			throw OutOfRangeException("cannot take logarithm of zero");
		}
		return std::log(input);
	}
};

struct IEEELnOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::log(input);
	}
};

} // namespace
ScalarFunction LnFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<LnOperator, IEEELnOperator>),
	    LogicalType::DOUBLE);
	function.SetFallible();
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// log
//===--------------------------------------------------------------------===//
namespace {

struct Log10Operator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < 0) {
			throw OutOfRangeException("cannot take logarithm of a negative number");
		}
		if (input == 0) {
			throw OutOfRangeException("cannot take logarithm of zero");
		}
		return std::log10(input);
	}
};

struct IEEELog10Operator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::log10(input);
	}
};

} // namespace

ScalarFunction Log10Fun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<Log10Operator, IEEELog10Operator>),
	    LogicalType::DOUBLE);
	function.SetFallible();
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// log with base
//===--------------------------------------------------------------------===//
namespace {

struct LogBaseOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA b, TB x) {
		auto divisor = Log10Operator::Operation<TA, TR>(b);
		if (divisor == 0) {
			throw OutOfRangeException("division by zero in based logarithm");
		}
		return Log10Operator::Operation<TB, TR>(x) / divisor;
	}
};

struct IEEELogBaseOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA b, TB x) {
		return IEEELog10Operator::Operation<TB, TR>(x) / IEEELog10Operator::Operation<TA, TR>(b);
	}
};

} // namespace

ScalarFunctionSet LogFun::GetFunctions() {
	ScalarFunctionSet funcs;
	auto log10 = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<Log10Operator, IEEELog10Operator>),
	    LogicalType::DOUBLE);
	// single-argument log is base-10: non-decreasing. the two-arg log(base, x) is only
	// monotone in x for a fixed base, and decreasing for base < 1, so it is left unannotated.
	log10.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	funcs.AddFunction(std::move(log10));
	ScalarFunction log_base({}, LogicalType::DOUBLE, nullptr,
	                        BindIEEEFloatingBinary<LogBaseOperator, IEEELogBaseOperator>);
	log_base.GetSignature().AddParameter("b", LogicalType::DOUBLE).AddParameter("x", LogicalType::DOUBLE);
	funcs.AddFunction(log_base);
	funcs.SetFallible();
	return funcs;
}

//===--------------------------------------------------------------------===//
// log2
//===--------------------------------------------------------------------===//
namespace {
struct Log2Operator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < 0) {
			throw OutOfRangeException("cannot take logarithm of a negative number");
		}
		if (input == 0) {
			throw OutOfRangeException("cannot take logarithm of zero");
		}
		return std::log2(input);
	}
};

struct IEEELog2Operator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::log2(input);
	}
};
} // namespace

ScalarFunction Log2Fun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<Log2Operator, IEEELog2Operator>),
	    LogicalType::DOUBLE);
	function.SetFallible();
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// pi
//===--------------------------------------------------------------------===//
static void PiFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 0);
	Value pi_value = Value::DOUBLE(PI);
	result.Reference(pi_value, count_t(args.size()));
}

ScalarFunction PiFun::GetFunction() {
	return ScalarFunction({}, LogicalType::DOUBLE, PiFunction);
}

//===--------------------------------------------------------------------===//
// degrees
//===--------------------------------------------------------------------===//
namespace {
struct DegreesOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return left * (180 / PI);
	}
};
} // namespace

ScalarFunction DegreesFun::GetFunction() {
	auto func = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, DegreesOperator>),
	    LogicalType::DOUBLE);
	func.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return func;
}

//===--------------------------------------------------------------------===//
// radians
//===--------------------------------------------------------------------===//
namespace {
struct RadiansOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return left * (PI / 180);
	}
};
} // namespace

ScalarFunction RadiansFun::GetFunction() {
	auto func = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, RadiansOperator>),
	    LogicalType::DOUBLE);
	func.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return func;
}

//===--------------------------------------------------------------------===//
// isnan
//===--------------------------------------------------------------------===//
namespace {
struct IsNanOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return Value::IsNan(input);
	}
};

static unique_ptr<BaseStatistics> PropagateIsNanStats(ClientContext &, FunctionStatisticsInput &input) {
	D_ASSERT(input.child_stats.size() == 1);
	auto &child_stats = input.child_stats[0];
	if (!NumericStats::HasMinMax(child_stats)) {
		return nullptr;
	}

	// NaN sorts above every other floating-point value, so a non-NaN maximum proves there is no NaN.
	bool max_is_nan;
	switch (input.expr.GetChildren()[0]->GetReturnType().id()) {
	case LogicalTypeId::FLOAT:
		max_is_nan = Value::IsNan(NumericStats::GetMax<float>(child_stats));
		break;
	case LogicalTypeId::DOUBLE:
		max_is_nan = Value::IsNan(NumericStats::GetMax<double>(child_stats));
		break;
	default:
		throw InternalException("Unsupported type for isnan statistics propagation");
	}
	if (max_is_nan) {
		return nullptr;
	}

	auto result = NumericStats::CreateEmpty(LogicalType::BOOLEAN);
	NumericStats::SetMin(result, false);
	NumericStats::SetMax(result, false);
	result.CopyValidity(child_stats);
	if (!child_stats.CanHaveNull()) {
		*input.expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
	}
	return result.ToUnique();
}
} // namespace

ScalarFunctionSet IsNanFun::GetFunctions() {
	ScalarFunctionSet funcs;
	auto float_function = NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<float, bool, IsNanOperator>),
	    LogicalType::FLOAT);
	float_function.SetStatisticsCallback(PropagateIsNanStats);
	funcs.AddFunction(float_function);
	auto double_function = NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<double, bool, IsNanOperator>),
	    LogicalType::DOUBLE);
	double_function.SetStatisticsCallback(PropagateIsNanStats);
	funcs.AddFunction(double_function);
	return funcs;
}

//===--------------------------------------------------------------------===//
// signbit
//===--------------------------------------------------------------------===//
namespace {
struct SignBitOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::signbit(input);
	}
};
} // namespace

ScalarFunctionSet SignBitFun::GetFunctions() {
	ScalarFunctionSet funcs;
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<float, bool, SignBitOperator>),
	    LogicalType::FLOAT));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<double, bool, SignBitOperator>),
	    LogicalType::DOUBLE));
	return funcs;
}

//===--------------------------------------------------------------------===//
// isinf
//===--------------------------------------------------------------------===//
namespace {
struct IsInfiniteOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return !Value::IsNan(input) && !Value::IsFinite(input);
	}
};

template <>
bool IsInfiniteOperator::Operation(date_t input) {
	return !Value::IsFinite(input);
}

template <>
bool IsInfiniteOperator::Operation(timestamp_t input) {
	return !Value::IsFinite(input);
}

} // namespace

ScalarFunctionSet IsInfiniteFun::GetFunctions() {
	ScalarFunctionSet funcs("isinf");
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<float, bool, IsInfiniteOperator>),
	    LogicalType::FLOAT));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<double, bool, IsInfiniteOperator>),
	    LogicalType::DOUBLE));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<date_t, bool, IsInfiniteOperator>),
	    LogicalType::DATE));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<timestamp_t, bool, IsInfiniteOperator>),
	    LogicalType::TIMESTAMP));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<timestamp_t, bool, IsInfiniteOperator>),
	    LogicalType::TIMESTAMP_TZ));
	return funcs;
}

//===--------------------------------------------------------------------===//
// isfinite
//===--------------------------------------------------------------------===//
namespace {

struct IsFiniteOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return Value::IsFinite(input);
	}
};

} // namespace

ScalarFunctionSet IsFiniteFun::GetFunctions() {
	ScalarFunctionSet funcs;
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<float, bool, IsFiniteOperator>),
	    LogicalType::FLOAT));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<double, bool, IsFiniteOperator>),
	    LogicalType::DOUBLE));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<date_t, bool, IsFiniteOperator>),
	    LogicalType::DATE));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<timestamp_t, bool, IsFiniteOperator>),
	    LogicalType::TIMESTAMP));
	funcs.AddFunction(NameXArgument(
	    ScalarFunction({}, LogicalType::BOOLEAN, ScalarFunction::UnaryFunction<timestamp_t, bool, IsFiniteOperator>),
	    LogicalType::TIMESTAMP_TZ));
	return funcs;
}

//===--------------------------------------------------------------------===//
// sin
//===--------------------------------------------------------------------===//
namespace {
template <class OP>
struct NoInfiniteDoubleWrapper {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		if (DUCKDB_UNLIKELY(!Value::IsFinite(input))) {
			if (Value::IsNan(input)) {
				return input;
			}
			throw OutOfRangeException("input value %lf is out of range for numeric function", input);
		}
		return OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input);
	}
};

struct SinOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::sin(input);
	}
};

} // namespace

ScalarFunction SinFun::GetFunction() {
	auto function =
	    NameXArgument(ScalarFunction({}, LogicalType::DOUBLE, nullptr,
	                                 BindIEEEFloatingUnary<NoInfiniteDoubleWrapper<SinOperator>, SinOperator>),
	                  LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// cos
//===--------------------------------------------------------------------===//
namespace {
struct CosOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::cos(input);
	}
};
} // namespace

ScalarFunction CosFun::GetFunction() {
	auto function =
	    NameXArgument(ScalarFunction({}, LogicalType::DOUBLE, nullptr,
	                                 BindIEEEFloatingUnary<NoInfiniteDoubleWrapper<CosOperator>, CosOperator>),
	                  LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// tan
//===--------------------------------------------------------------------===//
namespace {
struct TanOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::tan(input);
	}
};
} // namespace

ScalarFunction TanFun::GetFunction() {
	auto function =
	    NameXArgument(ScalarFunction({}, LogicalType::DOUBLE, nullptr,
	                                 BindIEEEFloatingUnary<NoInfiniteDoubleWrapper<TanOperator>, TanOperator>),
	                  LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// asin
//===--------------------------------------------------------------------===//
namespace {
struct ASinOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < -1 || input > 1) {
			throw InvalidInputException("ASIN is undefined outside [-1,1]");
		}
		return (double)std::asin(input);
	}
};

struct IEEEASinOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::asin(input);
	}
};
} // namespace

ScalarFunction AsinFun::GetFunction() {
	auto function =
	    NameXArgument(ScalarFunction({}, LogicalType::DOUBLE, nullptr,
	                                 BindIEEEFloatingUnary<NoInfiniteDoubleWrapper<ASinOperator>, IEEEASinOperator>),
	                  LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// atan
//===--------------------------------------------------------------------===//
namespace {
struct ATanOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::atan(input);
	}
};
} // namespace

ScalarFunction AtanFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, ATanOperator>),
	    LogicalType::DOUBLE);
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// atan2
//===--------------------------------------------------------------------===//
namespace {
struct ATan2 {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return (double)std::atan2(left, right);
	}
};
} // namespace

ScalarFunction Atan2Fun::GetFunction() {
	ScalarFunction function({}, LogicalType::DOUBLE, ScalarFunction::BinaryFunction<double, double, double, ATan2>);
	function.GetSignature().AddParameter("y", LogicalType::DOUBLE).AddParameter("x", LogicalType::DOUBLE);
	return function;
}

//===--------------------------------------------------------------------===//
// acos
//===--------------------------------------------------------------------===//
namespace {
struct ACos {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < -1 || input > 1) {
			throw InvalidInputException("ACOS is undefined outside [-1,1]");
		}
		return (double)std::acos(input);
	}
};

struct IEEEACos {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::acos(input);
	}
};
} // namespace

ScalarFunction AcosFun::GetFunction() {
	auto function = NameXArgument(ScalarFunction({}, LogicalType::DOUBLE, nullptr,
	                                             BindIEEEFloatingUnary<NoInfiniteDoubleWrapper<ACos>, IEEEACos>),
	                              LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// cosh
//===--------------------------------------------------------------------===//
namespace {
struct CoshOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::cosh(input);
	}
};
} // namespace

ScalarFunction CoshFun::GetFunction() {
	return NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, CoshOperator>),
	    LogicalType::DOUBLE);
}

//===--------------------------------------------------------------------===//
// acosh
//===--------------------------------------------------------------------===//
namespace {
struct AcoshOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::acosh(input);
	}
};
} // namespace

ScalarFunction AcoshFun::GetFunction() {
	return NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, AcoshOperator>),
	    LogicalType::DOUBLE);
}

//===--------------------------------------------------------------------===//
// sinh
//===--------------------------------------------------------------------===//
namespace {
struct SinhOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::sinh(input);
	}
};
} // namespace

ScalarFunction SinhFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, SinhOperator>),
	    LogicalType::DOUBLE);
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// asinh
//===--------------------------------------------------------------------===//
namespace {
struct AsinhOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::asinh(input);
	}
};
} // namespace

ScalarFunction AsinhFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, AsinhOperator>),
	    LogicalType::DOUBLE);
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// tanh
//===--------------------------------------------------------------------===//
namespace {
struct TanhOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::tanh(input);
	}
};
} // namespace

ScalarFunction TanhFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, TanhOperator>),
	    LogicalType::DOUBLE);
	function.SetUnaryArgProperties(ArgProperties().NonDecreasing());
	return function;
}

//===--------------------------------------------------------------------===//
// atanh
//===--------------------------------------------------------------------===//
namespace {
struct AtanhOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input < -1 || input > 1) {
			throw InvalidInputException("ATANH is undefined outside [-1,1]");
		}
		if (input == 1) {
			return INFINITY;
		}
		if (input == -1) {
			return -INFINITY;
		}
		return (double)std::atanh(input);
	}
};

struct IEEEAtanhOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return (double)std::atanh(input);
	}
};
} // namespace

ScalarFunction AtanhFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<AtanhOperator, IEEEAtanhOperator>),
	    LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// cot
//===--------------------------------------------------------------------===//
namespace {
template <class OP>
struct NoInfiniteNoZeroDoubleWrapper {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		if (DUCKDB_UNLIKELY(!Value::IsFinite(input))) {
			if (Value::IsNan(input)) {
				return input;
			}
			throw OutOfRangeException("input value %lf is out of range for numeric function", input);
		}
		if (DUCKDB_UNLIKELY((double)input == 0.0 || (double)input == -0.0)) {
			throw OutOfRangeException("input value %lf is out of range for numeric function cotangent", input);
		}
		return OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input);
	}
};

struct CotOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return 1.0 / (double)std::tan(input);
	}
};
} // namespace
ScalarFunction CotFun::GetFunction() {
	auto function =
	    NameXArgument(ScalarFunction({}, LogicalType::DOUBLE, nullptr,
	                                 BindIEEEFloatingUnary<NoInfiniteNoZeroDoubleWrapper<CotOperator>, CotOperator>),
	                  LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// gamma
//===--------------------------------------------------------------------===//
namespace {
struct GammaOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input == 0) {
			throw OutOfRangeException("cannot take gamma of zero");
		}
		return std::tgamma(input);
	}
};

struct IEEEGammaOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::tgamma(input);
	}
};
} // namespace

ScalarFunction GammaFun::GetFunction() {
	auto func = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<GammaOperator, IEEEGammaOperator>),
	    LogicalType::DOUBLE);
	func.SetFallible();
	return func;
}

//===--------------------------------------------------------------------===//
// gamma
//===--------------------------------------------------------------------===//
namespace {
struct LogGammaOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		if (input == 0) {
			throw OutOfRangeException("cannot take log gamma of zero");
		}
		return std::lgamma(input);
	}
};

struct IEEELogGammaOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return std::lgamma(input);
	}
};
} // namespace

ScalarFunction LogGammaFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, nullptr, BindIEEEFloatingUnary<LogGammaOperator, IEEELogGammaOperator>),
	    LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// factorial(), !
//===--------------------------------------------------------------------===//
namespace {
struct FactorialOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		if (left < 0) {
			throw OutOfRangeException("factorial of a negative number is undefined");
		}
		TR ret = 1;
		for (TA i = 2; i <= left; i++) {
			if (!TryMultiplyOperator::Operation(ret, TR(i), ret)) {
				throw OutOfRangeException("Value out of range");
			}
		}
		return ret;
	}
};
} // namespace

ScalarFunction FactorialOperatorFun::GetFunction() {
	auto function = NameXArgument(
	    ScalarFunction({}, LogicalType::HUGEINT, ScalarFunction::UnaryFunction<int32_t, hugeint_t, FactorialOperator>),
	    LogicalType::INTEGER);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// even
//===--------------------------------------------------------------------===//
namespace {
struct EvenOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		double value;
		if (left >= 0) {
			value = std::ceil(left);
		} else {
			value = std::ceil(-left);
			value = -value;
		}
		if (std::floor(value / 2) * 2 != value) {
			if (left >= 0) {
				return value += 1;
			}
			return value -= 1;
		}
		return value;
	}
};
} // namespace

ScalarFunction EvenFun::GetFunction() {
	return NameXArgument(
	    ScalarFunction({}, LogicalType::DOUBLE, ScalarFunction::UnaryFunction<double, double, EvenOperator>),
	    LogicalType::DOUBLE);
}

//===--------------------------------------------------------------------===//
// gcd
//===--------------------------------------------------------------------===//

// should be replaced with std::gcd in a newer C++ standard
namespace {
template <class TA>
TA GreatestCommonDivisor(TA left, TA right) {
	TA a = left;
	TA b = right;

	// This protects the following modulo operations from a corner case,
	// where we would get a runtime error due to an integer overflow.
	if ((left == NumericLimits<TA>::Minimum() && right == -1) ||
	    (left == -1 && right == NumericLimits<TA>::Minimum())) {
		return 1;
	}

	while (true) {
		if (a == 0) {
			return TryAbsOperator::Operation<TA, TA>(b);
		}
		b %= a;

		if (b == 0) {
			return TryAbsOperator::Operation<TA, TA>(a);
		}
		a %= b;
	}
}

struct GreatestCommonDivisorOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return GreatestCommonDivisor(left, right);
	}
};

} // namespace

ScalarFunctionSet GreatestCommonDivisorFun::GetFunctions() {
	ScalarFunctionSet funcs;
	ScalarFunction bigint_fun({}, LogicalType::BIGINT,
	                          ScalarFunction::BinaryFunction<int64_t, int64_t, int64_t, GreatestCommonDivisorOperator>);
	bigint_fun.GetSignature().AddParameter("x", LogicalType::BIGINT).AddParameter("y", LogicalType::BIGINT);
	funcs.AddFunction(bigint_fun);
	ScalarFunction hugeint_fun(
	    {}, LogicalType::HUGEINT,
	    ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, GreatestCommonDivisorOperator>);
	hugeint_fun.GetSignature().AddParameter("x", LogicalType::HUGEINT).AddParameter("y", LogicalType::HUGEINT);
	funcs.AddFunction(hugeint_fun);
	// negating the minimum value overflows, so the failure must be reportable
	funcs.SetFallible();
	return funcs;
}

//===--------------------------------------------------------------------===//
// lcm
//===--------------------------------------------------------------------===//
namespace {
// should be replaced with std::lcm in a newer C++ standard
struct LeastCommonMultipleOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		if (left == 0 || right == 0) {
			return 0;
		}
		TR result;
		if (!TryMultiplyOperator::Operation<TA, TB, TR>(left, right / GreatestCommonDivisor(left, right), result)) {
			throw OutOfRangeException("lcm value is out of range");
		}
		return TryAbsOperator::Operation<TR, TR>(result);
	}
};

} // namespace

ScalarFunctionSet LeastCommonMultipleFun::GetFunctions() {
	ScalarFunctionSet funcs;

	ScalarFunction bigint_fun({}, LogicalType::BIGINT,
	                          ScalarFunction::BinaryFunction<int64_t, int64_t, int64_t, LeastCommonMultipleOperator>);
	bigint_fun.GetSignature().AddParameter("x", LogicalType::BIGINT).AddParameter("y", LogicalType::BIGINT);
	funcs.AddFunction(bigint_fun);
	ScalarFunction hugeint_fun(
	    {}, LogicalType::HUGEINT,
	    ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, LeastCommonMultipleOperator>);
	hugeint_fun.GetSignature().AddParameter("x", LogicalType::HUGEINT).AddParameter("y", LogicalType::HUGEINT);
	funcs.AddFunction(hugeint_fun);
	funcs.SetFallible();
	return funcs;
}

//===--------------------------------------------------------------------===//
// binom(), C()
//===--------------------------------------------------------------------===//
namespace {
struct BinomOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		if (left < 0 || right < 0) {
			throw OutOfRangeException("binom with negative input is undefined");
		}
		if (left < right) {
			return 0;
		}
		TR ret = 1;
		TA n = left;
		TA k = std::min(right, left - right);
		for (TA i = 1; i <= k; i++) {
			TR numerator = TR(n - k + i);
			TR denominator = TR(i);

			auto divisor = GreatestCommonDivisor(numerator, denominator);
			numerator /= divisor;
			denominator /= divisor;

			divisor = GreatestCommonDivisor(ret, denominator);
			ret /= divisor;
			denominator /= divisor;

			// After canceling common factors, the denominator should equal 1.
			D_ASSERT(denominator == 1);

			if (!TryMultiplyOperator::Operation(ret, numerator, ret)) {
				throw OutOfRangeException("Value out of range");
			}
		}
		return ret;
	}
};
} // namespace

ScalarFunction BinomFun::GetFunction() {
	ScalarFunction function({}, LogicalType::HUGEINT,
	                        ScalarFunction::BinaryFunction<int32_t, int32_t, hugeint_t, BinomOperator>);
	function.GetSignature().AddParameter("n", LogicalType::INTEGER).AddParameter("k", LogicalType::INTEGER);
	function.SetFallible();
	return function;
}

} // namespace duckdb
