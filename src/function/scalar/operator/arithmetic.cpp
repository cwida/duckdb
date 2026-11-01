#include "duckdb/common/assert.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/settings.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/interpolate.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/negate.hpp"
#include "duckdb/common/operator/numeric_binary_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/operators.hpp"
#include "duckdb/function/scalar/operator_functions.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include <cmath>
#include <limits>

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
	case PhysicalType::INT128:
		function = &ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, OP>;
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
	case PhysicalType::UINT128:
		function = &ScalarFunction::BinaryFunction<uhugeint_t, uhugeint_t, uhugeint_t, OP>;
		break;
	default:
		throw NotImplementedException("Unimplemented type for GetScalarBinaryFunction: %s", TypeIdToString(type));
	}
	return function;
}

template <class OP>
static scalar_function_t GetScalarBinaryFunction(PhysicalType type) {
	scalar_function_t function;
	switch (type) {
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

template <class T>
static Value NumericStatsValue(const LogicalType &type, T value) {
	D_ASSERT(type.IsNumeric());
	switch (type.InternalType()) {
	case PhysicalType::FLOAT:
		return Value::FLOAT(static_cast<float>(value));
	case PhysicalType::DOUBLE:
		return Value::DOUBLE(static_cast<double>(value));
	default:
		return Value::Numeric(type, static_cast<int64_t>(value));
	}
}

static Value NumericStatsValue(const LogicalType &type, hugeint_t value) {
	D_ASSERT(type.IsNumeric());
	if (type.id() == LogicalTypeId::DECIMAL) {
		return Value::DECIMAL(value, DecimalType::GetWidth(type), DecimalType::GetScale(type));
	}
	return Value::Numeric(type, value);
}

static Value NumericStatsValue(const LogicalType &type, uint64_t value) {
	D_ASSERT(type.IsNumeric());
	return Value::Numeric(type, uhugeint_t(value));
}

static Value NumericStatsValue(const LogicalType &type, uhugeint_t value) {
	D_ASSERT(type.IsNumeric());
	return Value::Numeric(type, value);
}

template <class T>
static bool WidenFloatingBounds(const LogicalType &type, Value &new_min, Value &new_max) {
	auto min = new_min.GetValue<T>();
	auto max = new_max.GetValue<T>();
	if (!Value::IsFinite(min) || !Value::IsFinite(max)) {
		return false;
	}
	min = std::nextafter(min, -std::numeric_limits<T>::infinity());
	max = std::nextafter(max, std::numeric_limits<T>::infinity());

	new_min = NumericStatsValue(type, min);
	new_max = NumericStatsValue(type, max);
	return true;
}

template <class BASEOP>
struct FloatingTryOperator {
	template <class T>
	static bool Operation(T left, T right, T &result) {
		if (!Value::IsFinite(left) || !Value::IsFinite(right)) {
			return false;
		}
		result = BASEOP::template Operation<T, T, T>(left, right);
		return Value::IsFinite(result);
	}
};

//===--------------------------------------------------------------------===//
// + [add]
//===--------------------------------------------------------------------===//
namespace {

struct AddPropagateStatistics {
	template <class T, class OP>
	static bool Operation(const LogicalType &type, BaseStatistics &lstats, BaseStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		T min, max;
		// new min is min+min
		if (!OP::Operation(NumericStats::GetMin<T>(lstats), NumericStats::GetMin<T>(rstats), min)) {
			return true;
		}
		// new max is max+max
		if (!OP::Operation(NumericStats::GetMax<T>(lstats), NumericStats::GetMax<T>(rstats), max)) {
			return true;
		}

		new_min = NumericStatsValue(type, min);
		new_max = NumericStatsValue(type, max);
		return false;
	}
};

struct SubtractPropagateStatistics {
	template <class T, class OP>
	static bool Operation(const LogicalType &type, BaseStatistics &lstats, BaseStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		T min, max;
		if (!OP::Operation(NumericStats::GetMin<T>(lstats), NumericStats::GetMax<T>(rstats), min)) {
			return true;
		}
		if (!OP::Operation(NumericStats::GetMax<T>(lstats), NumericStats::GetMin<T>(rstats), max)) {
			return true;
		}
		new_min = NumericStatsValue(type, min);
		new_max = NumericStatsValue(type, max);
		return false;
	}
};

struct DecimalArithmeticBindData : public FunctionData {
	DecimalArithmeticBindData() : check_overflow(false) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto res = make_uniq<DecimalArithmeticBindData>();
		res->check_overflow = check_overflow;
		return std::move(res);
	}

	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<DecimalArithmeticBindData>();
		return other.check_overflow == check_overflow;
	}

	bool check_overflow;
};

template <class OP, class PROPAGATE, class BASEOP>
unique_ptr<BaseStatistics> PropagateNumericStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 2);
	// can only propagate stats if the children have stats
	auto &lstats = child_stats[0];
	auto &rstats = child_stats[1];
	Value new_min, new_max;
	bool potential_overflow = true;
	if (NumericStats::HasMinMax(lstats) && NumericStats::HasMinMax(rstats)) {
		switch (expr.GetReturnType().InternalType()) {
		case PhysicalType::INT8:
			potential_overflow =
			    PROPAGATE::template Operation<int8_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT16:
			potential_overflow =
			    PROPAGATE::template Operation<int16_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT32:
			potential_overflow =
			    PROPAGATE::template Operation<int32_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT64:
			potential_overflow =
			    PROPAGATE::template Operation<int64_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::INT128:
			potential_overflow =
			    PROPAGATE::template Operation<hugeint_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::UINT8:
			potential_overflow =
			    PROPAGATE::template Operation<uint8_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::UINT16:
			potential_overflow =
			    PROPAGATE::template Operation<uint16_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::UINT32:
			potential_overflow =
			    PROPAGATE::template Operation<uint32_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::UINT64:
			potential_overflow =
			    PROPAGATE::template Operation<uint64_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		case PhysicalType::UINT128:
			potential_overflow =
			    PROPAGATE::template Operation<uhugeint_t, OP>(expr.GetReturnType(), lstats, rstats, new_min, new_max);
			break;
		default:
			return nullptr;
		}
	}
	if (potential_overflow) {
		new_min = Value(expr.GetReturnType());
		new_max = Value(expr.GetReturnType());
	} else {
		// no potential overflow: replace with non-overflowing operator
		if (input.bind_data) {
			auto &bind_data = input.bind_data->Cast<DecimalArithmeticBindData>();
			bind_data.check_overflow = false;
		}
		expr.FunctionMutable().SetFunctionCallback(
		    GetScalarIntegerFunction<BASEOP>(expr.GetReturnType().InternalType()));
	}
	auto result = NumericStats::CreateEmpty(expr.GetReturnType());
	NumericStats::SetMin(result, new_min);
	NumericStats::SetMax(result, new_max);
	result.CombineValidity(lstats, rstats);
	return result.ToUnique();
}

template <class PROPAGATE, class BASEOP>
unique_ptr<BaseStatistics> PropagateFloatingStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 2);

	auto &return_type = expr.GetReturnType();

	auto &lstats = child_stats[0];
	auto &rstats = child_stats[1];
	if (!NumericStats::HasMinMax(lstats) || !NumericStats::HasMinMax(rstats)) {
		return nullptr;
	}

	Value new_min, new_max;
	bool failed;
	switch (return_type.InternalType()) {
	case PhysicalType::FLOAT:
		failed = PROPAGATE::template Operation<float, FloatingTryOperator<BASEOP>>(return_type, lstats, rstats, new_min,
		                                                                           new_max);
		if (failed || !WidenFloatingBounds<float>(return_type, new_min, new_max)) {
			return nullptr;
		}
		break;
	case PhysicalType::DOUBLE:
		failed = PROPAGATE::template Operation<double, FloatingTryOperator<BASEOP>>(return_type, lstats, rstats,
		                                                                            new_min, new_max);
		if (failed || !WidenFloatingBounds<double>(return_type, new_min, new_max)) {
			return nullptr;
		}
		break;
	default:
		return nullptr;
	}

	auto result = NumericStats::CreateEmpty(return_type);
	NumericStats::SetMin(result, new_min);
	NumericStats::SetMax(result, new_max);
	result.CombineValidity(lstats, rstats);
	return result.ToUnique();
}

//! Computes the result type of a decimal addition, subtraction or modulo over the given input types
template <bool IS_MODULO>
LogicalType DecimalArithmeticResultType(const vector<LogicalType> &input_types, bool &check_overflow) {
	check_overflow = false;
	// get the max width and scale of the input arguments
	uint8_t max_width = 0, max_scale = 0, max_width_over_scale = 0;
	for (auto &input_type : input_types) {
		if (input_type.id() == LogicalTypeId::UNKNOWN) {
			continue;
		}
		uint8_t width, scale;
		auto can_convert = input_type.GetDecimalProperties(width, scale);
		if (!can_convert) {
			throw BinderException("Could not convert type %s to a decimal for the arithmetic operator - add an "
			                      "explicit cast",
			                      input_type.ToString());
		}
		if (width > max_width) {
			max_width = width;
		}
		max_scale = MaxValue<uint8_t>(scale, max_scale);
		max_width_over_scale = MaxValue<uint8_t>(width - scale, max_width_over_scale);
	}
	D_ASSERT(max_width > 0);
	uint8_t required_width = MaxValue<uint8_t>(max_scale + max_width_over_scale, max_width);
	if (!IS_MODULO) {
		// for addition/subtraction, we add 1 to the width to ensure we don't overflow
		required_width = NumericCast<uint8_t>(required_width + 1);
	}
	if (required_width > Decimal::MAX_WIDTH_DECIMAL) {
		// target width does not fit in decimal at all: truncate the scale and perform overflow detection
		check_overflow = true;
		required_width = Decimal::MAX_WIDTH_DECIMAL;
	}
	return LogicalType::DECIMAL(required_width, max_scale);
}

