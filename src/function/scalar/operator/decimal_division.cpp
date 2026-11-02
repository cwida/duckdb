#include "duckdb/common/exception.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/operator_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//

struct DecimalDivBindData : public FunctionData {
	// scale_exp = s2 + result_scale - s1
	// >= 0: scale numerator up:   result_int = (a_int * 10^scale_exp) / b_int
	// <  0: scale divisor up:     result_int =  a_int / (b_int * 10^|scale_exp|)
	int32_t scale_exp;

	explicit DecimalDivBindData(int32_t scale_exp_p) : scale_exp(scale_exp_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DecimalDivBindData>(scale_exp);
	}

	bool Equals(const FunctionData &other) const override {
		return scale_exp == other.Cast<DecimalDivBindData>().scale_exp;
	}
};

//===--------------------------------------------------------------------===//
// Execute functions
//
// COMPUTE_TYPE controls intermediate arithmetic:
//   int64_t   -- when input_max_width + |scale_exp| < CACHED_POWERS_OF_TEN (product fits in int64_t)
//   hugeint_t -- otherwise
//===--------------------------------------------------------------------===//

template <class INPUT_TYPE, class RESULT_TYPE, class COMPUTE_TYPE>
static void DecimalDivExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<DecimalDivBindData>();

	bool scale_divisor = bind_data.scale_exp < 0;
	uint8_t abs_exp = UnsafeNumericCast<uint8_t>(AbsValue(bind_data.scale_exp));

	COMPUTE_TYPE scale_factor;
	if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
		scale_factor = NumericHelper::POWERS_OF_TEN[abs_exp];
	} else {
		scale_factor = Hugeint::POWERS_OF_TEN[abs_exp];
	}
	auto result_width = DecimalType::GetWidth(result.GetType());

	BinaryExecutor::Execute<INPUT_TYPE, INPUT_TYPE, RESULT_TYPE>(
	    args.data[0], args.data[1], result, args.size(), [&](INPUT_TYPE a, INPUT_TYPE b) -> RESULT_TYPE {
		    if (b == INPUT_TYPE(0)) {
			    throw InvalidInputException("decimal_division: division by zero");
		    }

		    COMPUTE_TYPE numerator, divisor;
		    if (scale_divisor) {
			    numerator = COMPUTE_TYPE(a);
			    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
				    divisor = COMPUTE_TYPE(b) * scale_factor;
			    } else {
				    if (!Hugeint::TryMultiply(hugeint_t(b), scale_factor, divisor)) {
					    throw OutOfRangeException("decimal_division: scaled divisor overflowed DECIMAL range");
				    }
			    }
		    } else {
			    numerator = COMPUTE_TYPE(a) * scale_factor;
			    divisor = COMPUTE_TYPE(b);
		    }

		    bool negative;
		    COMPUTE_TYPE abs_num, abs_div;
		    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
			    negative = (numerator < 0) != (divisor < 0);
			    abs_num = numerator < 0 ? -numerator : numerator;
			    abs_div = divisor < 0 ? -divisor : divisor;
		    } else {
			    negative = (numerator.upper < 0) != (divisor.upper < 0);
			    abs_num = numerator.upper < 0 ? -numerator : numerator;
			    abs_div = divisor.upper < 0 ? -divisor : divisor;
		    }

		    COMPUTE_TYPE q, remainder;
		    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
			    q = abs_num / abs_div;
			    remainder = abs_num % abs_div;
		    } else {
			    if (abs_div.upper == 0 && abs_div.lower <= uint64_t(NumericLimits<int64_t>::Maximum())) {
				    uint64_t r64;
				    q = Hugeint::DivModPositive(abs_num, abs_div.lower, r64);
				    remainder = hugeint_t(int64_t(r64));
			    } else {
				    q = Hugeint::DivMod(abs_num, abs_div, remainder);
			    }
		    }

		    auto distance_to_next = abs_div - remainder;
		    bool quotient_is_odd;
		    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
			    quotient_is_odd = q % 2 != 0;
		    } else {
			    quotient_is_odd = q.lower % 2 != 0;
		    }
		    if (remainder > distance_to_next || (remainder == distance_to_next && quotient_is_odd)) {
			    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
				    q++;
			    } else if (!Hugeint::TryAddInPlace(q, hugeint_t(1))) {
				    throw OutOfRangeException("decimal_division: result out of range for result type");
			    }
		    }

		    bool result_out_of_range;
		    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
			    result_out_of_range = q >= NumericHelper::POWERS_OF_TEN[result_width];
		    } else {
			    result_out_of_range = q >= Hugeint::POWERS_OF_TEN[result_width];
		    }
		    if (result_out_of_range) {
			    throw OutOfRangeException("decimal_division: result out of range for result type");
		    }

		    COMPUTE_TYPE final_val = negative ? -q : q;
		    RESULT_TYPE out;
		    if constexpr (std::is_same_v<COMPUTE_TYPE, int64_t>) {
			    if (!TryCast::Operation<int64_t, RESULT_TYPE>(final_val, out)) {
				    throw OutOfRangeException("decimal_division: result out of range for result type");
			    }
		    } else {
			    if (!Hugeint::TryCast(final_val, out)) {
				    throw OutOfRangeException("decimal_division: result out of range for result type");
			    }
		    }
		    return out;
	    });
}

