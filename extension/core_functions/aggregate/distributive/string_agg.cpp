#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

namespace {

struct StringAggState {
	using STATE_TYPE = OptionalStateType<StateString<StateReturnType>>;

	string_t value;
	bool is_set;
	uint32_t alloc_size;
};

struct StringAggBindData : public FunctionData {
	explicit StringAggBindData(string sep_p) : sep(std::move(sep_p)) {
	}

	string sep;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StringAggBindData>(sep);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<StringAggBindData>();
		return sep == other.sep;
	}
};

struct StringAggFunction {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = state.value;
		}
	}

	static bool IgnoreNull() {
		return true;
	}

	static inline void PerformOperation(StringAggState &state, ArenaAllocator &allocator, const char *str,
	                                    const char *sep, idx_t str_size, idx_t sep_size) {
		idx_t new_size;
		auto current_size = state.value.GetSize();
		if (!state.is_set) {
			new_size = str_size;
		} else {
			new_size = current_size + sep_size + str_size;
		}
		char *target_data;
		if (new_size > state.alloc_size && new_size > string_t::INLINE_LENGTH) {
			// need to (re-)allocate space
			if (new_size > NumericLimits<uint32_t>::Maximum()) {
				throw InvalidInputException("string_agg string size exceeds maximum string size");
			}
			state.alloc_size = MinValue<idx_t>(NextPowerOfTwo(new_size), string_t::MAX_STRING_SIZE);
			target_data = char_ptr_cast(allocator.Allocate(state.alloc_size));
			if (current_size > 0) {
				// copy over the current data
				memcpy(target_data, state.value.GetData(), current_size);
			}
			state.value = string_t(target_data, new_size);
		} else {
			target_data = state.value.GetDataWriteable();
		}
		if (!state.is_set) {
			memcpy(target_data, str, str_size);
			state.is_set = true;
		} else {
			// copy the separator
			target_data += current_size;
			memcpy(target_data, sep, sep_size);
			// copy the string
			target_data += sep_size;
			memcpy(target_data, str, str_size);
		}
		state.value.SetSizeAndFinalize(new_size, state.alloc_size);
	}

	static inline void PerformOperation(StringAggState &state, ArenaAllocator &allocator, string_t str,
	                                    optional_ptr<FunctionData> data_p) {
		auto &data = data_p->Cast<StringAggBindData>();
		PerformOperation(state, allocator, str.GetData(), data.sep.c_str(), str.GetSize(), data.sep.size());
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		PerformOperation(state, unary_input.input.allocator, input, unary_input.input.bind_data);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		if (!source.is_set) {
			// source is not set: skip combining
			return;
		}
		PerformOperation(target, aggr_input_data.allocator, source.value, aggr_input_data.bind_data);
	}
};

//! Binds the separator into the bind data. It stays part of the expression tree, and the aggregate is handed it
//! along with the input - the update callback only consumes the leading input argument
unique_ptr<FunctionData> StringAggBind(BindAggregateFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.size() == 1) {
		// single argument: default to comma
		return make_uniq<StringAggBindData>(",");
	}
	D_ASSERT(arguments.size() == 2);
	// Check if any argument is of UNKNOWN type (parameter not yet bound)
	for (auto &arg : arguments) {
		if (arg->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
	}
	auto separator_val = input.GetConstant(1);
	string separator_string = ",";
	if (separator_val.IsNull()) {
		arguments[0] = make_uniq<BoundConstantExpression>(Value(LogicalType::VARCHAR));
	} else {
		separator_string = separator_val.ToString();
	}
	return make_uniq<StringAggBindData>(std::move(separator_string));
}

void StringAggSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                        const BoundAggregateFunction &function) {
	auto bind_data = bind_data_p->Cast<StringAggBindData>();
	serializer.WriteProperty(100, "separator", bind_data.sep);
}

unique_ptr<FunctionData> StringAggDeserialize(Deserializer &deserializer, BoundAggregateFunction &bound_function) {
	auto sep = deserializer.ReadProperty<string>(100, "separator");
	return make_uniq<StringAggBindData>(std::move(sep));
}

AggregateStateLayout StringAggStateType(AggregateLayoutInput &input) {
	auto &function = input.function;
	using ST = StringAggState::STATE_TYPE;
	AggregateStateLayout layout;
	layout.type = AggregateFunction::BuildStateLogical<ST, StringAggState>(function);
	layout.total_state_size = AlignValue<idx_t>(sizeof(StringAggState));
	layout.field = BuildStateField<ST>();
	if (function.GetArguments().size() == 2) {
		// record the value of the separator if explicitly provided
		layout.constant_parameters.emplace(1, Value(input.bind_data->Cast<StringAggBindData>().sep));
	}
	return layout;
}

} // namespace

AggregateFunctionSet StringAggFun::GetFunctions() {
	AggregateFunctionSet string_agg;
	AggregateFunction string_agg_param(
	    {LogicalType::ANY_PARAMS(LogicalType::VARCHAR)}, LogicalType::VARCHAR,
	    AggregateFunction::StateSize<StringAggState>,
	    AggregateFunction::StateInitialize<StringAggState, StringAggFunction>,
	    AggregateFunction::UnaryScatterUpdate<StringAggState, string_t, StringAggFunction>,
	    AggregateFunction::StateCombine<StringAggState, StringAggFunction>,
	    AggregateFunction::StateFinalize<StringAggState, string_t, StringAggFunction>,
	    FunctionNullHandling::DEFAULT_NULL_HANDLING, AggregateFunction::NoClusterUpdate(), StringAggBind);

	string_agg_param.GetSignature().GetParameter(0).SetName("str");
	string_agg_param.SetSerializeCallback(StringAggSerialize);
	string_agg_param.SetDeserializeCallback(StringAggDeserialize);
	string_agg_param.SetStructStateExport(StringAggStateType);
	string_agg_param.SetFallible(); // the update callback throws when the result exceeds the maximum string size
	string_agg.AddFunction(string_agg_param);
	string_agg_param.GetSignature().AddParameter("arg", LogicalType::VARCHAR);
	string_agg.AddFunction(string_agg_param);
	return string_agg;
}

} // namespace duckdb
