#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/sum_helpers.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

namespace {

template <class T>
struct AvgState {
	static constexpr const char *STATE_NAMES[] = {"count", "value"};
	using STATE_TYPE = StructStateType<uint64_t, T>;

	uint64_t count;
	T value;

	void Combine(const AvgState<T> &other) {
		this->count += other.count;
		CombineSumStateValue(this->value, other.value);
	}
};

struct IntervalAvgState {
	static constexpr const char *STATE_NAMES[] = {"count", "value"};
	using STATE_TYPE = StructStateType<int64_t, interval_t>;

	int64_t count;
	interval_t value;

	void Combine(const IntervalAvgState &other) {
		this->count += other.count;
		this->value = AddOperator::Operation<interval_t, interval_t, interval_t>(this->value, other.value);
	}
};

struct KahanAvgState {
	static constexpr const char *STATE_NAMES[] = {"count", "value", "err"};
	using STATE_TYPE = StructStateType<uint64_t, double, double>;

	uint64_t count;
	double value;
	double err;

	void Combine(const KahanAvgState &other) {
		this->count += other.count;
		KahanAddInternal(other.value, this->value, this->err);
		KahanAddInternal(-other.err, this->value, this->err);
	}
};

struct AverageDecimalBindData : public FunctionData {
	explicit AverageDecimalBindData(double scale) : scale(scale) {
	}

	double scale;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AverageDecimalBindData>(scale);
	};

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AverageDecimalBindData>();
		return scale == other.scale;
	}
};

struct AverageSetOperation {
	template <class STATE>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target.Combine(source);
	}
	template <class STATE>
	static void AddValues(STATE &state, idx_t count) {
		state.count += count;
	}
};

template <class T>
static T GetAverageDivident(uint64_t count, optional_ptr<FunctionData> bind_data) {
	T divident = T(count);
	if (bind_data) {
		auto &avg_bind_data = bind_data->Cast<AverageDecimalBindData>();
		divident *= avg_bind_data.scale;
	}
	return divident;
}

struct IntegerAverageOperation : public BaseSumOperation<AverageSetOperation, RegularAdd> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			double divident = GetAverageDivident<double>(state.count, finalize_data.input.bind_data);
			target = double(state.value) / divident;
		}
	}
};

struct IntegerAverageOperationHugeint : public BaseSumOperation<AverageSetOperation, AddToHugeint> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			long double divident = GetAverageDivident<long double>(state.count, finalize_data.input.bind_data);
			target = Hugeint::Cast<long double>(state.value) / divident;
		}
	}
};

struct DiscreteAverageOperation : public BaseSumOperation<AverageSetOperation, AddToHugeint> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			hugeint_t remainder;
			target = Hugeint::Cast<T>(Hugeint::DivMod(state.value, state.count, remainder));
			// Round the result
			target += (remainder > (state.count / 2));
		}
	}
};

//! The average of timestamps with an infinity is that infinity, or NULL with both, as for DOUBLE it is NaN. A state
//! that saw an infinity has the maximum count and the infinities as its value
struct TimestampAverageOperation : public DiscreteAverageOperation {
	static constexpr uint64_t INFINITE_COUNT = NumericLimits<uint64_t>::Maximum();
	static constexpr uint64_t POSITIVE_INFINITY = 1;
	static constexpr uint64_t NEGATIVE_INFINITY = 2;
	static constexpr uint64_t BOTH_INFINITIES = POSITIVE_INFINITY | NEGATIVE_INFINITY;

	template <class STATE>
	static void AddInfinities(STATE &state, uint64_t infinities) {
		if (state.count != INFINITE_COUNT) {
			state.count = INFINITE_COUNT;
			state.value = hugeint_t(0);
		}
		state.value.lower |= infinities;
	}

