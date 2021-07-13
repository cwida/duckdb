#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/aggregate_executor.hpp"

namespace duckdb {

template <class T>
struct BitState {
	bool is_set;
	T value;
};

template <class OP>
static AggregateFunction GetBitfieldUnaryAggregate(LogicalType type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return AggregateFunction::UnaryAggregate<BitState<uint8_t>, int8_t, int8_t, OP>(type, type);
	case LogicalTypeId::SMALLINT:
		return AggregateFunction::UnaryAggregate<BitState<uint16_t>, int16_t, int16_t, OP>(type, type);
	case LogicalTypeId::INTEGER:
		return AggregateFunction::UnaryAggregate<BitState<uint32_t>, int32_t, int32_t, OP>(type, type);
	case LogicalTypeId::BIGINT:
		return AggregateFunction::UnaryAggregate<BitState<uint64_t>, int64_t, int64_t, OP>(type, type);
	case LogicalTypeId::HUGEINT:
		return AggregateFunction::UnaryAggregate<BitState<hugeint_t>, hugeint_t, hugeint_t, OP>(type, type);
	case LogicalTypeId::UTINYINT:
		return AggregateFunction::UnaryAggregate<BitState<uint8_t>, uint8_t, uint8_t, OP>(type, type);
	case LogicalTypeId::USMALLINT:
		return AggregateFunction::UnaryAggregate<BitState<uint16_t>, uint16_t, uint16_t, OP>(type, type);
	case LogicalTypeId::UINTEGER:
		return AggregateFunction::UnaryAggregate<BitState<uint32_t>, uint32_t, uint32_t, OP>(type, type);
	case LogicalTypeId::UBIGINT:
		return AggregateFunction::UnaryAggregate<BitState<uint64_t>, uint64_t, uint64_t, OP>(type, type);
	default:
		throw InternalException("Unimplemented bitfield type for unary aggregate");
	}
}

struct BitAndOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		//  If there are no matching rows, BIT_AND() returns a null value.
		state->is_set = false;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			state->is_set = true;
			state->value = input[idx];
		} else {
			state->value &= input[idx];
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		//  count is irrelevant
		Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = state->value;
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!source.is_set) {
			// source is NULL, nothing to do.
			return;
		}
		if (!target->is_set) {
			// target is NULL, use source value directly.
			*target = source;
		} else {
			target->value &= source.value;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

void BitAndFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bit_and("bit_and");
	for (auto &type : LogicalType::INTEGRAL) {
		bit_and.AddFunction(GetBitfieldUnaryAggregate<BitAndOperation>(type));
	}
	set.AddFunction(bit_and);
}

struct BitOrOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		//  If there are no matching rows, BIT_OR() returns a null value.
		state->is_set = false;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			state->is_set = true;
			state->value = input[idx];
		} else {
			state->value |= input[idx];
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		//  count is irrelevant
		Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = state->value;
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!source.is_set) {
			// source is NULL, nothing to do.
			return;
		}
		if (!target->is_set) {
			// target is NULL, use source value directly.
			*target = source;
		} else {
			target->value |= source.value;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

void BitOrFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bit_or("bit_or");
	for (auto &type : LogicalType::INTEGRAL) {
		bit_or.AddFunction(GetBitfieldUnaryAggregate<BitOrOperation>(type));
	}
	set.AddFunction(bit_or);
}

struct BitXorOperation {
	template <class STATE>
	static void Initialize(STATE *state) {
		//  If there are no matching rows, BIT_XOR() returns a null value.
		state->is_set = false;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			state->is_set = true;
			state->value = input[idx];
		} else {
			state->value ^= input[idx];
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		//  count is irrelevant
		Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = state->value;
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!source.is_set) {
			// source is NULL, nothing to do.
			return;
		}
		if (!target->is_set) {
			// target is NULL, use source value directly.
			*target = source;
		} else {
			target->value ^= source.value;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

void BitXorFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet bit_xor("bit_xor");
	for (auto &type : LogicalType::INTEGRAL) {
		bit_xor.AddFunction(GetBitfieldUnaryAggregate<BitXorOperation>(type));
	}
	set.AddFunction(bit_xor);
}

} // namespace duckdb