//! Resolves the types of a decimal addition, subtraction or modulo, returns whether overflow checks are required
template <bool IS_MODULO = false>
bool ResolveDecimalArithmeticTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	bool check_overflow;
	auto argument_types = input.GetArgumentTypes();
	auto result_type = DecimalArithmeticResultType<IS_MODULO>(argument_types, check_overflow);
	// we cast all input types to the specified type
	for (idx_t i = 0; i < argument_types.size(); i++) {
		// first check if the cast is necessary
		// if the argument has a matching scale and internal type as the output type, no casting is necessary
		auto &argument_type = argument_types[i];
		uint8_t width, scale;
		argument_type.GetDecimalProperties(width, scale);
		if (scale == DecimalType::GetScale(result_type) && argument_type.InternalType() == result_type.InternalType()) {
			bound_function.GetArguments()[i] = argument_type;
		} else {
			bound_function.GetArguments()[i] = result_type;
		}
	}
	bound_function.SetReturnType(result_type);
	return check_overflow;
}

//! The overflow check is derived from the types the function was called with, before they were cast
template <bool IS_MODULO = false>
unique_ptr<FunctionData> BindDecimalArithmetic(BindScalarFunctionInput &input) {
	auto bind_data = make_uniq<DecimalArithmeticBindData>();
	DecimalArithmeticResultType<IS_MODULO>(input.GetBoundFunction().GetLogicalArguments(), bind_data->check_overflow);
	return std::move(bind_data);
}

template <class OP, class OPOVERFLOWCHECK, bool IS_SUBTRACT = false>
void DecimalAddSubtractResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto check_overflow = ResolveDecimalArithmeticTypes(input);

	// now select the physical function to execute
	auto &result_type = bound_function.GetReturnType();
	if (check_overflow) {
		bound_function.SetFunctionCallback(GetScalarBinaryFunction<OPOVERFLOWCHECK>(result_type.InternalType()));
	} else {
		bound_function.SetFunctionCallback(GetScalarBinaryFunction<OP>(result_type.InternalType()));
	}
	if (IS_SUBTRACT) {
		bound_function.SetStatisticsCallback(
		    PropagateNumericStats<TryDecimalSubtract, SubtractPropagateStatistics, SubtractOperator>);
	} else {
		bound_function.SetStatisticsCallback(PropagateNumericStats<TryDecimalAdd, AddPropagateStatistics, AddOperator>);
	}
}

void SerializeDecimalArithmetic(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                                const BoundScalarFunction &function) {
	auto &bind_data = bind_data_p->Cast<DecimalArithmeticBindData>();
	serializer.WriteProperty(100, "check_overflow", bind_data.check_overflow);
	serializer.WriteProperty(101, "return_type", function.GetReturnType());
	serializer.WriteProperty(102, "arguments", function.GetArguments());
}

// TODO this is partially duplicated from the bind
template <class OP, class OPOVERFLOWCHECK, bool IS_SUBTRACT = false>
unique_ptr<FunctionData> DeserializeDecimalArithmetic(Deserializer &deserializer, BoundScalarFunction &bound_function) {
	//	// re-change the function pointers
	auto check_overflow = deserializer.ReadProperty<bool>(100, "check_overflow");
	auto return_type = deserializer.ReadProperty<LogicalType>(101, "return_type");
	auto arguments = deserializer.ReadProperty<vector<LogicalType>>(102, "arguments");
	if (check_overflow) {
		bound_function.SetFunctionCallback(GetScalarBinaryFunction<OPOVERFLOWCHECK>(return_type.InternalType()));
	} else {
		bound_function.SetFunctionCallback(GetScalarBinaryFunction<OP>(return_type.InternalType()));
	}
	bound_function.SetStatisticsCallback(nullptr); // TODO we likely dont want to do stats prop again
	bound_function.SetReturnType(return_type);
	bound_function.GetArguments() = arguments;

	auto bind_data = make_uniq<DecimalArithmeticBindData>();
	bind_data->check_overflow = check_overflow;
	return std::move(bind_data);
}

void NopDecimalResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &decimal_type = input.GetArgumentType(0);
	bound_function.SetReturnType(decimal_type);
	bound_function.GetArguments()[0] = decimal_type;
}

} // namespace

ScalarFunction AddFunction::GetFunction(const LogicalType &type) {
	D_ASSERT(type.IsNumeric());
	ScalarFunction fn("+", {type}, type, ScalarFunction::NopFunction);
	if (type.id() == LogicalTypeId::DECIMAL) {
		fn.SetResolveTypesCallback(NopDecimalResolveTypes);
	}
	fn.SetUnaryArgProperties(ArgProperties().StrictlyIncreasing());
	return fn;
}

static void BignumAdd(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	BinaryExecutor::Execute<bignum_t, bignum_t, string_t>(args.data[0], args.data[1], result,
	                                                      [&](bignum_t a, bignum_t b) {
		                                                      const BignumIntermediate lhs(a);
		                                                      const BignumIntermediate rhs(b);
		                                                      return BignumIntermediate::Add(heap, lhs, rhs);
	                                                      });
}

static void BignumSubtract(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	// negate a copy of the right operand - the input must not be modified, it can e.g. also be the left operand
	vector<uint8_t> negated_rhs;
	BinaryExecutor::Execute<bignum_t, bignum_t, string_t>(
	    args.data[0], args.data[1], result, [&](bignum_t a, bignum_t b) {
		    const BignumIntermediate lhs(a);
		    negated_rhs.resize(b.data.GetSize());
		    memcpy(negated_rhs.data(), b.data.GetData(), b.data.GetSize());
		    BignumIntermediate rhs(negated_rhs.data(), negated_rhs.size());
		    rhs.NegateInPlace();
		    return BignumIntermediate::Add(heap, lhs, rhs);
	    });
}

//! The client context (if any) lets long BIGNUM multiplications and divisions check for interrupts
static optional_ptr<ClientContext> GetBignumArithmeticContext(ExpressionState &state) {
	return state.HasContext() ? &state.GetContext() : nullptr;
}

static void BignumMultiply(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	BignumArithmeticState arithmetic_state(GetBignumArithmeticContext(state));
	BinaryExecutor::Execute<bignum_t, bignum_t, string_t>(
	    args.data[0], args.data[1], result, [&](bignum_t a, bignum_t b) {
		    const BignumIntermediate lhs(a);
		    const BignumIntermediate rhs(b);
		    return BignumIntermediate::Multiply(heap, arithmetic_state, lhs, rhs);
	    });
}

static void BignumNegate(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<bignum_t, string_t>(args.data[0], result, [&](bignum_t a) {
		const BignumIntermediate lhs(a);
		return lhs.Negate(heap);
	});
}

