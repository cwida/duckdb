#include "core_functions/aggregate/algebraic_functions.hpp"
#include "core_functions/aggregate/sum_helpers.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/types/decimal_helper.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

#include <cmath>

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
		this->err += other.err;
	}
};

struct AverageDecimalBindData : public FunctionData {
	AverageDecimalBindData(double scale, uint32_t decimal_scale) : scale(scale), decimal_scale(decimal_scale) {
	}

	//! 10^decimal_scale, the divisor of the unscaled sum
	double scale;
	uint32_t decimal_scale;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AverageDecimalBindData>(scale, decimal_scale);
	};

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AverageDecimalBindData>();
		return scale == other.scale && decimal_scale == other.decimal_scale;
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
			target = KahanFinalize(state.value, state.err) / static_cast<double>(state.count);
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

//! The average of the bignum_t tier of decimals: an exact BIGNUM sum, divided once at the end like the hugeint tier
struct BignumAvgState {
	uint64_t count;
	BignumIntermediate value;
};

//! The sum as a long double: through a hugeint when it fits, so that the same values in a fixed-size and a wide
//! decimal average to the same result
static long double BignumSumToLongDouble(const BignumIntermediate &value) {
	const auto start = value.GetStartDataPos();
	const auto size = value.size - start;
	if (size <= sizeof(hugeint_t)) {
		uhugeint_t magnitude = 0;
		for (idx_t i = start; i < value.size; i++) {
			magnitude = (magnitude << 8) | uhugeint_t(value.GetAbsoluteByte(NumericCast<int64_t>(i)));
		}
		if (magnitude <= uhugeint_t(NumericLimits<hugeint_t>::Maximum())) {
			auto sum = hugeint_t(magnitude.upper, magnitude.lower);
			return Hugeint::Cast<long double>(value.is_negative ? -sum : sum);
		}
	}
	long double result = 0;
	for (idx_t i = start; i < value.size; i++) {
		result = result * 256 + value.GetAbsoluteByte(NumericCast<int64_t>(i));
	}
	return value.is_negative ? -result : result;
}

//! The digits a quotient carries beyond the 17 significant digits of a DOUBLE, so that the quotient and a sticky
//! digit for a nonzero remainder round to the DOUBLE nearest to the exact average
static constexpr idx_t AVERAGE_GUARD_DIGITS = 40;

//! The average as the DOUBLE nearest to the exact quotient: the sum is scaled up by enough digits for the quotient to
//! carry the guard digits beyond those of the count, a nonzero remainder (which lies strictly between two quotients)
//! is kept as a trailing nonzero digit, and the decimal text is parsed once, so the result is rounded once
static double BignumExactAverage(const BignumIntermediate &sum, uint64_t count, uint32_t scale) {
	StringHeap heap;
	BignumArithmeticState state;
	const idx_t extra_digits = AVERAGE_GUARD_DIGITS + std::to_string(count).size();
	const string power_blob = DecimalHelper<bignum_t>::PowerOfTen(extra_digits);
	const bignum_t power_value {string_t(power_blob)};
	const BignumIntermediate power(power_value);
	const bignum_t scaled_value {BignumIntermediate::Multiply(heap, state, sum, power)};
	const BignumIntermediate scaled_sum(scaled_value);
	const string count_blob = DecimalHelper<bignum_t>::FromInteger(uhugeint_t(count));
	const bignum_t count_value {string_t(count_blob)};
	const BignumIntermediate divisor(count_value);
	string_t quotient;
	string_t remainder;
	if (!BignumIntermediate::TryDivMod(heap, state, scaled_sum, divisor, quotient, remainder)) {
		throw InternalException("avg: division of the wide decimal sum by the count failed");
	}
	auto text = Bignum::BignumToVarchar(bignum_t(quotient));
	text += DecimalHelper<bignum_t>::IsZero(bignum_t(remainder)) ? "0" : "1";
	text += "e-" + std::to_string(idx_t(scale) + extra_digits + 1);
	double result;
	if (!TryCast::Operation(string_t(text), result, false)) {
		throw InternalException("avg: could not convert the wide decimal average '%s' to a DOUBLE", text);
	}
	return result;
}

