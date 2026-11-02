#include "duckdb/common/exception.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// State: running scaled-integer sum and a non-NULL row count.
// count == 0 is the empty-group sentinel (no separate boolean needed).
//===--------------------------------------------------------------------===//

struct DecimalAvgState {
	static constexpr const char *STATE_NAMES[] = {"sum", "count"};
	using STATE_TYPE = StructStateType<hugeint_t, uint64_t>;

	hugeint_t sum;  // accumulated integer value at the input's scale
	uint64_t count; // number of non-NULL rows accumulated so far
};

//===--------------------------------------------------------------------===//
// Operation
//===--------------------------------------------------------------------===//

template <class INPUT_TYPE>
struct DecimalAvgOperation {
	static bool IgnoreNull() {
		return true;
	}

	static void Initialize(DecimalAvgState &state) {
		state.sum = hugeint_t(0);
		state.count = 0;
	}

	template <class INPUT, class STATE, class OP>
	static void Operation(STATE &state, const INPUT &input, AggregateUnaryInput &) {
		if (!Hugeint::TryAddInPlace(state.sum, hugeint_t(input))) {
			OutOfRangeException::Throw("decimal_average: intermediate sum overflowed HUGEINT range");
		}
		state.count++;
	}

	template <class INPUT, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT &input, AggregateUnaryInput &unary_input, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!Hugeint::TryAddInPlace(target.sum, source.sum)) {
			OutOfRangeException::Throw("decimal_average: intermediate sum overflowed HUGEINT range during combine");
		}
		target.count += source.count;
	}

	// Overload for hugeint_t result: direct assignment.
	static void AssignResult(hugeint_t &target, hugeint_t signed_q) {
		target = signed_q;
	}

	// Overload for smaller integer result types: take lower 64 bits.
	// Safe because the average of values bounded by the type's range is also
	// within that range.
	template <class RESULT>
	static void AssignResult(RESULT &target, hugeint_t signed_q) {
		target = static_cast<RESULT>(signed_q.lower);
	}

	template <class RESULT, class STATE>
	static void Finalize(STATE &state, RESULT &target, AggregateFinalizeData &fd) {
		if (state.count == 0) {
			fd.ReturnNull();
			return;
		}

		bool negative = state.sum.upper < 0;
		uhugeint_t abs_sum(static_cast<uint64_t>(state.sum.upper), state.sum.lower);
		if (negative) {
			abs_sum = -abs_sum;
		}

		uhugeint_t remainder;
		uhugeint_t q = Uhugeint::DivMod(abs_sum, uhugeint_t(state.count), remainder);
		auto r64 = remainder.lower;

		// Banker's rounding (round-half-to-even).
		uint64_t dist = state.count - r64;
		bool round_up = (dist < r64) | ((dist == r64) & static_cast<bool>(q.lower & 1));
		if (round_up) {
			q = q + uhugeint_t(1);
		}

		auto signed_q = hugeint_t(q);
		if (negative) {
			signed_q = hugeint_t(-q);
		}
		AssignResult(target, signed_q);
	}
};

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

template <class INPUT_TYPE>
static AggregateFunction MakeDecimalAvgFunction(const LogicalType &input_type, const LogicalType &return_type) {
	auto function =
	    AggregateFunction::UnaryAggregate<DecimalAvgState, INPUT_TYPE, INPUT_TYPE, DecimalAvgOperation<INPUT_TYPE>>(
	        input_type, return_type);
	if (std::is_same<INPUT_TYPE, hugeint_t>::value) {
		// adding DECIMAL(38) values to the HUGEINT accumulator throws on overflow
		function.SetFallible();
	}
	return function;
}

static unique_ptr<FunctionData> BindDecimalAverage(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto &input_type = arguments[0]->GetReturnType();

	if (input_type.id() == LogicalTypeId::UNKNOWN) {
		// unresolved prepared parameter - defer binding until the type is known
		throw ParameterNotResolvedException();
	}
	if (input_type.id() != LogicalTypeId::DECIMAL) {
		throw InvalidInputException("decimal_average requires a DECIMAL argument, got %s — "
		                            "cast the input explicitly, e.g. col::DECIMAL(18,2)",
		                            input_type.ToString());
	}

	uint8_t width = DecimalType::GetWidth(input_type);
	uint8_t scale = DecimalType::GetScale(input_type);
	auto return_type = LogicalType::DECIMAL(width, scale);

	auto name = function.GetName();
	switch (input_type.InternalType()) {
	case PhysicalType::INT16:
		function.ReplaceImplementation(
		    MakeDecimalAvgFunction<int16_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_INT16, scale), return_type));
		break;
	case PhysicalType::INT32:
		function.ReplaceImplementation(
		    MakeDecimalAvgFunction<int32_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_INT32, scale), return_type));
		break;
	case PhysicalType::INT64:
		function.ReplaceImplementation(
		    MakeDecimalAvgFunction<int64_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_INT64, scale), return_type));
		break;
	case PhysicalType::INT128:
		function.ReplaceImplementation(
		    MakeDecimalAvgFunction<hugeint_t>(LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, scale), return_type));
		break;
	default:
		throw InternalException("decimal_average: unexpected physical type");
	}
	function.SetName(std::move(name));
	function.GetArguments()[0] = input_type;
	return nullptr;
}

AggregateFunctionSet DecimalAverageFun::GetFunctions() {
	AggregateFunctionSet set("decimal_average");
	AggregateFunction fun({}, LogicalType(LogicalTypeId::DECIMAL), nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, BindDecimalAverage);
	fun.GetSignature().AddParameter("arg", LogicalTypeId::DECIMAL);
	set.AddFunction(fun);
	return set;
}

} // namespace duckdb
