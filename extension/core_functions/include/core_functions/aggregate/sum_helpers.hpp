//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/core_functions/aggregate/sum_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/function/aggregate_state.hpp"
#include "duckdb/function/aggregate_state_layout.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include <type_traits>
#include <utility>

namespace duckdb {

struct RepeatedStateValue {
	template <class T>
	static T Multiply(const T &input, idx_t count) {
		if constexpr (std::is_same<T, double>()) {
			return input * static_cast<double>(count);
		} else if constexpr (std::is_same<T, float>()) {
			return input * static_cast<float>(count);
		} else if constexpr (std::is_same<T, hugeint_t>()) {
			return Hugeint::Multiply(input, Hugeint::Convert(count));
		} else if constexpr (std::is_same<T, interval_t>()) {
			const auto count64 = Cast::Operation<idx_t, int64_t>(count);
			return MultiplyOperator::Operation<interval_t, int64_t, interval_t>(input, count64);
		} else {
			T result;
			const auto count_value = Cast::Operation<idx_t, T>(count);
			if (!TryMultiplyOperator::Operation(input, count_value, result)) {
				throw OutOfRangeException("Overflow in repeated aggregate state combine");
			}
			return result;
		}
	}

	template <class T>
	static void Add(T &target, const T &input) {
		if constexpr (std::is_same<T, double>() || std::is_same<T, float>()) {
			target += input;
		} else if constexpr (std::is_same<T, hugeint_t>()) {
			if (!Hugeint::TryAddInPlace(target, input)) {
				throw OutOfRangeException("Overflow in repeated aggregate state combine");
			}
		} else if constexpr (std::is_same<T, interval_t>()) {
			target = AddOperator::Operation<interval_t, interval_t, interval_t>(target, input);
		} else {
			if (!TryAddOperator::Operation(target, input, target)) {
				throw OutOfRangeException("Overflow in repeated aggregate state combine");
			}
		}
	}

	template <class STATE>
	static void AddRepeated(STATE &state, const decltype(std::declval<STATE>().value) &input, idx_t count) {
		Add(state.value, Multiply(input, count));
	}
};

static inline void KahanAddInternal(double input, double &summed, double &err) {
	double diff = input - err;
	double newval = summed + diff;
	err = (newval - summed) - diff;
	summed = newval;
}

template <class T>
static inline void CombineSumStateValue(T &target, const T &source) {
	if constexpr (std::is_same<T, hugeint_t>::value) {
		target = Hugeint::Add(target, source);
	} else {
		target += source;
	}
}

template <class T>
struct SumState {
	using value_type = T;
	using STATE_TYPE = OptionalStateType<T>;
	T value;
	bool is_set;
};

struct KahanSumState {
	static constexpr const char *STATE_NAMES[] = {"value", "err"};
	using STATE_TYPE = OptionalStateType<StructStateType<double, double>>;

	double value;
	double err;
	bool is_set;

	void Combine(const KahanSumState &other) {
		this->is_set = other.is_set || this->is_set;
		KahanAddInternal(other.value, this->value, this->err);
		KahanAddInternal(-other.err, this->value, this->err);
	}
};

struct RegularAdd {
	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		state.value += input;
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		RepeatedStateValue::AddRepeated(state, input, count);
	}
};

struct HugeintAdd {
	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		state.value = Hugeint::Add(state.value, input);
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		RepeatedStateValue::AddRepeated(state, input, count);
	}
};

struct IntervalAdd {
	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		state.value = AddOperator::Operation<interval_t, interval_t, interval_t>(state.value, input);
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		RepeatedStateValue::AddRepeated(state, input, count);
	}
};

struct KahanAdd {
	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		KahanAddInternal(input, state.value, state.err);
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		KahanAddInternal(input * count, state.value, state.err);
	}
};

struct AddToHugeint {
	static void AddValue(hugeint_t &result, uint64_t value, int positive) {
		// integer summation taken from Tim Gubner et al. - Efficient Query Processing
		// with Optimistically Compressed Hash Tables & Strings in the USSR

		// add the value to the lower part of the hugeint
		result.lower += value;
		// now handle overflows
		int overflow = result.lower < value;
		// we consider two situations:
		// (1) input[idx] is positive, and current value is lower than value: overflow
		// (2) input[idx] is negative, and current value is higher than value: underflow
		if (!(overflow ^ positive)) {
			// in the case of an overflow or underflow we either increment or decrement the upper base
			// positive: +1, negative: -1
			result.upper += -1 + 2 * positive;
		}
	}