struct BignumAverageOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.count = 0;
		state.value = BignumIntermediate();
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		if (state.count == 0) {
			state.value.Initialize(unary_input.input.allocator);
		}
		state.count++;
		BignumIntermediate rhs(input);
		state.value.AddInPlace(unary_input.input.allocator, rhs);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &input) {
		if (source.count == 0) {
			return;
		}
		if (target.count == 0) {
			target.value.Initialize(input.allocator);
		}
		target.count += source.count;
		target.value.AddInPlace(input.allocator, source.value);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.count == 0) {
			finalize_data.ReturnNull();
			return;
		}
		long double divident = GetAverageDivident<long double>(state.count, finalize_data.input.bind_data);
		const auto sum = BignumSumToLongDouble(state.value);
		if (std::isfinite(sum) && std::isfinite(divident)) {
			target = sum / divident;
			return;
		}
		// beyond the range of a long double (a sum or a scale factor near the DOUBLE range) the exact sum is divided
		// by the count instead, so that the average stays finite and a subnormal average keeps its value
		auto &bind_data = finalize_data.input.bind_data->Cast<AverageDecimalBindData>();
		target = BignumExactAverage(state.value, state.count, bind_data.decimal_scale);
	}

	static bool IgnoreNull() {
		return true;
	}
};

//! The states are exported as STRUCT(count UBIGINT, value BIGNUM), the value is NULL for an empty state
static LogicalType BignumAvgStateType() {
	return LogicalType::STRUCT({{"count", LogicalType::UBIGINT}, {"value", LogicalType::BIGNUM}});
}

AggregateStateLayout BignumAvgStateLayout(AggregateLayoutInput &input) {
	return AggregateStateLayout(BignumAvgStateType(), AlignValue<idx_t>(sizeof(BignumAvgState)));
}

void BignumAvgExportState(Vector &state_vector, AggregateFinalizeInputData &, Vector &result, idx_t count,
                          idx_t offset) {
	auto states = state_vector.Values<BignumAvgState *>();
	auto &entries = StructVector::GetEntries(result);
	auto count_data = FlatVector::GetDataMutable<uint64_t>(entries[0]);
	auto value_data = FlatVector::GetDataMutable<string_t>(entries[1]);
	auto &value_validity = FlatVector::ValidityMutable(entries[1]);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[i].GetValue();
		count_data[offset + i] = state.count;
		if (state.count == 0) {
			value_validity.SetInvalid(offset + i);
			continue;
		}
		value_data[offset + i] = BignumAggregates::WriteBignum(state.value, entries[1]);
	}
}

void BignumAvgImportState(AggregateImportInputData &input) {
	auto &input_vec = input.input_vec;
	auto count = input_vec.size();
	UnifiedVectorFormat struct_format;
	input_vec.ToUnifiedFormat(struct_format);
	auto &entries = StructVector::GetEntries(input_vec);
	UnifiedVectorFormat count_format;
	UnifiedVectorFormat value_format;
	entries[0].ToUnifiedFormat(count_format);
	entries[1].ToUnifiedFormat(value_format);
	auto count_data = count_format.GetData<uint64_t>(count_format);
	auto value_data = value_format.GetData<string_t>(value_format);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *reinterpret_cast<BignumAvgState *>(input.dest_buffer + i * input.layout.total_state_size);
		BignumAverageOperation::Initialize(state);
		const auto struct_idx = struct_format.sel->get_index(i);
		const auto count_idx = count_format.sel->get_index(i);
		const auto value_idx = value_format.sel->get_index(i);
		if (!struct_format.validity.RowIsValid(struct_idx) || !count_format.validity.RowIsValid(count_idx) ||
		    count_data[count_idx] == 0) {
			// an empty state, e.g. to_aggregate_state(NULL, ...)
			continue;
		}
		if (!value_format.validity.RowIsValid(value_idx)) {
			throw InvalidInputException("Invalid avg state - the value cannot be NULL when the count is not 0");
		}
		state.count = count_data[count_idx];
		state.value = BignumAggregates::ImportBignum(value_data[value_idx], input.allocator);
	}
}

AggregateFunction GetBignumAverageAggregate() {
	auto function = AggregateFunction::UnaryAggregate<BignumAvgState, bignum_t, double, BignumAverageOperation>(
	    LogicalTypeId::DECIMAL, LogicalType::DOUBLE);
	function.SetStateExportCallbacks(BignumAvgStateLayout, BignumAvgExportState, BignumAvgImportState);
	return function;
}