	static uint64_t GetInfinity(int64_t input) {
		return input > 0 ? POSITIVE_INFINITY : NEGATIVE_INFINITY;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.count == INFINITE_COUNT) {
			AddInfinities(target, source.value.lower);
		} else if (source.count != 0 && target.count != INFINITE_COUNT) {
			if (target.count == 0) {
				// an empty target holds no sum
				target.value = hugeint_t(0);
			}
			if (!TryAddOperator::Operation(target.count, source.count, target.count) ||
			    target.count == INFINITE_COUNT) {
				throw OutOfRangeException("Overflow in aggregate state combine");
			}
			CombineSumStateValue(target.value, source.value);
		}
	}

	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		if (source.count == INFINITE_COUNT) {
			if (count > 0) {
				AddInfinities(target, source.value.lower);
			}
		} else if (target.count != INFINITE_COUNT) {
			DiscreteAverageOperation::RepeatedCombine<STATE, OP>(source, target, input, count);
			if (target.count == INFINITE_COUNT) {
				throw OutOfRangeException("Overflow in repeated aggregate state combine");
			}
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		if (DUCKDB_UNLIKELY(!timestamp_t(input).IsFinite())) {
			AddInfinities(state, GetInfinity(input));
		} else if (state.count != INFINITE_COUNT) {
			DiscreteAverageOperation::Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		if (DUCKDB_UNLIKELY(!timestamp_t(input).IsFinite())) {
			AddInfinities(state, GetInfinity(input));
		} else if (state.count != INFINITE_COUNT) {
			DiscreteAverageOperation::ConstantOperation<INPUT_TYPE, STATE, OP>(state, input, unary_input, count);
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count != INFINITE_COUNT) {
			DiscreteAverageOperation::Finalize<T, STATE>(state, target, finalize_data);
		} else if (state.value == hugeint_t(POSITIVE_INFINITY)) {
			target = timestamp_t::infinity().value;
		} else if (state.value == hugeint_t(NEGATIVE_INFINITY)) {
			target = timestamp_t::ninfinity().value;
		} else {
			finalize_data.ReturnNull();
		}
	}
};

//! The average of timestamps in nanosecond resolution - mirrors TimestampAverageOperation, but
//! interprets the state value as a nanosecond timestamp so infinity is the ns infinity
template <class NS_TIMESTAMP>
struct TimestampNSAverageOperation : public DiscreteAverageOperation {
	static constexpr uint64_t INFINITE_COUNT = NumericLimits<uint64_t>::Maximum();
	static constexpr uint64_t POSITIVE_INFINITY = 1;
	static constexpr uint64_t NEGATIVE_INFINITY = 2;
	static constexpr uint64_t BOTH_INFINITIES = POSITIVE_INFINITY | NEGATIVE_INFINITY;

	template <class STATE>
	static void AddInfinities(STATE &state, uint64_t infinities) {
		if (state.count != INFINITE_COUNT) {
			state.count = INFINITE_COUNT;
			state.value = hugeint_t(0);
		}
		state.value.lower |= infinities;
	}

	static uint64_t GetInfinity(int64_t input) {
		return input > 0 ? POSITIVE_INFINITY : NEGATIVE_INFINITY;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.count == INFINITE_COUNT) {
			AddInfinities(target, source.value.lower);
		} else if (source.count != 0 && target.count != INFINITE_COUNT) {
			if (target.count == 0) {
				// an empty target holds no sum
				target.value = hugeint_t(0);
			}
			if (!TryAddOperator::Operation(target.count, source.count, target.count) ||
			    target.count == INFINITE_COUNT) {
				throw OutOfRangeException("Overflow in aggregate state combine");
			}
			CombineSumStateValue(target.value, source.value);
		}
	}

	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		if (source.count == INFINITE_COUNT) {
			if (count > 0) {
				AddInfinities(target, source.value.lower);
			}
		} else if (target.count != INFINITE_COUNT) {
			DiscreteAverageOperation::RepeatedCombine<STATE, OP>(source, target, input, count);
			if (target.count == INFINITE_COUNT) {
				throw OutOfRangeException("Overflow in repeated aggregate state combine");
			}
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		if (DUCKDB_UNLIKELY(!NS_TIMESTAMP(input).IsFinite())) {
			AddInfinities(state, GetInfinity(input));
		} else if (state.count != INFINITE_COUNT) {
			DiscreteAverageOperation::Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		if (DUCKDB_UNLIKELY(!NS_TIMESTAMP(input).IsFinite())) {
			AddInfinities(state, GetInfinity(input));
		} else if (state.count != INFINITE_COUNT) {
			DiscreteAverageOperation::ConstantOperation<INPUT_TYPE, STATE, OP>(state, input, unary_input, count);
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count != INFINITE_COUNT) {
			DiscreteAverageOperation::Finalize<T, STATE>(state, target, finalize_data);
		} else if (state.value == hugeint_t(POSITIVE_INFINITY)) {
			target = NS_TIMESTAMP::infinity().value;
		} else if (state.value == hugeint_t(NEGATIVE_INFINITY)) {
			target = NS_TIMESTAMP::ninfinity().value;
		} else {
			finalize_data.ReturnNull();
		}
	}
};