ScalarFunction AddFunction::GetFunction(const LogicalType &left_type, const LogicalType &right_type) {
	const auto inc = ArgProperties().StrictlyIncreasing();
	const auto unset = ArgProperties();
	if (left_type.IsNumeric() && left_type.id() == right_type.id()) {
		if (left_type.id() == LogicalTypeId::DECIMAL) {
			auto function = ScalarFunction("+", {left_type, right_type}, left_type, nullptr, BindDecimalArithmetic);
			function.SetResolveTypesCallback(DecimalAddSubtractResolveTypes<AddOperator, DecimalAddOverflowCheck>);
			function.SetFallible();
			function.SetSerializeCallback(SerializeDecimalArithmetic);
			function.SetDeserializeCallback(DeserializeDecimalArithmetic<AddOperator, DecimalAddOverflowCheck>);
			function.SetArgProperties({inc, inc});
			return function;
		} else if (left_type.IsIntegral()) {
			ScalarFunction function("+", {left_type, right_type}, left_type,
			                        GetScalarIntegerFunction<AddOperatorOverflowCheck>(left_type.InternalType()),
			                        nullptr,
			                        PropagateNumericStats<TryAddOperator, AddPropagateStatistics, AddOperator>);
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			return function;
		} else if (left_type.IsFloating()) {
			ScalarFunction function("+", {left_type, right_type}, left_type,
			                        GetScalarBinaryFunction<AddOperator>(left_type.InternalType()));
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			function.SetStatisticsCallback(PropagateFloatingStats<AddPropagateStatistics, AddOperator>);
			return function;
		} else {
			ScalarFunction function("+", {left_type, right_type}, left_type,
			                        GetScalarBinaryFunction<AddOperator>(left_type.InternalType()));
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			return function;
		}
	}

	switch (left_type.id()) {
	case LogicalTypeId::BIGNUM:
		if (right_type.id() == LogicalTypeId::BIGNUM) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::BIGNUM, BignumAdd);
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			return function;
		}
		break;

	case LogicalTypeId::DATE:
		if (right_type.id() == LogicalTypeId::INTEGER) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::DATE,
			                        ScalarFunction::BinaryFunction<date_t, int32_t, date_t, AddOperator>);
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			return function;
		} else if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<date_t, interval_t, timestamp_t, AddOperator>);
			function.SetFallible();
			// INTERVAL ordering uses 30-day months but DATE arithmetic uses calendar months,
			// e.g. '2025-02-01' + INTERVAL '29 days' = '2025-03-02' > + INTERVAL '1 month' = '2025-03-01'.
			// Adding months clamps the day, e.g. both '2000-01-30' and '2000-01-31' + INTERVAL '1 month' are
			// '2000-02-29', so this is not strictly increasing in the DATE.
			function.SetArgProperties({ArgProperties().NonDecreasing(), unset});
			return function;
		} else if (right_type.id() == LogicalTypeId::TIME) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<date_t, dtime_t, timestamp_t, AddOperator>);
			function.SetFallible();
			return function;
		} else if (right_type.id() == LogicalTypeId::TIME_TZ) {
			// DATE + TIME_TZ -> TIMESTAMP_TZ orders in UTC, but TIME_TZ ordering does not agree
			// with UTC instant ordering — corner-evaluation picks the wrong extremum.
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP_TZ,
			                        ScalarFunction::BinaryFunction<date_t, dtime_tz_t, timestamp_t, AddOperator>);
			function.SetFallible();
			return function;
		}
		break;
	case LogicalTypeId::INTEGER:
		if (right_type.id() == LogicalTypeId::DATE) {
			ScalarFunction function("+", {left_type, right_type}, right_type,
			                        ScalarFunction::BinaryFunction<int32_t, date_t, date_t, AddOperator>);
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			return function;
		}
		break;
	case LogicalTypeId::INTERVAL:
		if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::INTERVAL,
			                        ScalarFunction::BinaryFunction<interval_t, interval_t, interval_t, AddOperator>);
			function.SetFallible();
			function.SetArgProperties({inc, inc});
			return function;
		} else if (right_type.id() == LogicalTypeId::DATE) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<interval_t, date_t, timestamp_t, AddOperator>);
			function.SetFallible();
			function.SetArgProperties({unset, ArgProperties().NonDecreasing()});
			return function;
		} else if (right_type.id() == LogicalTypeId::TIME) {
			// TIME +/- INTERVAL wraps modulo 24h.
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIME,
			                        ScalarFunction::BinaryFunction<interval_t, dtime_t, dtime_t, AddTimeOperator>);
			function.SetFallible();
			return function;
		} else if (right_type.id() == LogicalTypeId::TIME_TZ) {
			ScalarFunction function(
			    "+", {left_type, right_type}, LogicalType::TIME_TZ,
			    ScalarFunction::BinaryFunction<interval_t, dtime_tz_t, dtime_tz_t, AddTimeOperator>);
			function.SetFallible();
			return function;
		} else if (right_type.id() == LogicalTypeId::TIMESTAMP) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<interval_t, timestamp_t, timestamp_t, AddOperator>);
			function.SetFallible();
			// not monotonic: adding months clamps the day but keeps the time, e.g.
			// '2000-01-30 23:00' + INTERVAL '1 month' > '2000-01-31 01:00' + INTERVAL '1 month'
			return function;
		}
		break;
	case LogicalTypeId::TIME:
		if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIME,
			                        ScalarFunction::BinaryFunction<dtime_t, interval_t, dtime_t, AddTimeOperator>);
			function.SetFallible();
			return function;
		} else if (right_type.id() == LogicalTypeId::DATE) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<dtime_t, date_t, timestamp_t, AddOperator>);
			function.SetFallible();
			return function;
		}
		break;
	case LogicalTypeId::TIME_TZ:
		if (right_type.id() == LogicalTypeId::DATE) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP_TZ,
			                        ScalarFunction::BinaryFunction<dtime_tz_t, date_t, timestamp_t, AddOperator>);
			function.SetFallible();
			return function;
		} else if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function(
			    "+", {left_type, right_type}, LogicalType::TIME_TZ,
			    ScalarFunction::BinaryFunction<dtime_tz_t, interval_t, dtime_tz_t, AddTimeOperator>);
			function.SetFallible();
			return function;
		}
		break;
	case LogicalTypeId::TIMESTAMP:
		if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function("+", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<timestamp_t, interval_t, timestamp_t, AddOperator>);
			function.SetFallible();
			// not monotonic: see INTERVAL + TIMESTAMP
			return function;
		}
		break;
	default:
		break;
	}
	// LCOV_EXCL_START
	throw NotImplementedException("AddFunction for types %s, %s", EnumUtil::ToString(left_type.id()),
	                              EnumUtil::ToString(right_type.id()));
	// LCOV_EXCL_STOP
}

// The overloads built up in this file leave their signature's parameter names at the auto-generated "colN"
// default; name unary/binary numeric operators consistently with the rest of the catalog (x / left,right)
// without touching overloads (e.g. list_concat, merged into "+") that already have real names.
static void NameOperatorParameters(ScalarFunction &fun) {
	auto &sig = fun.GetSignature();
	if (sig.GetParameterCount() == 1) {
		if (sig.GetParameter(0).GetName() == "col0") {
			sig.GetParameter(0).SetName("x");
		}
	} else if (sig.GetParameterCount() == 2) {
		if (sig.GetParameter(0).GetName() == "col0") {
			sig.GetParameter(0).SetName("left");
		}
		if (sig.GetParameter(1).GetName() == "col1") {
			sig.GetParameter(1).SetName("right");
		}
	}
}

ScalarFunctionSet OperatorAddFun::GetFunctions() {
	ScalarFunctionSet add("+");
	for (auto &type : LogicalType::Numeric()) {
		// unary add function is a nop, but only exists for numeric types
		add.AddFunction(AddFunction::GetFunction(type));
		// binary add function adds two numbers together
		add.AddFunction(AddFunction::GetFunction(type, type));
	}
	// we can add integers to dates
	add.AddFunction(AddFunction::GetFunction(LogicalType::DATE, LogicalType::INTEGER));
	add.AddFunction(AddFunction::GetFunction(LogicalType::INTEGER, LogicalType::DATE));
	// we can add intervals together
	add.AddFunction(AddFunction::GetFunction(LogicalType::INTERVAL, LogicalType::INTERVAL));
	// we can add intervals to dates/times/timestamps
	add.AddFunction(AddFunction::GetFunction(LogicalType::DATE, LogicalType::INTERVAL));
	add.AddFunction(AddFunction::GetFunction(LogicalType::INTERVAL, LogicalType::DATE));

	add.AddFunction(AddFunction::GetFunction(LogicalType::TIME, LogicalType::INTERVAL));
	add.AddFunction(AddFunction::GetFunction(LogicalType::INTERVAL, LogicalType::TIME));

	add.AddFunction(AddFunction::GetFunction(LogicalType::TIMESTAMP, LogicalType::INTERVAL));
	add.AddFunction(AddFunction::GetFunction(LogicalType::INTERVAL, LogicalType::TIMESTAMP));

	add.AddFunction(AddFunction::GetFunction(LogicalType::TIME_TZ, LogicalType::INTERVAL));
	add.AddFunction(AddFunction::GetFunction(LogicalType::INTERVAL, LogicalType::TIME_TZ));

	// we can add times to dates
	add.AddFunction(AddFunction::GetFunction(LogicalType::TIME, LogicalType::DATE));
	add.AddFunction(AddFunction::GetFunction(LogicalType::DATE, LogicalType::TIME));

	// we can add times with time zones (offsets) to dates
	add.AddFunction(AddFunction::GetFunction(LogicalType::TIME_TZ, LogicalType::DATE));
	add.AddFunction(AddFunction::GetFunction(LogicalType::DATE, LogicalType::TIME_TZ));

	// we can add lists together
	add.AddFunction(ListConcatFun::GetFunction());

	// we can add bignums together
	add.AddFunction(AddFunction::GetFunction(LogicalType::BIGNUM, LogicalType::BIGNUM));

	add.ApplyToFunctions(NameOperatorParameters);
	return add;
}

//===--------------------------------------------------------------------===//
// - [subtract]
//===--------------------------------------------------------------------===//
template <>
interval_t NegateOperator::Operation(interval_t input) {
	interval_t result;
	result.months = NegateOperator::Operation<int32_t, int32_t>(input.months);
	result.days = NegateOperator::Operation<int32_t, int32_t>(input.days);
	result.micros = NegateOperator::Operation<int64_t, int64_t>(input.micros);
	return result;
}

static void DecimalNegateResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &decimal_type = input.GetArgumentType(0);
	auto width = DecimalType::GetWidth(decimal_type);
	if (width <= Decimal::MAX_WIDTH_INT16) {
		bound_function.SetFunctionCallback(
		    ScalarFunction::GetScalarUnaryFunction<NegateOperator>(LogicalTypeId::SMALLINT));
	} else if (width <= Decimal::MAX_WIDTH_INT32) {
		bound_function.SetFunctionCallback(
		    ScalarFunction::GetScalarUnaryFunction<NegateOperator>(LogicalTypeId::INTEGER));
	} else if (width <= Decimal::MAX_WIDTH_INT64) {
		bound_function.SetFunctionCallback(
		    ScalarFunction::GetScalarUnaryFunction<NegateOperator>(LogicalTypeId::BIGINT));
	} else {
		D_ASSERT(width <= Decimal::MAX_WIDTH_INT128);
		bound_function.SetFunctionCallback(
		    ScalarFunction::GetScalarUnaryFunction<NegateOperator>(LogicalTypeId::HUGEINT));
	}
	decimal_type.Verify();
	bound_function.GetArguments()[0] = decimal_type;
	bound_function.SetReturnType(decimal_type);
}