static AggregateFunction NameXParameter(AggregateFunction fun) {
	fun.GetSignature().GetParameter(0).SetName("x");
	return fun;
}

unique_ptr<BaseStatistics> AveragePropagateStats(ClientContext &context, BoundAggregateExpression &expr,
                                                 AggregateStatisticsInput &input) {
	auto bounds = SumAccumulatorBounds::TryGet(input);
	expr.FunctionMutable().SetErrorMode(bounds ? FunctionErrors::CANNOT_ERROR
	                                           : FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	return nullptr;
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
		function.SetStatisticsCallback(AveragePropagateStats);
		return NameXParameter(std::move(function));
	}
	case PhysicalType::INTERVAL: {
		auto function =
		    AggregateFunction::UnaryAggregate<IntervalAvgState, interval_t, interval_t, IntervalAverageOperation>(
		        LogicalType::INTERVAL, LogicalType::INTERVAL);
		function.SetFallible(); // interval addition throws on overflow of any field
		return NameXParameter(std::move(function));
	}
	case PhysicalType::DOUBLE: {
		return NameXParameter(
		    AggregateFunction::UnaryAggregate<AvgState<double>, double, double, NumericAverageOperation>(
		        LogicalType::DOUBLE, LogicalType::DOUBLE));
	}
	default:
		throw InternalException("Unimplemented average aggregate");
	}
}

//! 10^scale as a double, through the hugeint table where it reaches so that both decimal tiers divide identically
static double DecimalScaleFactor(uint32_t scale) {
	if (scale < Hugeint::CACHED_POWERS_OF_TEN) {
		return Hugeint::Cast<double>(Hugeint::POWERS_OF_TEN[scale]);
	}
	return std::pow(10.0, static_cast<double>(scale));
}

unique_ptr<FunctionData> BindDecimalAvg(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto decimal_type = BindDecimalArgument(input.GetClientContext(), arguments[0], "avg");
	if (decimal_type.InternalType() == PhysicalType::VARCHAR) {
		// the bignum_t tier of decimals sums its BIGNUM blobs
		function.ReplaceImplementation(GetBignumAverageAggregate());
	} else {
		function.ReplaceImplementation(GetAverageAggregate(decimal_type.InternalType()));
	}
	function.SetName("avg");
	function.GetArguments()[0] = decimal_type;
	function.SetReturnType(LogicalType::DOUBLE);
	const auto scale = DecimalType::GetScale(decimal_type);
	return make_uniq<AverageDecimalBindData>(DecimalScaleFactor(scale), scale);
}

AggregateFunction GetTimestampAverage() {
	auto timestamp_avg =
	    AggregateFunction::UnaryAggregate<AvgState<hugeint_t>, int64_t, int64_t, TimestampAverageOperation>(
	        LogicalType::TIMESTAMP, LogicalType::TIMESTAMP);
	timestamp_avg.GetSignature().GetParameter(0).SetName("x");
	timestamp_avg.SetStructStateExport(TimestampAverageStateLayout);
	return timestamp_avg;
}

//! Averages the argument as a TIMESTAMP, whose range holds every ordinary date of the lower precisions
unique_ptr<FunctionData> BindWidenedTimestampAvg(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	function.ReplaceImplementation(GetTimestampAverage());
	function.SetName("avg");
	function.GetArguments()[0] = LogicalType::TIMESTAMP;
	function.SetReturnType(LogicalType::TIMESTAMP);
	return nullptr;
}

//! Without an overload of their own, TIMESTAMP_S, TIMESTAMP_MS and DATE would widen to TIMESTAMP_NS, the cheapest
//! implicit cast, whose range is far smaller than theirs
AggregateFunction GetWidenedTimestampAverage(const LogicalType &type) {
	// stub function, the bind sets the TIMESTAMP implementation and the argument type it casts to
	AggregateFunction fun({}, LogicalType::TIMESTAMP, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, BindWidenedTimestampAvg);
	fun.GetSignature().AddParameter("x", type);
	return fun;
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
	avg.AddFunction(GetAverageAggregate(PhysicalType::DOUBLE));

	avg.AddFunction(GetTimestampAverage());
	for (auto &type : {LogicalType::TIMESTAMP_S, LogicalType::TIMESTAMP_MS, LogicalType::DATE}) {
		avg.AddFunction(GetWidenedTimestampAverage(type));
	}

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