//! A state with the maximum count holds only infinities, reject imported ones that hold other values
void ValidateTimestampAverageStates(const_data_ptr_t states, idx_t count, idx_t stride) {
	for (idx_t i = 0; i < count; i++) {
		const auto state = Load<AvgState<hugeint_t>>(states + i * stride);
		if (state.count == TimestampAverageOperation::INFINITE_COUNT &&
		    (state.value < hugeint_t(TimestampAverageOperation::POSITIVE_INFINITY) ||
		     state.value > hugeint_t(TimestampAverageOperation::BOTH_INFINITIES))) {
			throw InvalidInputException(
			    "Invalid avg state - with a count of %llu the value must be 1 (infinity), 2 (-infinity) or 3 (both), "
			    "not %s",
			    state.count, state.value.ToString());
		}
	}
}

AggregateStateLayout TimestampAverageStateLayout(AggregateLayoutInput &input) {
	auto layout = AggregateFunction::StructStateLayout<AvgState<hugeint_t>>(input);
	layout.validate_state = ValidateTimestampAverageStates;
	return layout;
}

//! The average of TIME values is within a day - reject imported states that average outside of it
void ValidateTimeAverageStates(const_data_ptr_t states, idx_t count, idx_t stride) {
	for (idx_t i = 0; i < count; i++) {
		const auto state = Load<AvgState<hugeint_t>>(states + i * stride);
		if (state.count == 0) {
			continue;
		}
		const auto max_value = hugeint_t(0, state.count) * hugeint_t(Interval::MICROS_PER_DAY);
		if (state.value < 0 || state.value > max_value) {
			throw InvalidInputException("Invalid avg state - the average of %s over %llu values is not a valid time",
			                            state.value.ToString(), state.count);
		}
	}
}

AggregateStateLayout TimeAverageStateLayout(AggregateLayoutInput &input) {
	auto layout = AggregateFunction::StructStateLayout<AvgState<hugeint_t>>(input);
	layout.validate_state = ValidateTimeAverageStates;
	return layout;
}

//! The average of TIME_NS values is within a day - reject imported states that average outside of it
void ValidateTimeNSAverageStates(const_data_ptr_t states, idx_t count, idx_t stride) {
	for (idx_t i = 0; i < count; i++) {
		const auto state = Load<AvgState<hugeint_t>>(states + i * stride);
		if (state.count == 0) {
			continue;
		}
		const auto max_value = hugeint_t(0, state.count) * hugeint_t(Interval::NANOS_PER_DAY);
		if (state.value < 0 || state.value > max_value) {
			throw InvalidInputException("Invalid avg state - the average of %s over %llu values is not a valid time",
			                            state.value.ToString(), state.count);
		}
	}
}

AggregateStateLayout TimeNSAverageStateLayout(AggregateLayoutInput &input) {
	auto layout = AggregateFunction::StructStateLayout<AvgState<hugeint_t>>(input);
	layout.validate_state = ValidateTimeNSAverageStates;
	return layout;
}

struct HugeintAverageOperation : public BaseSumOperation<AverageSetOperation, HugeintAdd> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			long double divident = GetAverageDivident<long double>(state.count, finalize_data.input.bind_data);
			target = Hugeint::Cast<long double>(state.value) / divident;
		}
	}
};

struct NumericAverageOperation : public BaseSumOperation<AverageSetOperation, RegularAdd> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = state.value / state.count;
		}
	}
};