	template <class STATE, class T>
	static void AddNumber(STATE &state, T input) {
		AddValue(state.value, uint64_t(input), input >= 0);
	}

	template <class STATE, class T>
	static void AddConstant(STATE &state, T input, idx_t count) {
		if constexpr (std::is_same<T, hugeint_t>()) {
			RepeatedStateValue::AddRepeated(state, input, count);
			return;
		}
		// add a constant X number of times
		// fast path: check if value * count fits into a uint64_t
		if (input >= 0 && (count == 0 || uint64_t(input) <= (NumericLimits<uint64_t>::Maximum() / count))) {
			// if it does just multiply it and add the value
			uint64_t value = uint64_t(input) * count;
			AddValue(state.value, value, 1);
		} else {
			// if it doesn't fit we have two choices
			// either we loop over count and add the values individually
			// or we convert to a hugeint and multiply the hugeint
			// the problem is that hugeint multiplication is expensive
			// hence we switch here: with a low count we do the loop
			// with a high count we do the hugeint multiplication
			if (count < 8) {
				for (idx_t i = 0; i < count; i++) {
					AddValue(state.value, uint64_t(input), input >= 0);
				}
			} else {
				hugeint_t addition = hugeint_t(input) * Hugeint::Convert(count);
				state.value += addition;
			}
		}
	}
};

struct RepeatedSumState {
	template <class STATE>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &, idx_t count) {
		if (!source.is_set) {
			return;
		}
		target.is_set = true;
		RepeatedStateValue::AddRepeated(target, source.value, count);
	}
};

struct RepeatedAverageState {
	template <class COUNT_TYPE>
	static COUNT_TYPE MultiplyCount(COUNT_TYPE source_count, idx_t count) {
		COUNT_TYPE repeated_count;
		const auto count_value = Cast::Operation<idx_t, COUNT_TYPE>(count);
		if (!TryMultiplyOperator::Operation(source_count, count_value, repeated_count)) {
			throw OutOfRangeException("Overflow in repeated aggregate state combine");
		}
		return repeated_count;
	}

	template <class COUNT_TYPE>
	static void AddCount(COUNT_TYPE &target_count, COUNT_TYPE repeated_count) {
		if (!TryAddOperator::Operation(target_count, repeated_count, target_count)) {
			throw OutOfRangeException("Overflow in repeated aggregate state combine");
		}
	}

	template <class STATE>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &, idx_t count) {
		if (source.count == 0) {
			return;
		}
		AddCount(target.count, MultiplyCount(source.count, count));
		RepeatedStateValue::AddRepeated(target, source.value, count);
	}
};

template <class STATEOP, class ADDOP>
struct BaseSumOperation {
	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		STATEOP::template Combine<STATE>(source, target, aggr_input_data);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		STATEOP::template AddValues<STATE>(state, 1);
		ADDOP::template AddNumber<STATE, INPUT_TYPE>(state, input);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t count) {
		STATEOP::template AddValues<STATE>(state, count);
		ADDOP::template AddConstant<STATE, INPUT_TYPE>(state, input, count);
	}
	static bool IgnoreNull() {
		return true;
	}
};

//! The DECIMAL overloads can be selected for non-DECIMAL arguments (e.g. through named arguments) - the bind runs
//! before the argument is cast, so cast it to the DECIMAL type that represents its values here
inline LogicalType BindDecimalArgument(ClientContext &context, unique_ptr<Expression> &argument,
                                       const string &function_name) {
	auto type = argument->GetReturnType();
	if (type.id() == LogicalTypeId::DECIMAL) {
		return type;
	}
	uint8_t width, scale;
	if (!type.GetDecimalProperties(width, scale)) {
		throw BinderException("%s: cannot compute the DECIMAL aggregate of type %s", function_name, type.ToString());
	}
	auto decimal_type = LogicalType::DECIMAL(width, scale);
	argument = BoundCastExpression::AddCastToType(context, std::move(argument), decimal_type);
	return decimal_type;
}

} // namespace duckdb