template <class INPUT_TYPE, class COMPUTE_TYPE>
static scalar_function_t GetDecimalDivExecuteFunction(PhysicalType result_physical) {
	switch (result_physical) {
	case PhysicalType::INT16:
		return DecimalDivExecute<INPUT_TYPE, int16_t, COMPUTE_TYPE>;
	case PhysicalType::INT32:
		return DecimalDivExecute<INPUT_TYPE, int32_t, COMPUTE_TYPE>;
	case PhysicalType::INT64:
		return DecimalDivExecute<INPUT_TYPE, int64_t, COMPUTE_TYPE>;
	default:
		return DecimalDivExecute<INPUT_TYPE, hugeint_t, COMPUTE_TYPE>;
	}
}

//===--------------------------------------------------------------------===//
// Bind function
//===--------------------------------------------------------------------===//

// Result width and scale for e1 / e2 (SQL Server semantics):
//   result_scale = max(6, s1 + p2 + 1)
//   result_width = p1 - s1 + s2 + result_scale
// Both are capped at Decimal::MAX_WIDTH_DECIMAL (38).
static void DecimalDivisionResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &lhs_type = input.GetArgumentType(0);
	auto &rhs_type = input.GetArgumentType(1);

	uint8_t p1, s1, p2, s2;
	if (!lhs_type.GetDecimalProperties(p1, s1) || !rhs_type.GetDecimalProperties(p2, s2)) {
		throw InvalidInputException("decimal_division: both arguments must be DECIMAL");
	}

	uint8_t result_scale;
	if (input.GetArgumentCount() == 3) {
		auto scale_val = input.GetNonNullConstant(2);
		auto scale_i = scale_val.GetValue<int32_t>();
		if (scale_i < 0 || scale_i > Decimal::MAX_WIDTH_DECIMAL) {
			throw InvalidInputException("decimal_division: scale must be between 0 and %d, got %d",
			                            Decimal::MAX_WIDTH_DECIMAL, scale_i);
		}
		result_scale = UnsafeNumericCast<uint8_t>(scale_i);
	} else {
		result_scale = MaxValue<uint8_t>(6, s1 + p2 + 1);
	}
	// result_width uses signed arithmetic to catch underflow before casting.
	auto result_width_i = p1 - s1 + s2 + result_scale;
	if (result_width_i < 1) {
		throw InvalidInputException("decimal_division: scale %d produces a zero-width result for these input types",
		                            result_scale);
	}
	uint8_t result_width = UnsafeNumericCast<uint8_t>(result_width_i);

	if (result_scale > Decimal::MAX_WIDTH_DECIMAL) {
		throw OutOfRangeException(
		    "Needed scale %d to accurately represent the division result, but this is out of range of the "
		    "DECIMAL type. Max scale is %d; could not perform an accurate division. Either add a cast to DOUBLE, "
		    "or add an explicit cast to a decimal with a lower scale.",
		    result_scale, Decimal::MAX_WIDTH_DECIMAL);
	}
	if (result_width > Decimal::MAX_WIDTH_DECIMAL) {
		throw OutOfRangeException(
		    "Needed width %d to accurately represent the division result, but this is out of range of the "
		    "DECIMAL type. Max width is %d; could not perform an accurate division. Either add a cast to DOUBLE, "
		    "or add an explicit cast to a decimal with a lower scale.",
		    result_width, Decimal::MAX_WIDTH_DECIMAL);
	}

	PhysicalType result_physical;
	if (result_width <= Decimal::MAX_WIDTH_INT16) {
		result_physical = PhysicalType::INT16;
	} else if (result_width <= Decimal::MAX_WIDTH_INT32) {
		result_physical = PhysicalType::INT32;
	} else if (result_width <= Decimal::MAX_WIDTH_INT64) {
		result_physical = PhysicalType::INT64;
	} else {
		result_physical = PhysicalType::INT128;
	}

	bound_function.SetReturnType(LogicalType::DECIMAL(result_width, result_scale));

	auto lhs_physical = lhs_type.InternalType();
	auto rhs_physical = rhs_type.InternalType();
	auto wider = MaxValue<PhysicalType>(lhs_physical, rhs_physical);

	int32_t scale_exp = s2 + result_scale - s1;
	int32_t abs_scale_exp = scale_exp < 0 ? -scale_exp : scale_exp;

	uint8_t input_max_width;
	switch (wider) {
	case PhysicalType::INT16:
		input_max_width = Decimal::MAX_WIDTH_INT16;
		break;
	case PhysicalType::INT32:
		input_max_width = Decimal::MAX_WIDTH_INT32;
		break;
	case PhysicalType::INT64:
		input_max_width = Decimal::MAX_WIDTH_INT64;
		break;
	case PhysicalType::INT128:
		input_max_width = Decimal::MAX_WIDTH_DECIMAL;
		break;
	default:
		throw InternalException("decimal_division: unexpected physical type");
	}

	// For same-tier inputs, preserve the original argument type so bind is idempotent
	// during plan deserialization (bind is re-called with the stored argument types, and
	// changing p changes the result-width formula).
	// For cross-tier inputs, promote the narrower argument to the wider physical tier.
	bound_function.GetArguments()[0] = (lhs_physical != wider) ? LogicalType::DECIMAL(input_max_width, s1) : lhs_type;
	bound_function.GetArguments()[1] = (rhs_physical != wider) ? LogicalType::DECIMAL(input_max_width, s2) : rhs_type;

	switch (wider) {
	case PhysicalType::INT16:
		if (Decimal::MAX_WIDTH_INT16 + abs_scale_exp < NumericHelper::CACHED_POWERS_OF_TEN) {
			bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<int16_t, int64_t>(result_physical));
		} else {
			bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<int16_t, hugeint_t>(result_physical));
		}
		break;
	case PhysicalType::INT32:
		if (Decimal::MAX_WIDTH_INT32 + abs_scale_exp < NumericHelper::CACHED_POWERS_OF_TEN) {
			bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<int32_t, int64_t>(result_physical));
		} else {
			bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<int32_t, hugeint_t>(result_physical));
		}
		break;
	case PhysicalType::INT64:
		if (Decimal::MAX_WIDTH_INT64 + abs_scale_exp < NumericHelper::CACHED_POWERS_OF_TEN) {
			bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<int64_t, int64_t>(result_physical));
		} else {
			bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<int64_t, hugeint_t>(result_physical));
		}
		break;
	default:
		bound_function.SetFunctionCallback(GetDecimalDivExecuteFunction<hugeint_t, hugeint_t>(result_physical));
		break;
	}
}