static void IntegerNegateResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	D_ASSERT(input.GetArgumentCount() == 1);

	// only need to promote if the argument is a constant that exactly equals the type's minimum value
	auto constant = input.TryGetConstant(0);
	if (!constant || constant->IsNull()) {
		return;
	}
	auto &type = bound_function.GetArguments()[0];
	if (*constant != Value::MinimumValue(type)) {
		return;
	}
	LogicalType promoted_type;
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		promoted_type = LogicalType::SMALLINT;
		break;
	case LogicalTypeId::SMALLINT:
		promoted_type = LogicalType::INTEGER;
		break;
	case LogicalTypeId::INTEGER:
		promoted_type = LogicalType::BIGINT;
		break;
	case LogicalTypeId::BIGINT:
		promoted_type = LogicalType::HUGEINT;
		break;
	default:
		return;
	}
	bound_function.GetArguments()[0] = promoted_type;
	bound_function.SetReturnType(promoted_type);
	bound_function.SetFunctionCallback(ScalarFunction::GetScalarUnaryFunction<NegateOperator>(promoted_type));
}

ScalarFunction SubtractFunction::GetFunction(const LogicalType &type) {
	if (type.id() == LogicalTypeId::INTERVAL) {
		ScalarFunction func("-", {type}, type, ScalarFunction::UnaryFunction<interval_t, interval_t, NegateOperator>);
		func.SetFallible();
		func.SetUnaryArgProperties(ArgProperties().StrictlyDecreasing());
		return func;
	} else if (type.id() == LogicalTypeId::DECIMAL) {
		ScalarFunction func("-", {type}, type, nullptr);
		func.SetResolveTypesCallback(DecimalNegateResolveTypes);
		func.SetUnaryArgProperties(ArgProperties().StrictlyDecreasing());
		return func;
	} else if (type.id() == LogicalTypeId::BIGNUM) {
		ScalarFunction func("+", {type}, LogicalType::BIGNUM, BignumNegate);
		func.SetUnaryArgProperties(ArgProperties().StrictlyDecreasing());
		return func;
	} else {
		D_ASSERT(type.IsNumeric());
		ScalarFunction func("-", {type}, type, ScalarFunction::GetScalarUnaryFunction<NegateOperator>(type));
		func.SetResolveTypesCallback(IntegerNegateResolveTypes);
		func.SetFallible();
		func.SetUnaryArgProperties(ArgProperties().StrictlyDecreasing());
		return func;
	}
}

ScalarFunction SubtractFunction::GetFunction(const LogicalType &left_type, const LogicalType &right_type) {
	if (left_type.IsNumeric() && left_type.id() == right_type.id()) {
		if (left_type.id() == LogicalTypeId::DECIMAL) {
			ScalarFunction function("-", {left_type, right_type}, left_type, nullptr, BindDecimalArithmetic);
			function.SetResolveTypesCallback(
			    DecimalAddSubtractResolveTypes<SubtractOperator, DecimalSubtractOverflowCheck, true>);
			function.SetFallible();
			function.SetSerializeCallback(SerializeDecimalArithmetic);
			function.SetDeserializeCallback(
			    DeserializeDecimalArithmetic<SubtractOperator, DecimalSubtractOverflowCheck>);
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;
		} else if (left_type.IsIntegral()) {
			ScalarFunction function(
			    "-", {left_type, right_type}, left_type,
			    GetScalarIntegerFunction<SubtractOperatorOverflowCheck>(left_type.InternalType()), nullptr,
			    PropagateNumericStats<TrySubtractOperator, SubtractPropagateStatistics, SubtractOperator>);
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;

		} else if (left_type.IsFloating()) {
			ScalarFunction function("-", {left_type, right_type}, left_type,
			                        GetScalarBinaryFunction<SubtractOperator>(left_type.InternalType()));
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			function.SetStatisticsCallback(PropagateFloatingStats<SubtractPropagateStatistics, SubtractOperator>);
			return function;
		} else {
			ScalarFunction function("-", {left_type, right_type}, left_type,
			                        GetScalarBinaryFunction<SubtractOperator>(left_type.InternalType()));
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;
		}
	}

	switch (left_type.id()) {
	case LogicalTypeId::BIGNUM: {
		ScalarFunction function("-", {left_type, right_type}, left_type, BignumSubtract);
		function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
		return function;
	}
	case LogicalTypeId::DATE:
		if (right_type.id() == LogicalTypeId::DATE) {
			ScalarFunction function("-", {left_type, right_type}, LogicalType::BIGINT,
			                        ScalarFunction::BinaryFunction<date_t, date_t, int64_t, SubtractOperator>);
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;

		} else if (right_type.id() == LogicalTypeId::INTEGER) {
			ScalarFunction function("-", {left_type, right_type}, LogicalType::DATE,
			                        ScalarFunction::BinaryFunction<date_t, int32_t, date_t, SubtractOperator>);
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;
		} else if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function("-", {left_type, right_type}, LogicalType::TIMESTAMP,
			                        ScalarFunction::BinaryFunction<date_t, interval_t, timestamp_t, SubtractOperator>);
			function.SetFallible();
			// Monotonic in DATE only; INTERVAL ordering uses 30-day months but DATE arithmetic uses
			// calendar months so monotonicity in the INTERVAL arg is broken (see OperatorAddFun).
			// Subtracting months clamps the day, so this is not strictly increasing in the DATE.
			function.SetArgProperties({ArgProperties().NonDecreasing(), ArgProperties()});
			return function;
		}
		break;
	case LogicalTypeId::TIMESTAMP:
		if (right_type.id() == LogicalTypeId::TIMESTAMP) {
			ScalarFunction function(
			    "-", {left_type, right_type}, LogicalType::INTERVAL,
			    ScalarFunction::BinaryFunction<timestamp_t, timestamp_t, interval_t, SubtractOperator>);
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;
		} else if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function(
			    "-", {left_type, right_type}, LogicalType::TIMESTAMP,
			    ScalarFunction::BinaryFunction<timestamp_t, interval_t, timestamp_t, SubtractOperator>);
			function.SetFallible();
			// not monotonic: subtracting months clamps the day but keeps the time, e.g.
			// '2000-03-30 23:00' - INTERVAL '1 month' > '2000-03-31 01:00' - INTERVAL '1 month'

			return function;
		}
		break;
	case LogicalTypeId::INTERVAL:
		if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function(
			    "-", {left_type, right_type}, LogicalType::INTERVAL,
			    ScalarFunction::BinaryFunction<interval_t, interval_t, interval_t, SubtractOperator>);
			function.SetFallible();
			function.SetArgProperties({ArgProperties().StrictlyIncreasing(), ArgProperties().StrictlyDecreasing()});
			return function;
		}
		break;
	case LogicalTypeId::TIME:
		if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function("-", {left_type, right_type}, LogicalType::TIME,
			                        ScalarFunction::BinaryFunction<dtime_t, interval_t, dtime_t, SubtractTimeOperator>);
			function.SetFallible();
			// TIME - INTERVAL wraps modulo 24h (e.g. 04:00 - INTERVAL '5 hours' = 23:00), so monotonicity
			// in either argument does not hold.
			return function;
		}
		break;
	case LogicalTypeId::TIME_TZ:
		if (right_type.id() == LogicalTypeId::INTERVAL) {
			ScalarFunction function(
			    "-", {left_type, right_type}, LogicalType::TIME_TZ,
			    ScalarFunction::BinaryFunction<dtime_tz_t, interval_t, dtime_tz_t, SubtractTimeOperator>);
			function.SetFallible();
			// TIME_TZ - INTERVAL wraps modulo 24h, same reasoning as TIME above.
			return function;
		}
		break;
	default:
		break;
	}
	// LCOV_EXCL_START
	throw NotImplementedException("SubtractFun for types %s, %s", EnumUtil::ToString(left_type.id()),
	                              EnumUtil::ToString(right_type.id()));
	// LCOV_EXCL_STOP
}

ScalarFunctionSet OperatorSubtractFun::GetFunctions() {
	ScalarFunctionSet subtract("-");
	for (auto &type : LogicalType::Numeric()) {
		// unary subtract function, negates the input (i.e. multiplies by -1)
		subtract.AddFunction(SubtractFunction::GetFunction(type));
		// binary subtract function "a - b", subtracts b from a
		subtract.AddFunction(SubtractFunction::GetFunction(type, type));
	}
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::BIGNUM));
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::BIGNUM, LogicalType::BIGNUM));
	// we can subtract dates from each other
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::DATE, LogicalType::DATE));
	// we can subtract integers from dates
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::DATE, LogicalType::INTEGER));
	// we can subtract timestamps from each other
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::TIMESTAMP, LogicalType::TIMESTAMP));
	// we can subtract intervals from each other
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::INTERVAL, LogicalType::INTERVAL));
	// we can subtract intervals from dates/times/timestamps, but not the other way around
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::DATE, LogicalType::INTERVAL));
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::TIME, LogicalType::INTERVAL));
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::TIMESTAMP, LogicalType::INTERVAL));
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::TIME_TZ, LogicalType::INTERVAL));
	// we can negate intervals
	subtract.AddFunction(SubtractFunction::GetFunction(LogicalType::INTERVAL));

	subtract.ApplyToFunctions(NameOperatorParameters);
	return subtract;
}