struct KahanAverageOperation : public BaseSumOperation<AverageSetOperation, KahanAdd> {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			target = (state.value / state.count) - (state.err / state.count);
		}
	}
};

struct IntervalAverageOperation : public BaseSumOperation<AverageSetOperation, IntervalAdd> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			// DivideOperator does not borrow fractions right,
			// TODO: Maybe it should?
			// Copy PG implementation.
			const auto &value = state.value;
			const auto count = UnsafeNumericCast<int64_t>(state.count);

			target.months = value.months / count;
			auto months_remainder = value.months % count;

			target.days = value.days / count;
			auto days_remainder = value.days % count;

			target.micros = value.micros / count;
			auto micros_remainder = value.micros % count;

			//	Shift the remainders right
			months_remainder *= Interval::DAYS_PER_MONTH;
			target.days += months_remainder / count;
			days_remainder += months_remainder % count;

			// the remainder in micros can exceed int64 for large counts, but divided by the count it is below two days
			const auto shifted_days = hugeint_t(days_remainder) * hugeint_t(Interval::MICROS_PER_DAY);
			micros_remainder += Hugeint::Cast<int64_t>(shifted_days / hugeint_t(count));
			target.micros += micros_remainder;
		}
	}
};

struct TimeTZAverageOperation : public BaseSumOperation<AverageSetOperation, AddToHugeint> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedAverageState::Combine(source, target, input, count);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &aggr_unary) {
		const auto micros = Time::NormalizeTimeTZ(input).value;
		AverageSetOperation::template AddValues<STATE>(state, 1);
		AddToHugeint::template AddNumber<STATE, int64_t>(state, micros);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &aggr_unary, idx_t count) {
		const auto micros = Time::NormalizeTimeTZ(input).value;
		AverageSetOperation::template AddValues<STATE>(state, count);
		AddToHugeint::template AddConstant<STATE, int64_t>(state, micros, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
		} else {
			uint64_t remainder;
			auto micros = Hugeint::Cast<int64_t>(Hugeint::DivModPositive(state.value, state.count, remainder));
			// Round the result
			micros += (remainder > (state.count / 2));
			target = dtime_tz_t(dtime_t(micros), 0);
		}
	}
};

static AggregateFunction NameXParameter(AggregateFunction fun) {
	fun.GetSignature().GetParameter(0).SetName("x");
	return fun;
}

AggregateFunction GetAverageAggregate(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT16: {
		return NameXParameter(
		    AggregateFunction::UnaryAggregate<AvgState<int64_t>, int16_t, double, IntegerAverageOperation>(
		        LogicalType::SMALLINT, LogicalType::DOUBLE));
	}
	case PhysicalType::INT32: {
		return NameXParameter(
		    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int32_t, double, IntegerAverageOperationHugeint>(
		        LogicalType::INTEGER, LogicalType::DOUBLE));
	}
	case PhysicalType::INT64: {
		return NameXParameter(
		    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, double, IntegerAverageOperationHugeint>(
		        LogicalType::BIGINT, LogicalType::DOUBLE));
	}
	case PhysicalType::INT128: {
		auto function =
		    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, hugeint_t, double, HugeintAverageOperation>(
		        LogicalType::HUGEINT, LogicalType::DOUBLE);
		function.SetFallible(); // adding HUGEINT values to the HUGEINT accumulator throws on overflow
		return NameXParameter(std::move(function));
	}
	case PhysicalType::INTERVAL: {
		auto function =
		    AggregateFunction::UnaryAggregate<IntervalAvgState, interval_t, interval_t, IntervalAverageOperation>(
		        LogicalType::INTERVAL, LogicalType::INTERVAL);
		function.SetFallible(); // interval addition throws on overflow of any field
		return NameXParameter(std::move(function));
	}
	default:
		throw InternalException("Unimplemented average aggregate");
	}
}

unique_ptr<FunctionData> BindDecimalAvg(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto decimal_type = BindDecimalArgument(input.GetClientContext(), arguments[0], "avg");
	function.ReplaceImplementation(GetAverageAggregate(decimal_type.InternalType()));
	function.SetName("avg");
	function.GetArguments()[0] = decimal_type;
	function.SetReturnType(LogicalType::DOUBLE);
	return make_uniq<AverageDecimalBindData>(
	    Hugeint::Cast<double>(Hugeint::POWERS_OF_TEN[DecimalType::GetScale(decimal_type)]));
}

} // namespace