static unique_ptr<FunctionData> DecimalDivisionBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto lhs_scale = DecimalType::GetScale(bound_function.GetArguments()[0]);
	auto rhs_scale = DecimalType::GetScale(bound_function.GetArguments()[1]);
	auto result_scale = DecimalType::GetScale(bound_function.GetReturnType());
	return make_uniq<DecimalDivBindData>(rhs_scale + result_scale - lhs_scale);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

ScalarFunctionSet DecimalDivisionFun::GetFunctions() {
	auto decimal_type = LogicalType(LogicalTypeId::DECIMAL);
	ScalarFunctionSet set("decimal_division");

	ScalarFunction two_arg({}, decimal_type, DecimalDivExecute<hugeint_t, hugeint_t, hugeint_t>, DecimalDivisionBind);
	two_arg.GetSignature().AddParameter("x", decimal_type).AddParameter("y", decimal_type);
	two_arg.SetResolveTypesCallback(DecimalDivisionResolveTypes);
	two_arg.SetFallible();
	set.AddFunction(two_arg);

	ScalarFunction three_arg({}, decimal_type, DecimalDivExecute<hugeint_t, hugeint_t, hugeint_t>, DecimalDivisionBind);
	three_arg.GetSignature()
	    .AddParameter("x", decimal_type)
	    .AddParameter("y", decimal_type)
	    .AddParameter("scale", LogicalType::INTEGER);
	three_arg.SetResolveTypesCallback(DecimalDivisionResolveTypes);
	three_arg.SetFallible();
	set.AddFunction(three_arg);

	return set;
}

} // namespace duckdb