//===--------------------------------------------------------------------===//
// * [multiply]
//===--------------------------------------------------------------------===//
namespace {

struct MultiplyPropagateStatistics {
	template <class T, class OP>
	static bool Operation(const LogicalType &type, BaseStatistics &lstats, BaseStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		// statistics propagation on the multiplication is slightly less straightforward because of negative numbers
		// the new min/max depend on the signs of the input types
		// if both are positive the result is [lmin * rmin][lmax * rmax]
		// if lmin/lmax are negative the result is [lmin * rmax][lmax * rmin]
		// etc
		// rather than doing all this switcheroo we just multiply all combinations of lmin/lmax with rmin/rmax
		// and check what the minimum/maximum value is
		T lvals[] {NumericStats::GetMin<T>(lstats), NumericStats::GetMax<T>(lstats)};
		T rvals[] {NumericStats::GetMin<T>(rstats), NumericStats::GetMax<T>(rstats)};
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
		new_min = NumericStatsValue(type, min);
		new_max = NumericStatsValue(type, max);
		return false;
	}
};

//! Computes the result type of a decimal multiplication over the given input types
LogicalType DecimalMultiplyResultType(const vector<LogicalType> &input_types, bool &check_overflow) {
	check_overflow = false;
	uint8_t result_width = 0, result_scale = 0;
	uint8_t max_width = 0;
	for (auto &input_type : input_types) {
		if (input_type.id() == LogicalTypeId::UNKNOWN) {
			continue;
		}
		uint8_t width, scale;
		auto can_convert = input_type.GetDecimalProperties(width, scale);
		if (!can_convert) {
			throw BinderException("Could not convert type %s to a decimal for the arithmetic operator - add an "
			                      "explicit cast",
			                      input_type.ToString());
		}
		max_width = MaxValue<uint8_t>(width, max_width);
		result_width += width;
		result_scale += scale;
	}
	D_ASSERT(max_width > 0);
	if (result_scale > Decimal::MAX_WIDTH_DECIMAL) {
		throw OutOfRangeException(
		    "Needed scale %d to accurately represent the multiplication result, but this is out of range of the "
		    "DECIMAL type. Max scale is %d; could not perform an accurate multiplication. Either add a cast to DOUBLE, "
		    "or add an explicit cast to a decimal with a lower scale.",
		    result_scale, Decimal::MAX_WIDTH_DECIMAL);
	}
	if (result_width > Decimal::MAX_WIDTH_INT64 && max_width <= Decimal::MAX_WIDTH_INT64 &&
	    result_scale < Decimal::MAX_WIDTH_INT64) {
		check_overflow = true;
		result_width = Decimal::MAX_WIDTH_INT64;
	}
	if (result_width > Decimal::MAX_WIDTH_DECIMAL) {
		check_overflow = true;
		result_width = Decimal::MAX_WIDTH_DECIMAL;
	}
	return LogicalType::DECIMAL(result_width, result_scale);
}

void DecimalMultiplyResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	bool check_overflow;
	auto argument_types = input.GetArgumentTypes();
	auto result_type = DecimalMultiplyResultType(argument_types, check_overflow);
	auto result_width = DecimalType::GetWidth(result_type);
	// since our scale is the summation of our input scales, we do not need to cast to the result scale
	// however, we might need to cast to the correct internal type
	for (idx_t i = 0; i < argument_types.size(); i++) {
		auto &argument_type = argument_types[i];
		if (argument_type.InternalType() == result_type.InternalType()) {
			bound_function.GetArguments()[i] = argument_type;
		} else {
			uint8_t width, scale;
			if (!argument_type.GetDecimalProperties(width, scale)) {
				scale = 0;
			}

			bound_function.GetArguments()[i] = LogicalType::DECIMAL(result_width, scale);
		}
	}
	result_type.Verify();
	bound_function.SetReturnType(result_type);
	// now select the physical function to execute
	if (check_overflow) {
		bound_function.SetFunctionCallback(
		    GetScalarBinaryFunction<DecimalMultiplyOverflowCheck>(result_type.InternalType()));
	} else {
		bound_function.SetFunctionCallback(GetScalarBinaryFunction<MultiplyOperator>(result_type.InternalType()));
	}
	bound_function.SetStatisticsCallback(
	    PropagateNumericStats<TryDecimalMultiply, MultiplyPropagateStatistics, MultiplyOperator>);
}

//! The overflow check is derived from the types the function was called with, before they were cast
unique_ptr<FunctionData> BindDecimalMultiply(BindScalarFunctionInput &input) {
	auto bind_data = make_uniq<DecimalArithmeticBindData>();
	DecimalMultiplyResultType(input.GetBoundFunction().GetLogicalArguments(), bind_data->check_overflow);
	return std::move(bind_data);
}

} // namespace

ScalarFunctionSet OperatorMultiplyFun::GetFunctions() {
	ScalarFunctionSet multiply("*");
	for (auto &type : LogicalType::Numeric()) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			ScalarFunction function({type, type}, type, nullptr, BindDecimalMultiply);
			function.SetResolveTypesCallback(DecimalMultiplyResolveTypes);
			function.SetSerializeCallback(SerializeDecimalArithmetic);
			function.SetDeserializeCallback(
			    DeserializeDecimalArithmetic<MultiplyOperator, DecimalMultiplyOverflowCheck>);
			multiply.AddFunction(function);
		} else if (TypeIsIntegral(type.InternalType())) {
			multiply.AddFunction(ScalarFunction(
			    {type, type}, type, GetScalarIntegerFunction<MultiplyOperatorOverflowCheck>(type.InternalType()),
			    nullptr, PropagateNumericStats<TryMultiplyOperator, MultiplyPropagateStatistics, MultiplyOperator>));
		} else if (type.IsFloating()) {
			multiply.AddFunction(ScalarFunction({type, type}, type,
			                                    GetScalarBinaryFunction<MultiplyOperator>(type.InternalType()), nullptr,
			                                    PropagateFloatingStats<MultiplyPropagateStatistics, MultiplyOperator>));
		} else {
			multiply.AddFunction(
			    ScalarFunction({type, type}, type, GetScalarBinaryFunction<MultiplyOperator>(type.InternalType())));
		}
	}
	multiply.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::DOUBLE}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<interval_t, double, interval_t, MultiplyOperator>));
	multiply.AddFunction(
	    ScalarFunction({LogicalType::DOUBLE, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<double, interval_t, interval_t, MultiplyOperator>));
	multiply.AddFunction(
	    ScalarFunction({LogicalType::BIGINT, LogicalType::INTERVAL}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<int64_t, interval_t, interval_t, MultiplyOperator>));
	multiply.AddFunction(
	    ScalarFunction({LogicalType::INTERVAL, LogicalType::BIGINT}, LogicalType::INTERVAL,
	                   ScalarFunction::BinaryFunction<interval_t, int64_t, interval_t, MultiplyOperator>));
	// we can multiply bignums together
	multiply.AddFunction(
	    ScalarFunction({LogicalType::BIGNUM, LogicalType::BIGNUM}, LogicalType::BIGNUM, BignumMultiply));
	multiply.SetFallible();

	multiply.ApplyToFunctions(NameOperatorParameters);
	return multiply;
}

//===--------------------------------------------------------------------===//
// / [divide]
//===--------------------------------------------------------------------===//
template <>
float DivideOperator::Operation(float left, float right) {
	auto result = left / right;
	return result;
}

template <>
double DivideOperator::Operation(double left, double right) {
	auto result = left / right;
	return result;
}

template <>
hugeint_t DivideOperator::Operation(hugeint_t left, hugeint_t right) {
	if (right.lower == 0 && right.upper == 0) {
		throw InternalException("Hugeint division by zero!");
	}
	return left / right;
}

template <>
interval_t DivideOperator::Operation(interval_t left, double right) {
	interval_t result;
	if (!TryMultiplyOperator::Operation<interval_t, double, interval_t>(left, 1.0 / right, result)) {
		throw OutOfRangeException("Overflow in INTERVAL division");
	}

	return result;
}

namespace {

[[noreturn]] void ThrowDivisionByZero(const Expression &expr) {
	throw InvalidInputException("Division by zero in expression %s. Use TRY(...) to return NULL for this expression, "
	                            "or SET error_on_division_by_zero=false to return NULL for all divisions by zero.",
	                            expr.ToString());
}

template <bool NULL_ON_ZERO>
struct BinaryNumericDivideWrapper {
	template <class FUNC, class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static inline RESULT_TYPE Operation(FUNC fun, LEFT_TYPE left, RIGHT_TYPE right, ValidityMask &mask, idx_t idx) {
		if (left == NumericLimits<LEFT_TYPE>::Minimum() && right == -1) {
			if constexpr (std::is_same<OP, ModuloOperator>::value) {
				// the remainder of MIN / -1 is 0 - only the division itself overflows
				return RESULT_TYPE(0);
			} else {
				throw OutOfRangeException("Overflow in division of %d / %d", left, right);
			}
		} else if (right == 0) {
			if (!NULL_ON_ZERO) {
				ThrowDivisionByZero(fun.get());
			}
			mask.SetInvalid(idx);
			return left;
		} else {
			return OP::template Operation<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(left, right);
		}
	}

