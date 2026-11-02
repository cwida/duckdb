#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/aggregate_operators.hpp"
#include "duckdb/common/vector_operations/aggregate_executor.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/aggregate_state_layout.hpp"

namespace duckdb {

namespace {

template <class T>
struct BitState {
	using value_type = T;
	using STATE_TYPE = OptionalStateType<StateTypedValue<T, StateReturnType>>;
	T value;
	bool is_set;
};

static AggregateFunction NameArgParameter(AggregateFunction fun) {
	fun.GetSignature().GetParameter(0).SetName("arg");
	return fun;
}

template <class OP>
AggregateFunction GetBitfieldUnaryAggregate(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return NameArgParameter(AggregateFunction::UnaryAggregate<BitState<uint8_t>, int8_t, int8_t, OP>(type, type));
	case LogicalTypeId::SMALLINT:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uint16_t>, int16_t, int16_t, OP>(type, type));
	case LogicalTypeId::INTEGER:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uint32_t>, int32_t, int32_t, OP>(type, type));
	case LogicalTypeId::BIGINT:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uint64_t>, int64_t, int64_t, OP>(type, type));
	case LogicalTypeId::HUGEINT:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<hugeint_t>, hugeint_t, hugeint_t, OP>(type, type));
	case LogicalTypeId::UTINYINT:
		return NameArgParameter(AggregateFunction::UnaryAggregate<BitState<uint8_t>, uint8_t, uint8_t, OP>(type, type));
	case LogicalTypeId::USMALLINT:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uint16_t>, uint16_t, uint16_t, OP>(type, type));
	case LogicalTypeId::UINTEGER:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uint32_t>, uint32_t, uint32_t, OP>(type, type));
	case LogicalTypeId::UBIGINT:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uint64_t>, uint64_t, uint64_t, OP>(type, type));
	case LogicalTypeId::UHUGEINT:
		return NameArgParameter(
		    AggregateFunction::UnaryAggregate<BitState<uhugeint_t>, uhugeint_t, uhugeint_t, OP>(type, type));
	default:
		throw InternalException("Unimplemented bitfield type for unary aggregate");
	}
}

struct BitwiseOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		if (!state.is_set) {
			OP::template Assign<INPUT_TYPE>(state, input);
			state.is_set = true;
		} else {
			OP::template Execute<INPUT_TYPE>(state, input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		OP::template Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!source.is_set) {
			// source is NULL, nothing to do.
			return;
		}
		if (!target.is_set) {
			// target is NULL, use source value directly.
			OP::template Assign<typename STATE::value_type>(target, source.value);
			target.is_set = true;
		} else {
			OP::template Execute<typename STATE::value_type>(target, source.value);
		}
	}

	template <class INPUT_TYPE, class STATE>
	static void Assign(STATE &state, INPUT_TYPE input) {
		state.value = typename STATE::value_type(input);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = T(state.value);
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

template <class OP>
struct NumericBitwiseOperation // NOLINT(bugprone-crtp-constructor-accessibility)
    : public BitwiseOperation,
      public ClusteredStateCopy {
	template <class INPUT_TYPE, class STATE>
	static void UpdateClusteredLocal(STATE &local, const INPUT_TYPE &input) {
		if (!local.is_set) {
			Assign(local, input);
			local.is_set = true;
		} else {
			OP::template Execute<INPUT_TYPE>(local, input);
		}
	}

	template <class INPUT_TYPE, class STATE>
	static void UpdateClusteredLocal(STATE &local, const INPUT_TYPE &input, idx_t count) {
		if (count != 0) {
			UpdateClusteredLocal(local, input);
		}
	}
};

template <class OP>
struct SimpleBitwiseOperation : public NumericBitwiseOperation<SimpleBitwiseOperation<OP>> {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, INPUT_TYPE input) {
		state.value =
		    OP::template Operation<typename STATE::value_type>(state.value, typename STATE::value_type(input));
	}
};

using BitAndOperation = SimpleBitwiseOperation<BitAnd>;
using BitOrOperation = SimpleBitwiseOperation<BitOr>;