AggregateFunctionSet AvgFun::GetFunctions() {
	AggregateFunctionSet avg;

	// The first is already opted-in during `BindDecimalAvg`
	AggregateFunction decimal_avg({}, LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr, nullptr,
	                              FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, BindDecimalAvg);
	decimal_avg.GetSignature().AddParameter("x", LogicalTypeId::DECIMAL);
	avg.AddFunction(decimal_avg);
	avg.AddFunction(GetAverageAggregate(PhysicalType::INT16));
	avg.AddFunction(GetAverageAggregate(PhysicalType::INT32));
	avg.AddFunction(GetAverageAggregate(PhysicalType::INT64));
	avg.AddFunction(GetAverageAggregate(PhysicalType::INT128));
	avg.AddFunction(GetAverageAggregate(PhysicalType::INTERVAL));
	auto numeric_avg = AggregateFunction::UnaryAggregate<AvgState<double>, double, double, NumericAverageOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	numeric_avg.GetSignature().GetParameter(0).SetName("x");
	avg.AddFunction(numeric_avg);

	auto timestamp_avg =
	    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t, TimestampAverageOperation>(
	        LogicalType::TIMESTAMP, LogicalType::TIMESTAMP);
	timestamp_avg.GetSignature().GetParameter(0).SetName("x");
	timestamp_avg.SetStructStateExport(TimestampAverageStateLayout);
	avg.AddFunction(timestamp_avg);

	auto timestamp_tz_avg =
	    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t, TimestampAverageOperation>(
	        LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ);
	timestamp_tz_avg.GetSignature().GetParameter(0).SetName("x");
	timestamp_tz_avg.SetStructStateExport(TimestampAverageStateLayout);
	avg.AddFunction(timestamp_tz_avg);

	auto time_avg = AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t, DiscreteAverageOperation>(
	    LogicalType::TIME, LogicalType::TIME);
	time_avg.GetSignature().GetParameter(0).SetName("x");
	time_avg.SetStructStateExport(TimeAverageStateLayout);
	avg.AddFunction(time_avg);

	auto time_tz_avg =
	    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, dtime_tz_t, dtime_tz_t, TimeTZAverageOperation>(
	        LogicalType::TIME_TZ, LogicalType::TIME_TZ);
	time_tz_avg.GetSignature().GetParameter(0).SetName("x");
	time_tz_avg.SetStructStateExport(TimeAverageStateLayout);
	avg.AddFunction(time_tz_avg);

	auto timestamp_ns_avg = AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t,
	                                                          TimestampNSAverageOperation<timestamp_ns_t>>(
	    LogicalType::TIMESTAMP_NS, LogicalType::TIMESTAMP_NS);
	timestamp_ns_avg.GetSignature().GetParameter(0).SetName("x");
	timestamp_ns_avg.SetStructStateExport(TimestampAverageStateLayout);
	avg.AddFunction(timestamp_ns_avg);

	auto timestamp_tz_ns_avg = AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t,
	                                                             TimestampNSAverageOperation<timestamp_tz_ns_t>>(
	    LogicalType::TIMESTAMP_TZ_NS, LogicalType::TIMESTAMP_TZ_NS);
	timestamp_tz_ns_avg.GetSignature().GetParameter(0).SetName("x");
	timestamp_tz_ns_avg.SetStructStateExport(TimestampAverageStateLayout);
	avg.AddFunction(timestamp_tz_ns_avg);

	auto time_ns_avg =
	    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t, DiscreteAverageOperation>(
	        LogicalType::TIME_NS, LogicalType::TIME_NS);
	time_ns_avg.GetSignature().GetParameter(0).SetName("x");
	time_ns_avg.SetStructStateExport(TimeNSAverageStateLayout);
	avg.AddFunction(time_ns_avg);

	return avg;
}

AggregateFunction FAvgFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<KahanAvgState, double, double, KahanAverageOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("x");
	return fun;
}

} // namespace duckdb
