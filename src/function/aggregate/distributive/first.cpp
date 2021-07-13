#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

template <class T>
struct FirstState {
	T value;
	bool is_set;
	bool is_null;
};

struct FirstFunctionBase {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->is_set = false;
		state->is_null = false;
	}

	static bool IgnoreNull() {
		return false;
	}
};

struct FirstFunction : public FirstFunctionBase {
	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			state->is_set = true;
			if (!mask.RowIsValid(idx)) {
				state->is_null = true;
			} else {
				state->is_null = false;
				state->value = input[idx];
			}
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (!target->is_set) {
			*target = source;
		}
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set || state->is_null) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = state->value;
		}
	}
};

struct FirstFunctionString : public FirstFunctionBase {
	template <class STATE>
	static void SetValue(STATE *state, string_t value, bool is_null) {
		state->is_set = true;
		if (is_null) {
			state->is_null = true;
		} else {
			if (value.IsInlined()) {
				state->value = value;
			} else {
				// non-inlined string, need to allocate space for it
				auto len = value.GetSize();
				auto ptr = new char[len];
				memcpy(ptr, value.GetDataUnsafe(), len);

				state->value = string_t(ptr, len);
			}
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask, idx_t idx) {
		if (!state->is_set) {
			SetValue(state, input[idx], !mask.RowIsValid(idx));
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE *state, FunctionData *bind_data, INPUT_TYPE *input, ValidityMask &mask,
	                              idx_t count) {
		Operation<INPUT_TYPE, STATE, OP>(state, bind_data, input, mask, 0);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (source.is_set && !target->is_set) {
			SetValue(target, source.value, source.is_null);
		}
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->is_set || state->is_null) {
			mask.SetInvalid(idx);
		} else {
			target[idx] = StringVector::AddString(result, state->value);
		}
	}

	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->is_set && !state->is_null && !state->value.IsInlined()) {
			delete[] state->value.GetDataUnsafe();
		}
	}
};

struct FirstStateValue {
	Value *value;
};

struct FirstValueFunction {
	template <class STATE>
	static void Initialize(STATE *state) {
		state->value = nullptr;
	}

	template <class STATE>
	static void Destroy(STATE *state) {
		if (state->value) {
			delete state->value;
		}
	}
	static bool IgnoreNull() {
		return false;
	}

	static void Update(Vector inputs[], FunctionData *, idx_t input_count, Vector &state_vector, idx_t count) {
		auto &input = inputs[0];
		VectorData sdata;
		state_vector.Orrify(count, sdata);

		auto states = (FirstStateValue **)sdata.data;
		for (idx_t i = 0; i < count; i++) {
			auto state = states[sdata.sel->get_index(i)];
			if (!state->value) {
				state->value = new Value(input.GetValue(i));
			}
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target) {
		if (source.value && !target->value) {
			target->value = new Value(*source.value);
		}
	}

	template <class T, class STATE>
	static void Finalize(Vector &result, FunctionData *, STATE *state, T *target, ValidityMask &mask, idx_t idx) {
		if (!state->value) {
			mask.SetInvalid(idx);
		} else {
			result.SetValue(idx, *state->value);
		}
	}

	static unique_ptr<FunctionData> Bind(ClientContext &context, AggregateFunction &function,
	                                     vector<unique_ptr<Expression>> &arguments) {
		function.arguments[0] = arguments[0]->return_type;
		function.return_type = arguments[0]->return_type;
		return nullptr;
	}
};

template <class T>
static AggregateFunction GetFirstAggregateTemplated(LogicalType type) {
	return AggregateFunction::UnaryAggregate<FirstState<T>, T, T, FirstFunction>(type, type);
}

AggregateFunction GetDecimalFirstFunction(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::DECIMAL);
	switch (type.InternalType()) {
	case PhysicalType::INT16:
		return FirstFun::GetFunction(LogicalType::SMALLINT);
	case PhysicalType::INT32:
		return FirstFun::GetFunction(LogicalType::INTEGER);
	case PhysicalType::INT64:
		return FirstFun::GetFunction(LogicalType::BIGINT);
	default:
		return FirstFun::GetFunction(LogicalType::HUGEINT);
	}
}

AggregateFunction FirstFun::GetFunction(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return GetFirstAggregateTemplated<int8_t>(type);
	case LogicalTypeId::TINYINT:
		return GetFirstAggregateTemplated<int8_t>(type);
	case LogicalTypeId::SMALLINT:
		return GetFirstAggregateTemplated<int16_t>(type);
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::DATE:
		return GetFirstAggregateTemplated<int32_t>(type);
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
		return GetFirstAggregateTemplated<int64_t>(type);
	case LogicalTypeId::UTINYINT:
		return GetFirstAggregateTemplated<uint8_t>(type);
	case LogicalTypeId::USMALLINT:
		return GetFirstAggregateTemplated<uint16_t>(type);
	case LogicalTypeId::UINTEGER:
		return GetFirstAggregateTemplated<uint32_t>(type);
	case LogicalTypeId::UBIGINT:
		return GetFirstAggregateTemplated<uint64_t>(type);
	case LogicalTypeId::HUGEINT:
		return GetFirstAggregateTemplated<hugeint_t>(type);
	case LogicalTypeId::FLOAT:
		return GetFirstAggregateTemplated<float>(type);
	case LogicalTypeId::DOUBLE:
		return GetFirstAggregateTemplated<double>(type);
	case LogicalTypeId::INTERVAL:
		return GetFirstAggregateTemplated<interval_t>(type);
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		return AggregateFunction::UnaryAggregateDestructor<FirstState<string_t>, string_t, string_t,
		                                                   FirstFunctionString>(type, type);
	case LogicalTypeId::DECIMAL: {
		type.Verify();
		AggregateFunction function = GetDecimalFirstFunction(type);
		function.arguments[0] = type;
		function.return_type = type;
		return function;
	}
	default:
		return AggregateFunction(
		    {type}, type, AggregateFunction::StateSize<FirstStateValue>,
		    AggregateFunction::StateInitialize<FirstStateValue, FirstValueFunction>, FirstValueFunction::Update,
		    AggregateFunction::StateCombine<FirstStateValue, FirstValueFunction>,
		    AggregateFunction::StateFinalize<FirstStateValue, void, FirstValueFunction>, nullptr,
		    FirstValueFunction::Bind, AggregateFunction::StateDestroy<FirstStateValue, FirstValueFunction>);
	}
}

unique_ptr<FunctionData> BindDecimalFirst(ClientContext &context, AggregateFunction &function,
                                          vector<unique_ptr<Expression>> &arguments) {
	auto decimal_type = arguments[0]->return_type;
	function = FirstFun::GetFunction(decimal_type);
	return nullptr;
}

void FirstFun::RegisterFunction(BuiltinFunctions &set) {
	AggregateFunctionSet first("first");
	for (auto &type : LogicalType::ALL_TYPES) {
		if (type.id() == LogicalTypeId::DECIMAL) {
			first.AddFunction(AggregateFunction({type}, type, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			                                    BindDecimalFirst));
		} else {
			first.AddFunction(FirstFun::GetFunction(type));
		}
	}
	set.AddFunction(first);
	first.name = "arbitrary";
	set.AddFunction(first);
}

} // namespace duckdb