	static bool AddsNulls() {
		return NULL_ON_ZERO;
	}
};

template <bool NULL_ON_ZERO>
struct BinaryZeroCheckWrapper {
	template <class FUNC, class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static inline RESULT_TYPE Operation(FUNC fun, LEFT_TYPE left, RIGHT_TYPE right, ValidityMask &mask, idx_t idx) {
		if (right == 0) {
			if (!NULL_ON_ZERO) {
				ThrowDivisionByZero(fun.get());
			}
			mask.SetInvalid(idx);
			return left;
		} else {
			return OP::template Operation<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(left, right);
		}
	}

	static bool AddsNulls() {
		return NULL_ON_ZERO;
	}
};

template <bool NULL_ON_ZERO>
struct BinaryNumericDivideHugeintWrapper {
	template <class FUNC, class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static inline RESULT_TYPE Operation(FUNC fun, LEFT_TYPE left, RIGHT_TYPE right, ValidityMask &mask, idx_t idx) {
		if (left == NumericLimits<LEFT_TYPE>::Minimum() && right == -1) {
			if constexpr (std::is_same<OP, ModuloOperator>::value) {
				// the remainder of MIN / -1 is 0 - only the division itself overflows
				return RESULT_TYPE(0);
			} else {
				throw OutOfRangeException("Overflow in division of %s / %s", left.ToString(), right.ToString());
			}
		} else if (right == 0) {
			if (!NULL_ON_ZERO) {
				ThrowDivisionByZero(fun.get());
			}
			mask.SetInvalid(idx);
			return left;
		} else {
			return OP::template Operation<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE>(left, right);
		}
	}

	static bool AddsNulls() {
		return NULL_ON_ZERO;
	}
};

template <class TA, class TB, class TC, class OP, class ZWRAPPER>
void BinaryScalarFunctionZeroCheck(DataChunk &input, ExpressionState &state, Vector &result) {
	BinaryExecutor::ExecuteWithWrapper<TA, TB, TC, OP, ZWRAPPER>(input.data[0], input.data[1], result, input.size(),
	                                                             reference<const Expression>(state.expr));
}

template <class OP, bool NULL_ON_ZERO>
scalar_function_t GetBinaryFunctionZeroCheck(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT8:
		return BinaryScalarFunctionZeroCheck<int8_t, int8_t, int8_t, OP, BinaryNumericDivideWrapper<NULL_ON_ZERO>>;
	case PhysicalType::INT16:
		return BinaryScalarFunctionZeroCheck<int16_t, int16_t, int16_t, OP, BinaryNumericDivideWrapper<NULL_ON_ZERO>>;
	case PhysicalType::INT32:
		return BinaryScalarFunctionZeroCheck<int32_t, int32_t, int32_t, OP, BinaryNumericDivideWrapper<NULL_ON_ZERO>>;
	case PhysicalType::INT64:
		return BinaryScalarFunctionZeroCheck<int64_t, int64_t, int64_t, OP, BinaryNumericDivideWrapper<NULL_ON_ZERO>>;
	case PhysicalType::UINT8:
		return BinaryScalarFunctionZeroCheck<uint8_t, uint8_t, uint8_t, OP, BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	case PhysicalType::UINT16:
		return BinaryScalarFunctionZeroCheck<uint16_t, uint16_t, uint16_t, OP, BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	case PhysicalType::UINT32:
		return BinaryScalarFunctionZeroCheck<uint32_t, uint32_t, uint32_t, OP, BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	case PhysicalType::UINT64:
		return BinaryScalarFunctionZeroCheck<uint64_t, uint64_t, uint64_t, OP, BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	case PhysicalType::INT128:
		return BinaryScalarFunctionZeroCheck<hugeint_t, hugeint_t, hugeint_t, OP,
		                                     BinaryNumericDivideHugeintWrapper<NULL_ON_ZERO>>;
	case PhysicalType::UINT128:
		return BinaryScalarFunctionZeroCheck<uhugeint_t, uhugeint_t, uhugeint_t, OP,
		                                     BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	case PhysicalType::FLOAT:
		return BinaryScalarFunctionZeroCheck<float, float, float, OP, BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	case PhysicalType::DOUBLE:
		return BinaryScalarFunctionZeroCheck<double, double, double, OP, BinaryZeroCheckWrapper<NULL_ON_ZERO>>;
	default:
		throw NotImplementedException("Unimplemented type for GetBinaryFunctionZeroCheck");
	}
}

template <class OP>
scalar_function_t GetBinaryFunctionZeroCheck(PhysicalType type, bool null_on_zero) {
	if (null_on_zero) {
		return GetBinaryFunctionZeroCheck<OP, true>(type);
	}
	return GetBinaryFunctionZeroCheck<OP, false>(type);
}

template <class OP>
unique_ptr<FunctionData> BindDivisionByZero(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto null_on_zero = !Settings::Get<ErrorOnDivisionByZeroSetting>(context);
	bound_function.SetFunctionCallback(
	    GetBinaryFunctionZeroCheck<OP>(bound_function.GetReturnType().InternalType(), null_on_zero));
	return nullptr;
}

unique_ptr<FunctionData> BindIntervalDivide(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	if (!Settings::Get<ErrorOnDivisionByZeroSetting>(context)) {
		bound_function.SetFunctionCallback(BinaryScalarFunctionZeroCheck<interval_t, double, interval_t, DivideOperator,
		                                                                 BinaryZeroCheckWrapper<true>>);
	} else {
		bound_function.SetFunctionCallback(BinaryScalarFunctionZeroCheck<interval_t, double, interval_t, DivideOperator,
		                                                                 BinaryZeroCheckWrapper<false>>);
	}
	return nullptr;
}

template <class OP>
unique_ptr<FunctionData> BindBinaryFloatingPoint(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();

	if (Settings::Get<IeeeFloatingPointOpsSetting>(context)) {
		bound_function.SetFunctionCallback(GetScalarBinaryFunction<OP>(bound_function.GetReturnType().InternalType()));
	} else {
		auto null_on_zero = !Settings::Get<ErrorOnDivisionByZeroSetting>(context);
		bound_function.SetFunctionCallback(
		    GetBinaryFunctionZeroCheck<OP>(bound_function.GetReturnType().InternalType(), null_on_zero));
	}
	return nullptr;
}

struct DividePropagateStatistics {
	template <class T, class OP>
	static bool Operation(const LogicalType &type, BaseStatistics &lstats, BaseStatistics &rstats, Value &new_min,
	                      Value &new_max) {
		D_ASSERT(NumericStats::HasMinMax(lstats) && NumericStats::HasMinMax(rstats));
		// A divisor range that contains zero produces NULLs, and the result grows without
		// bound as the divisor approaches zero - no statistics can be derived
		if (NumericStats::GetMin<T>(rstats) <= T(0) && NumericStats::GetMax<T>(rstats) >= T(0)) {
			return true;
		}
		// The extremes depend on the signs of the inputs: evaluate all combinations of the bounds and take the
		// minimum/maximum
		T lvals[] {NumericStats::GetMin<T>(lstats), NumericStats::GetMax<T>(lstats)};
		T rvals[] {NumericStats::GetMin<T>(rstats), NumericStats::GetMax<T>(rstats)};
		T min = NumericLimits<T>::Maximum();
		T max = NumericLimits<T>::Minimum();
		for (idx_t l = 0; l < 2; l++) {
			for (idx_t r = 0; r < 2; r++) {
				T result;
				if (!OP::Operation(lvals[l], rvals[r], result)) {
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
		new_min = NumericStatsValue(type, min);
		new_max = NumericStatsValue(type, max);
		return false;
	}
};

// When propagation fails the divisor range may contain zero, and division by zero either raises or (with
// error_on_division_by_zero=false) produces NULLs that the inputs do not have: return no statistics instead of the
// validity-only statistics that PropagateNumericStats falls back to.
unique_ptr<BaseStatistics> PropagateIntegerDivideStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto stats = PropagateNumericStats<TryDivideOperator, DividePropagateStatistics, DivideOperator>(context, input);
	if (stats && !NumericStats::HasMinMax(*stats)) {
		return nullptr;
	}
	return stats;
}

} // namespace

//===--------------------------------------------------------------------===//
// BIGNUM division / modulo
//===--------------------------------------------------------------------===//
template <bool NULL_ON_ZERO, bool MODULO>
static void BignumDivideOperation(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	BignumArithmeticState arithmetic_state(GetBignumArithmeticContext(state));
	BinaryExecutor::Execute<bignum_t, bignum_t, string_t>(
	    args.data[0], args.data[1], result, [&](bignum_t a, bignum_t b) -> optional<string_t> {
		    const BignumIntermediate lhs(a);
		    const BignumIntermediate rhs(b);
		    string_t quotient_or_remainder;
		    if (!BignumIntermediate::TryDivMod(heap, arithmetic_state, lhs, rhs, MODULO, quotient_or_remainder)) {
			    if (!NULL_ON_ZERO) {
				    ThrowDivisionByZero(state.expr);
			    }
			    return nullopt;
		    }
		    return quotient_or_remainder;
	    });
}

template <bool MODULO>
static unique_ptr<FunctionData> BindBignumDivide(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto null_on_zero = !Settings::Get<ErrorOnDivisionByZeroSetting>(input.GetClientContext());
	if (null_on_zero) {
		bound_function.SetFunctionCallback(BignumDivideOperation<true, MODULO>);
	} else {
		bound_function.SetFunctionCallback(BignumDivideOperation<false, MODULO>);
	}
	return nullptr;
}

ScalarFunctionSet OperatorFloatDivideFun::GetFunctions() {
	ScalarFunctionSet fp_divide("/");
	fp_divide.AddFunction(ScalarFunction({LogicalType::FLOAT, LogicalType::FLOAT}, LogicalType::FLOAT, nullptr,
	                                     BindBinaryFloatingPoint<DivideOperator>,
	                                     PropagateFloatingStats<DividePropagateStatistics, DivideOperator>));
	fp_divide.AddFunction(ScalarFunction({LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE, nullptr,
	                                     BindBinaryFloatingPoint<DivideOperator>,
	                                     PropagateFloatingStats<DividePropagateStatistics, DivideOperator>));
	fp_divide.AddFunction(ScalarFunction({LogicalType::INTERVAL, LogicalType::DOUBLE}, LogicalType::INTERVAL, nullptr,
	                                     BindIntervalDivide));
	fp_divide.SetFallible();
	fp_divide.ApplyToFunctions(NameOperatorParameters);
	return fp_divide;
}

ScalarFunctionSet OperatorIntegerDivideFun::GetFunctions() {
	ScalarFunctionSet full_divide("//");
	for (auto &type : LogicalType::Numeric()) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			continue;
		} else if (TypeIsIntegral(type.InternalType())) {
			full_divide.AddFunction(ScalarFunction({type, type}, type, nullptr, BindDivisionByZero<DivideOperator>,
			                                       PropagateIntegerDivideStats));
		} else {
			full_divide.AddFunction(ScalarFunction({type, type}, type, nullptr, BindDivisionByZero<DivideOperator>));
		}
	}
	// we can divide bignums by bignums
	full_divide.AddFunction(ScalarFunction({LogicalType::BIGNUM, LogicalType::BIGNUM}, LogicalType::BIGNUM, nullptr,
	                                       BindBignumDivide<false>));
	full_divide.SetFallible();
	full_divide.ApplyToFunctions(NameOperatorParameters);
	return full_divide;
}