struct BitXorOperation : public NumericBitwiseOperation<BitXorOperation> {
	using NumericBitwiseOperation<BitXorOperation>::UpdateClusteredLocal;

	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, INPUT_TYPE input) {
		state.value =
		    BitXor::template Operation<typename STATE::value_type>(state.value, typename STATE::value_type(input));
	}

	template <class INPUT_TYPE, class STATE>
	static void UpdateClusteredLocal(STATE &local, const INPUT_TYPE &input, idx_t count) {
		if ((count & 1) != 0) {
			NumericBitwiseOperation<BitXorOperation>::template UpdateClusteredLocal<INPUT_TYPE>(local, input);
		} else if (count != 0 && !local.is_set) {
			local.value = typename STATE::value_type(0);
			local.is_set = true;
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}
};

using BitStringState = BitState<string_t>;

struct BitStringBitwiseOperation : public BitwiseOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		if (!state.is_set) {
			OP::template Assign<INPUT_TYPE>(state, input, unary_input.input);
			state.is_set = true;
		} else {
			OP::template Execute<INPUT_TYPE>(state, input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		if (!source.is_set) {
			// source is NULL, nothing to do.
			return;
		}
		if (!target.is_set) {
			// target is NULL, use source value directly.
			OP::template Assign<typename STATE::value_type>(target, source.value, aggr_input_data);
			target.is_set = true;
		} else {
			OP::template Execute<typename STATE::value_type>(target, source.value);
		}
	}

	template <class INPUT_TYPE, class STATE>
	static void Assign(STATE &state, INPUT_TYPE input, AggregateInputData &aggr_input_data) {
		D_ASSERT(state.is_set == false);
		if (input.IsInlined()) {
			state.value = input;
		} else { // non-inlined string, need to allocate space for it
			auto len = input.GetSize();
			auto ptr = aggr_input_data.allocator.Allocate(len);
			memcpy(ptr, input.GetData(), len);
			state.value = string_t(char_ptr_cast(ptr), UnsafeNumericCast<uint32_t>(len));
		}
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = finalize_data.ReturnString(state.value);
		}
	}
};

struct BitStringAndOperation : public BitStringBitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, INPUT_TYPE input) {
		Bit::BitwiseAnd(input, state.value, state.value);
	}
};

struct BitStringOrOperation : public BitStringBitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, INPUT_TYPE input) {
		Bit::BitwiseOr(input, state.value, state.value);
	}
};

struct BitStringXorOperation : public BitStringBitwiseOperation {
	template <class INPUT_TYPE, class STATE>
	static void Execute(STATE &state, INPUT_TYPE input) {
		Bit::BitwiseXor(input, state.value, state.value);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}
};

} // namespace

AggregateFunctionSet BitAndFun::GetFunctions() {
	AggregateFunctionSet bit_and;
	for (auto &type : LogicalType::Integral()) {
		bit_and.AddFunction(GetBitfieldUnaryAggregate<BitAndOperation>(type));
	}
	auto bit_and_bit =
	    NameArgParameter(AggregateFunction::UnaryAggregate<BitStringState, string_t, string_t, BitStringAndOperation>(
	        LogicalType::BIT, LogicalType::BIT));
	bit_and_bit.SetFallible(); // combining bit strings of different lengths throws at runtime
	bit_and.AddFunction(std::move(bit_and_bit));
	return bit_and;
}

AggregateFunctionSet BitOrFun::GetFunctions() {
	AggregateFunctionSet bit_or;
	for (auto &type : LogicalType::Integral()) {
		bit_or.AddFunction(GetBitfieldUnaryAggregate<BitOrOperation>(type));
	}
	auto bit_or_bit =
	    NameArgParameter(AggregateFunction::UnaryAggregate<BitStringState, string_t, string_t, BitStringOrOperation>(
	        LogicalType::BIT, LogicalType::BIT));
	bit_or_bit.SetFallible(); // combining bit strings of different lengths throws at runtime
	bit_or.AddFunction(std::move(bit_or_bit));
	return bit_or;
}

AggregateFunctionSet BitXorFun::GetFunctions() {
	AggregateFunctionSet bit_xor;
	for (auto &type : LogicalType::Integral()) {
		bit_xor.AddFunction(GetBitfieldUnaryAggregate<BitXorOperation>(type));
	}
	auto bit_xor_bit =
	    NameArgParameter(AggregateFunction::UnaryAggregate<BitStringState, string_t, string_t, BitStringXorOperation>(
	        LogicalType::BIT, LogicalType::BIT));
	bit_xor_bit.SetFallible(); // combining bit strings of different lengths throws at runtime
	bit_xor.AddFunction(std::move(bit_xor_bit));
	return bit_xor;
}

} // namespace duckdb