//===--------------------------------------------------------------------===//
// % [modulo]
//===--------------------------------------------------------------------===//
template <class OP>
static void DecimalModuloResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto check_overflow = ResolveDecimalArithmeticTypes<true>(input);
	// now select the physical function to execute
	if (check_overflow) {
		// fallback to DOUBLE if the decimal type is not guaranteed to fit within the max decimal width
		for (auto &arg : bound_function.GetArguments()) {
			arg = LogicalType::DOUBLE;
		}
		bound_function.SetReturnType(LogicalType::DOUBLE);
	}
	auto &result_type = bound_function.GetReturnType();
	auto null_on_zero = !Settings::Get<ErrorOnDivisionByZeroSetting>(input.GetClientContext());
	bound_function.SetFunctionCallback(GetBinaryFunctionZeroCheck<OP>(result_type.InternalType(), null_on_zero));
}

template <>
float ModuloOperator::Operation(float left, float right) {
	auto result = std::fmod(left, right);
	return result;
}

template <>
double ModuloOperator::Operation(double left, double right) {
	auto result = std::fmod(left, right);
	return result;
}

template <>
hugeint_t ModuloOperator::Operation(hugeint_t left, hugeint_t right) {
	if (right.lower == 0 && right.upper == 0) {
		throw InternalException("Hugeint division by zero!");
	}
	return left % right;
}

namespace {

// a % b keeps the sign of a with |a % b| <= |a| and |a % b| < |b|; when |a| < |b| for every pair the result is a
template <class T>
bool SignedModuloBounds(const LogicalType &type, const BaseStatistics &lstats, const BaseStatistics &rstats,
                        Value &new_min, Value &new_max, bool &zero_divisor) {
	auto lmin = NumericStats::GetMin<T>(lstats);
	auto lmax = NumericStats::GetMax<T>(lstats);
	auto rmin = NumericStats::GetMin<T>(rstats);
	auto rmax = NumericStats::GetMax<T>(rstats);
	const T zero(0);
	zero_divisor = rmin <= zero && rmax >= zero;
	if (rmin == zero && rmax == zero) {
		return false;
	}
	T result_min = lmin < zero ? lmin : zero;
	T result_max = lmax > zero ? lmax : zero;
	// |rmin| is not representable for the minimum value, and the bound it implies is the full type range anyway
	if (rmin != NumericLimits<T>::Minimum()) {
		T abs_rmin = rmin < zero ? T(-rmin) : rmin;
		T abs_rmax = rmax < zero ? T(-rmax) : rmax;
		T max_abs = MaxValue(abs_rmin, abs_rmax);
		T min_abs = zero_divisor ? zero : MinValue(abs_rmin, abs_rmax);
		if (!zero_divisor && T(-min_abs) < lmin && lmax < min_abs) {
			result_min = lmin;
			result_max = lmax;
		} else {
			T bound = T(max_abs - T(1));
			result_min = MaxValue(result_min, T(-bound));
			result_max = MinValue(result_max, bound);
		}
	}
	new_min = NumericStatsValue(type, result_min);
	new_max = NumericStatsValue(type, result_max);
	return true;
}

template <class T>
bool UnsignedModuloBounds(const LogicalType &type, const BaseStatistics &lstats, const BaseStatistics &rstats,
                          Value &new_min, Value &new_max, bool &zero_divisor) {
	auto lmin = NumericStats::GetMin<T>(lstats);
	auto lmax = NumericStats::GetMax<T>(lstats);
	auto rmin = NumericStats::GetMin<T>(rstats);
	auto rmax = NumericStats::GetMax<T>(rstats);
	const T zero(0);
	zero_divisor = rmin == zero;
	if (rmax == zero) {
		return false;
	}
	T result_min = zero;
	T result_max = lmax;
	if (lmax < rmin) {
		result_min = lmin;
	} else {
		result_max = MinValue(result_max, T(rmax - T(1)));
	}
	new_min = NumericStatsValue(type, result_min);
	new_max = NumericStatsValue(type, result_max);
	return true;
}

template <class T>
bool FloatingModuloBounds(const LogicalType &type, const BaseStatistics &lstats, const BaseStatistics &rstats,
                          Value &new_min, Value &new_max, bool &zero_divisor) {
	auto lmin = NumericStats::GetMin<T>(lstats);
	auto lmax = NumericStats::GetMax<T>(lstats);
	auto rmin = NumericStats::GetMin<T>(rstats);
	auto rmax = NumericStats::GetMax<T>(rstats);
	if (!Value::IsFinite(lmin) || !Value::IsFinite(lmax) || !Value::IsFinite(rmin) || !Value::IsFinite(rmax)) {
		return false;
	}
	zero_divisor = rmin <= 0 && rmax >= 0;
	if (rmin == 0 && rmax == 0) {
		return false;
	}
	T result_min = lmin < 0 ? lmin : 0;
	T result_max = lmax > 0 ? lmax : 0;
	T abs_rmin = std::fabs(rmin);
	T abs_rmax = std::fabs(rmax);
	T max_abs = MaxValue(abs_rmin, abs_rmax);
	T min_abs = zero_divisor ? 0 : MinValue(abs_rmin, abs_rmax);
	if (!zero_divisor && -min_abs < lmin && lmax < min_abs) {
		result_min = lmin;
		result_max = lmax;
	} else {
		result_min = MaxValue(result_min, -max_abs);
		result_max = MinValue(result_max, max_abs);
	}
	new_min = NumericStatsValue(type, result_min);
	new_max = NumericStatsValue(type, result_max);
	return true;
}

template <class T>
void SetFullRange(const LogicalType &type, BaseStatistics &stats) {
	NumericStats::SetMin(stats, NumericStatsValue(type, NumericLimits<T>::Minimum()));
	NumericStats::SetMax(stats, NumericStatsValue(type, NumericLimits<T>::Maximum()));
}

bool ModuloFullRange(const LogicalType &type, BaseStatistics &stats) {
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		SetFullRange<int8_t>(type, stats);
		break;
	case PhysicalType::INT16:
		SetFullRange<int16_t>(type, stats);
		break;
	case PhysicalType::INT32:
		SetFullRange<int32_t>(type, stats);
		break;
	case PhysicalType::INT64:
		SetFullRange<int64_t>(type, stats);
		break;
	case PhysicalType::INT128:
		SetFullRange<hugeint_t>(type, stats);
		break;
	case PhysicalType::UINT8:
		SetFullRange<uint8_t>(type, stats);
		break;
	case PhysicalType::UINT16:
		SetFullRange<uint16_t>(type, stats);
		break;
	case PhysicalType::UINT32:
		SetFullRange<uint32_t>(type, stats);
		break;
	case PhysicalType::UINT64:
		SetFullRange<uint64_t>(type, stats);
		break;
	case PhysicalType::UINT128:
		SetFullRange<uhugeint_t>(type, stats);
		break;
	case PhysicalType::FLOAT:
		SetFullRange<float>(type, stats);
		break;
	case PhysicalType::DOUBLE:
		SetFullRange<double>(type, stats);
		break;
	default:
		return false;
	}
	return true;
}

unique_ptr<BaseStatistics> PropagateModuloStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 2);
	auto &lstats = child_stats[0];
	auto &rstats = child_stats[1];
	if (!NumericStats::HasMinMax(rstats)) {
		return nullptr;
	}
	auto &return_type = expr.GetReturnType();
	// the divisor alone bounds the result: a dividend without statistics spans its whole type
	BaseStatistics full_range = NumericStats::CreateUnknown(return_type);
	if (!NumericStats::HasMinMax(lstats)) {
		if (return_type.id() == LogicalTypeId::DECIMAL || !ModuloFullRange(return_type, full_range)) {
			return nullptr;
		}
	}
	auto &dividend_stats = NumericStats::HasMinMax(lstats) ? lstats : full_range;
	Value new_min, new_max;
	bool zero_divisor = false;
	bool has_bounds;
	switch (return_type.InternalType()) {
	case PhysicalType::INT8:
		has_bounds = SignedModuloBounds<int8_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::INT16:
		has_bounds = SignedModuloBounds<int16_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::INT32:
		has_bounds = SignedModuloBounds<int32_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::INT64:
		has_bounds = SignedModuloBounds<int64_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::INT128:
		has_bounds = SignedModuloBounds<hugeint_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::UINT8:
		has_bounds = UnsignedModuloBounds<uint8_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::UINT16:
		has_bounds =
		    UnsignedModuloBounds<uint16_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::UINT32:
		has_bounds =
		    UnsignedModuloBounds<uint32_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::UINT64:
		has_bounds =
		    UnsignedModuloBounds<uint64_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::UINT128:
		has_bounds =
		    UnsignedModuloBounds<uhugeint_t>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::FLOAT:
		has_bounds = FloatingModuloBounds<float>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	case PhysicalType::DOUBLE:
		has_bounds = FloatingModuloBounds<double>(return_type, dividend_stats, rstats, new_min, new_max, zero_divisor);
		break;
	default:
		return nullptr;
	}
	if (!has_bounds) {
		return nullptr;
	}
	if (zero_divisor) {
		// x % 0 is NaN under IEEE semantics and raises by default: neither must be hidden by derived bounds
		if (return_type.IsFloating() && Settings::Get<IeeeFloatingPointOpsSetting>(context)) {
			return nullptr;
		}
		if (Settings::Get<ErrorOnDivisionByZeroSetting>(context)) {
			return nullptr;
		}
	}
	auto result = NumericStats::CreateEmpty(return_type);
	NumericStats::SetMin(result, new_min);
	NumericStats::SetMax(result, new_max);
	result.CombineValidity(lstats, rstats);
	if (zero_divisor) {
		// x % 0 is NULL
		result.SetHasNull();
	}
	return result.ToUnique();
}

} // namespace

ScalarFunctionSet OperatorModuloFun::GetFunctions() {
	ScalarFunctionSet modulo("%");
	for (auto &type : LogicalType::Numeric()) {
		if (type.id() == LogicalTypeId::FLOAT || type.id() == LogicalTypeId::DOUBLE) {
			modulo.AddFunction(ScalarFunction({type, type}, type, nullptr, BindBinaryFloatingPoint<ModuloOperator>,
			                                  PropagateModuloStats));
		} else if (type.id() == LogicalTypeId::DECIMAL) {
			ScalarFunction function({type, type}, type, nullptr, BindDecimalArithmetic<true>, PropagateModuloStats);
			function.SetResolveTypesCallback(DecimalModuloResolveTypes<ModuloOperator>);
			modulo.AddFunction(std::move(function));
		} else {
			modulo.AddFunction(
			    ScalarFunction({type, type}, type, nullptr, BindDivisionByZero<ModuloOperator>, PropagateModuloStats));
		}
	}
	// we can take the remainder of bignums divided by bignums
	modulo.AddFunction(ScalarFunction({LogicalType::BIGNUM, LogicalType::BIGNUM}, LogicalType::BIGNUM, nullptr,
	                                  BindBignumDivide<true>));
	modulo.SetFallible();

	modulo.ApplyToFunctions(NameOperatorParameters);
	return modulo;
}

//===--------------------------------------------------------------------===//
// fmod [floored modulo]
//===--------------------------------------------------------------------===//
struct FlooredModuloOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		// the remainder is exact, but takes the sign of the dividend
		auto result = ModuloOperator::Operation<TA, TB, TR>(left, right);
		// a zero remainder is returned as a positive zero
		if (result == TR(0)) {
			return TR(0);
		}
		if ((result < TR(0)) != (right < TB(0))) {
			result += right;
		}
		return result;
	}
};

ScalarFunctionSet FmodFun::GetFunctions() {
	ScalarFunctionSet fmod;
	for (auto &type : {LogicalType::FLOAT, LogicalType::DOUBLE}) {
		ScalarFunction function({}, type, nullptr, BindBinaryFloatingPoint<FlooredModuloOperator>);
		function.GetSignature().AddParameter("x", type).AddParameter("y", type);
		fmod.AddFunction(std::move(function));
	}
	fmod.SetFallible();
	return fmod;
}

//===--------------------------------------------------------------------===//
// Linear Interpolation
//===--------------------------------------------------------------------===//

template <>
double InterpolateOperator::Operation(const double &lo, const double d, const double &hi) {
	return lo * (1.0 - d) + hi * d;
}

//! Interpolates between two integers without going through a double for the values themselves
static int64_t InterpolateInt64(const int64_t lo, const double d, const int64_t hi) {
	int64_t delta;
	if (!TrySubtractOperator::Operation(hi, lo, delta)) {
		return std::llround(static_cast<double>(lo) * (1.0 - d) + static_cast<double>(hi) * d);
	}
	return lo + std::llround(static_cast<double>(delta) * d);
}

template <>
dtime_t InterpolateOperator::Operation(const dtime_t &lo, const double d, const dtime_t &hi) {
	return dtime_t(InterpolateInt64(lo.value, d, hi.value));
}

//! TIMETZ values are interpolated by their time normalized to UTC - the result is the normalized time at offset +00
template <>
dtime_tz_t InterpolateOperator::Operation(const dtime_tz_t &lo, const double d, const dtime_tz_t &hi) {
	if (lo.sort_key() == hi.sort_key()) {
		return lo;
	}
	// the sort key holds the normalized time (biased by the maximum offset) in its upper bits
	const auto lo_micros = static_cast<double>(dtime_tz_t::decode_micros(lo.sort_key()));
	const auto hi_micros = static_cast<double>(dtime_tz_t::decode_micros(hi.sort_key()));
	auto micros = std::llround(lo_micros * (1.0 - d) + hi_micros * d);
	micros -= int64_t(dtime_tz_t::MAX_OFFSET) * int64_t(dtime_tz_t::OFFSET_MICROS);
	micros %= Interval::MICROS_PER_DAY;
	if (micros < 0) {
		micros += Interval::MICROS_PER_DAY;
	}
	return dtime_tz_t(dtime_t(micros), 0);
}

template <>
timestamp_t InterpolateOperator::Operation(const timestamp_t &lo, const double d, const timestamp_t &hi) {
	return timestamp_t(InterpolateInt64(lo.value, d, hi.value));
}

template <>
hugeint_t InterpolateOperator::Operation(const hugeint_t &lo, const double d, const hugeint_t &hi) {
	hugeint_t delta_hugeint = hi;
	if (Hugeint::TrySubtractInPlace(delta_hugeint, lo)) {
		const auto delta = Hugeint::Cast<double>(delta_hugeint);
		return lo + Hugeint::Convert(delta * d);
	}

	// if delta overflows, we fall back to original subtraction to avoid UB
	// only happens when lo and hi are so apart that their difference can't be stored in a hugeint
	return Hugeint::Convert(Operation(Hugeint::Cast<double>(lo), d, Hugeint::Cast<double>(hi)));
}

template <>
uhugeint_t InterpolateOperator::Operation(const uhugeint_t &lo, const double d, const uhugeint_t &hi) {
	return Uhugeint::Convert(Operation(Uhugeint::Cast<double>(lo), d, Uhugeint::Cast<double>(hi)));
}

static interval_t MultiplyByDouble(const interval_t &i, const double &d) { // NOLINT
	D_ASSERT(d >= 0 && d <= 1);
	return Interval::FromMicro(std::llround(static_cast<double>(Interval::GetMicro(i)) * d));
}

inline interval_t operator+(const interval_t &lhs, const interval_t &rhs) {
	return Interval::FromMicro(Interval::GetMicro(lhs) + Interval::GetMicro(rhs));
}

inline interval_t operator-(const interval_t &lhs, const interval_t &rhs) {
	return Interval::FromMicro(Interval::GetMicro(lhs) - Interval::GetMicro(rhs));
}

template <>
interval_t InterpolateOperator::Operation(const interval_t &lo, const double d, const interval_t &hi) {
	const interval_t delta = hi - lo;
	return lo + MultiplyByDouble(delta, d);
}

} // namespace duckdb
