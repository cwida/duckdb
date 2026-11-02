#include "duckdb/common/types.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/reservoir_sample.hpp"
#include "core_functions/aggregate/holistic_functions.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include <algorithm>
#include <cstdint>
#include <stdlib.h>

namespace duckdb {

namespace {

//! The quantile defaults to the median
constexpr double DEFAULT_QUANTILE = 0.5;
constexpr int32_t DEFAULT_SAMPLE_SIZE = 8192;

template <typename T>
struct ReservoirQuantileState {
	T *v;
	idx_t len;
	idx_t pos;
	BaseReservoirSampling *r_samp;

	void Resize(idx_t new_len) {
		if (new_len <= len) {
			return;
		}
		T *old_v = v;
		v = (T *)realloc(v, new_len * sizeof(T));
		if (!v) {
			free(old_v);
			throw InternalException("Memory allocation failure");
		}
		len = new_len;
	}

	void ReplaceElement(T &input) {
		v[r_samp->min_weighted_entry_index] = input;
		r_samp->ReplaceElement();
	}

	void FillReservoir(idx_t sample_size, T element) {
		if (pos < sample_size) {
			v[pos++] = element;
			r_samp->InitializeReservoirWeights(pos, len);
		} else {
			D_ASSERT(r_samp->next_index_to_sample >= r_samp->num_entries_to_skip_b4_next_sample);
			if (r_samp->next_index_to_sample == r_samp->num_entries_to_skip_b4_next_sample) {
				ReplaceElement(element);
			} else {
				r_samp->num_entries_to_skip_b4_next_sample++;
			}
		}
	}
};

struct ReservoirQuantileBindData : public FunctionData {
	ReservoirQuantileBindData() : decimal_type(LogicalType::INVALID) {
	}
	ReservoirQuantileBindData(double quantile_p, idx_t sample_size_p)
	    : quantiles(1, quantile_p), sample_size(sample_size_p), decimal_type(LogicalType::INVALID) {
	}

	ReservoirQuantileBindData(vector<double> quantiles_p, idx_t sample_size_p)
	    : quantiles(std::move(quantiles_p)), sample_size(sample_size_p), decimal_type(LogicalType::INVALID) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ReservoirQuantileBindData>(quantiles, sample_size);
		result->decimal_type = decimal_type;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ReservoirQuantileBindData>();
		return quantiles == other.quantiles && sample_size == other.sample_size && decimal_type == other.decimal_type;
	}

	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
	                      const BoundAggregateFunction &function) {
		auto &bind_data = bind_data_p->Cast<ReservoirQuantileBindData>();
		serializer.WriteProperty(100, "quantiles", bind_data.quantiles);
		serializer.WriteProperty(101, "sample_size", bind_data.sample_size);
		serializer.WriteProperty(102, "decimal_type", bind_data.decimal_type);
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto result = make_uniq<ReservoirQuantileBindData>();
		deserializer.ReadProperty(100, "quantiles", result->quantiles);
		deserializer.ReadProperty(101, "sample_size", result->sample_size);
		result->decimal_type =
		    deserializer.ReadPropertyWithExplicitDefault<LogicalType>(102, "decimal_type", LogicalType::INVALID);
		return std::move(result);
	}

	vector<double> quantiles;
	idx_t sample_size;
	LogicalType decimal_type;
};

struct ReservoirQuantileOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		auto &bind_data = unary_input.input.bind_data->template Cast<ReservoirQuantileBindData>();
		if (state.pos == 0) {
			state.Resize(bind_data.sample_size);
		}
		if (!state.r_samp) {
			state.r_samp = new BaseReservoirSampling();
		}
		D_ASSERT(state.v);
		state.FillReservoir(bind_data.sample_size, input);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.pos == 0) {
			return;
		}
		if (target.pos == 0) {
			target.Resize(source.len);
		}
		if (!target.r_samp) {
			target.r_samp = new BaseReservoirSampling();
		}
		for (idx_t src_idx = 0; src_idx < source.pos; src_idx++) {
			target.FillReservoir(target.len, source.v[src_idx]);
		}
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &aggr_input_data) {
		if (state.v) {
			free(state.v);
			state.v = nullptr;
		}
		if (state.r_samp) {
			delete state.r_samp;
			state.r_samp = nullptr;
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct ReservoirQuantileScalarOperation : public ReservoirQuantileOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.pos == 0) {
			finalize_data.ReturnNull();
			return;
		}
		D_ASSERT(state.v);
		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->template Cast<ReservoirQuantileBindData>();
		auto v_t = state.v;
		D_ASSERT(bind_data.quantiles.size() == 1);
		auto offset = (idx_t)((double)(state.pos - 1) * bind_data.quantiles[0]);
		std::nth_element(v_t, v_t + offset, v_t + state.pos);
		target = v_t[offset];
	}
};

AggregateFunction GetReservoirQuantileAggregateFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT8:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<int8_t>, int8_t, int8_t,
		                                         ReservoirQuantileScalarOperation>(LogicalType::TINYINT,
		                                                                           LogicalType::TINYINT);

	case PhysicalType::INT16:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<int16_t>, int16_t, int16_t,
		                                         ReservoirQuantileScalarOperation>(LogicalType::SMALLINT,
		                                                                           LogicalType::SMALLINT);

	case PhysicalType::INT32:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<int32_t>, int32_t, int32_t,
		                                         ReservoirQuantileScalarOperation>(LogicalType::INTEGER,
		                                                                           LogicalType::INTEGER);

	case PhysicalType::INT64:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<int64_t>, int64_t, int64_t,
		                                         ReservoirQuantileScalarOperation>(LogicalType::BIGINT,
		                                                                           LogicalType::BIGINT);

	case PhysicalType::INT128:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<hugeint_t>, hugeint_t, hugeint_t,
		                                         ReservoirQuantileScalarOperation>(LogicalType::HUGEINT,
		                                                                           LogicalType::HUGEINT);
	case PhysicalType::FLOAT:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<float>, float, float,
		                                         ReservoirQuantileScalarOperation>(LogicalType::FLOAT,
		                                                                           LogicalType::FLOAT);
	case PhysicalType::DOUBLE:
		return AggregateFunction::UnaryAggregate<ReservoirQuantileState<double>, double, double,
		                                         ReservoirQuantileScalarOperation>(LogicalType::DOUBLE,
		                                                                           LogicalType::DOUBLE);
	default:
		throw InternalException("Unimplemented reservoir quantile aggregate");
	}
}

template <class CHILD_TYPE>
struct ReservoirQuantileListOperation : public ReservoirQuantileOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.pos == 0) {
			finalize_data.ReturnNull();
			return;
		}

		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->template Cast<ReservoirQuantileBindData>();

		auto &result = ListVector::GetChildMutable(finalize_data.result);
		auto ridx = ListVector::GetListSize(finalize_data.result);
		ListVector::Reserve(finalize_data.result, ridx + bind_data.quantiles.size());
		auto rdata = FlatVector::GetDataMutable<CHILD_TYPE>(result);

		auto v_t = state.v;
		D_ASSERT(v_t);

		auto &entry = target;
		entry.offset = ridx;
		entry.length = bind_data.quantiles.size();
		for (size_t q = 0; q < entry.length; ++q) {
			const auto &quantile = bind_data.quantiles[q];
			auto offset = (idx_t)((double)(state.pos - 1) * quantile);
			std::nth_element(v_t, v_t + offset, v_t + state.pos);
			rdata[ridx + q] = v_t[offset];
		}

		ListVector::SetListSize(finalize_data.result, entry.offset + entry.length);
	}
};

template <class STATE, class INPUT_TYPE, class RESULT_TYPE, class OP>
AggregateFunction ReservoirQuantileListAggregate(const LogicalType &input_type, const LogicalType &child_type) {
	LogicalType result_type = LogicalType::LIST(child_type);
	return AggregateFunction(
	    {input_type}, result_type, AggregateFunction::StateSize<STATE>, AggregateFunction::StateInitialize<STATE, OP>,
	    AggregateFunction::UnaryScatterUpdate<STATE, INPUT_TYPE, OP>, AggregateFunction::StateCombine<STATE, OP>,
	    AggregateFunction::StateFinalize<STATE, RESULT_TYPE, OP>, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	    AggregateFunction::NoClusterUpdate(), AggregateFunction::NoBind(), AggregateFunction::StateDestroy<STATE, OP>);
}

template <typename INPUT_TYPE, typename SAVE_TYPE>
AggregateFunction GetTypedReservoirQuantileListAggregateFunction(const LogicalType &type) {
	using STATE = ReservoirQuantileState<SAVE_TYPE>;
	using OP = ReservoirQuantileListOperation<INPUT_TYPE>;
	auto fun = ReservoirQuantileListAggregate<STATE, INPUT_TYPE, list_entry_t, OP>(type, type);
	return fun;
}

AggregateFunction GetReservoirQuantileListAggregateFunction(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return GetTypedReservoirQuantileListAggregateFunction<int8_t, int8_t>(type);
	case LogicalTypeId::SMALLINT:
		return GetTypedReservoirQuantileListAggregateFunction<int16_t, int16_t>(type);
	case LogicalTypeId::INTEGER:
		return GetTypedReservoirQuantileListAggregateFunction<int32_t, int32_t>(type);
	case LogicalTypeId::BIGINT:
		return GetTypedReservoirQuantileListAggregateFunction<int64_t, int64_t>(type);
	case LogicalTypeId::HUGEINT:
		return GetTypedReservoirQuantileListAggregateFunction<hugeint_t, hugeint_t>(type);
	case LogicalTypeId::FLOAT:
		return GetTypedReservoirQuantileListAggregateFunction<float, float>(type);
	case LogicalTypeId::DOUBLE:
		return GetTypedReservoirQuantileListAggregateFunction<double, double>(type);
	case LogicalTypeId::DECIMAL:
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return GetTypedReservoirQuantileListAggregateFunction<int16_t, int16_t>(type);
		case PhysicalType::INT32:
			return GetTypedReservoirQuantileListAggregateFunction<int32_t, int32_t>(type);
		case PhysicalType::INT64:
			return GetTypedReservoirQuantileListAggregateFunction<int64_t, int64_t>(type);
		case PhysicalType::INT128:
			return GetTypedReservoirQuantileListAggregateFunction<hugeint_t, hugeint_t>(type);
		default:
			throw NotImplementedException("Unimplemented reservoir quantile list aggregate");
		}
	default:
		// TODO: Add quantitative temporal types
		throw NotImplementedException("Unimplemented reservoir quantile list aggregate");
	}
}

double CheckReservoirQuantile(const Value &quantile_val) {
	if (quantile_val.IsNull()) {
		throw BinderException("RESERVOIR_QUANTILE QUANTILE parameter cannot be NULL");
	}
	auto quantile = quantile_val.GetValue<double>();
	if (Value::IsNan(quantile)) {
		throw BinderException("RESERVOIR_QUANTILE parameter cannot be NaN");
	}
	if (quantile < 0 || quantile > 1) {
		throw BinderException("RESERVOIR_QUANTILE can only take parameters in the range [0, 1]");
	}
	return quantile;
}

//! Binds the quantile parameter and the sample size into the bind data. They stay part of the expression tree, and
//! the aggregate is handed them along with the input - the update callbacks only consume the leading input argument
unique_ptr<FunctionData> BindReservoirQuantile(BindAggregateFunctionInput &input) {
	Value quantile_val = input.GetConstant(1);
	vector<double> quantiles;
	if (quantile_val.type().id() != LogicalTypeId::LIST) {
		quantiles.push_back(CheckReservoirQuantile(quantile_val));
	} else {
		for (const auto &element_val : ListValue::GetChildren(quantile_val)) {
			quantiles.push_back(CheckReservoirQuantile(element_val));
		}
	}

	auto sample_size_val = input.GetNonNullConstant(2);
	auto sample_size = sample_size_val.GetValue<int32_t>();
	if (sample_size <= 0) {
		throw BinderException("Size of the RESERVOIR_QUANTILE sample must be bigger than 0");
	}

	return make_uniq<ReservoirQuantileBindData>(quantiles, NumericCast<idx_t>(sample_size));
}

static unique_ptr<FunctionData> DeserializeReservoirQuantileDecimal(Deserializer &deserializer,
                                                                    BoundAggregateFunction &function);

static void SetDecimalImplementation(BoundAggregateFunction &function, const LogicalType &decimal_type, bool is_list) {
	auto declared_arguments = function.GetArguments();
	if (is_list) {
		function.ReplaceImplementation(GetReservoirQuantileListAggregateFunction(decimal_type));
	} else {
		switch (decimal_type.InternalType()) {
		case PhysicalType::INT16:
			function.ReplaceImplementation(
			    AggregateFunction::UnaryAggregate<ReservoirQuantileState<int16_t>, int16_t, int16_t,
			                                      ReservoirQuantileScalarOperation>(decimal_type, decimal_type));
			break;
		case PhysicalType::INT32:
			function.ReplaceImplementation(
			    AggregateFunction::UnaryAggregate<ReservoirQuantileState<int32_t>, int32_t, int32_t,
			                                      ReservoirQuantileScalarOperation>(decimal_type, decimal_type));
			break;
		case PhysicalType::INT64:
			function.ReplaceImplementation(
			    AggregateFunction::UnaryAggregate<ReservoirQuantileState<int64_t>, int64_t, int64_t,
			                                      ReservoirQuantileScalarOperation>(decimal_type, decimal_type));
			break;
		case PhysicalType::INT128:
			function.ReplaceImplementation(
			    AggregateFunction::UnaryAggregate<ReservoirQuantileState<hugeint_t>, hugeint_t, hugeint_t,
			                                      ReservoirQuantileScalarOperation>(decimal_type, decimal_type));
			break;
		default:
			throw InternalException("Invalid physical type for decimal reservoir quantile");
		}
	}
	for (idx_t i = function.GetArguments().size(); i < declared_arguments.size(); i++) {
		function.GetArguments().push_back(declared_arguments[i]);
	}

	function.SetName("reservoir_quantile");
	function.SetSerializeCallback(ReservoirQuantileBindData::Serialize);
	function.SetDeserializeCallback(DeserializeReservoirQuantileDecimal);
}

static unique_ptr<FunctionData> DeserializeReservoirQuantileDecimal(Deserializer &deserializer,
                                                                    BoundAggregateFunction &function) {
	auto result = ReservoirQuantileBindData::Deserialize(deserializer, function);
	auto &bind_data = result->Cast<ReservoirQuantileBindData>();
	auto &return_type = deserializer.Get<const LogicalType &>();
	bool is_list = function.GetReturnType().id() == LogicalTypeId::LIST || return_type.id() == LogicalTypeId::LIST;
	if (bind_data.decimal_type.id() == LogicalTypeId::INVALID && !function.GetArguments().empty() &&
	    function.GetArguments()[0].id() == LogicalTypeId::DECIMAL) {
		bind_data.decimal_type = function.GetArguments()[0];
	}

	if (bind_data.decimal_type.id() != LogicalTypeId::INVALID) {
		SetDecimalImplementation(function, bind_data.decimal_type, is_list);
	}

	return result;
}

unique_ptr<FunctionData> BindReservoirQuantileDecimal(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();

	auto decimal_type = arguments[0]->GetReturnType();
	bool is_list = arguments[1]->GetReturnType().id() == LogicalTypeId::LIST;

	auto bind_data_ptr = BindReservoirQuantile(input);
	auto &bind_data = bind_data_ptr->Cast<ReservoirQuantileBindData>();
	bind_data.decimal_type = decimal_type;

	SetDecimalImplementation(function, decimal_type, is_list);
	return bind_data_ptr;
}

AggregateFunction GetReservoirQuantileAggregate(PhysicalType type) {
	auto fun = GetReservoirQuantileAggregateFunction(type);
	fun.SetBindCallback(BindReservoirQuantile);
	fun.SetSerializeCallback(ReservoirQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ReservoirQuantileBindData::Deserialize);
	// temporarily push an argument so we can bind the actual quantile
	fun.GetSignature().GetParameter(0).SetName("x");
	fun.GetSignature().AddParameter("quantile", LogicalType::DOUBLE, Value::DOUBLE(DEFAULT_QUANTILE));
	return fun;
}

AggregateFunction GetReservoirQuantileListAggregate(const LogicalType &type) {
	auto fun = GetReservoirQuantileListAggregateFunction(type);
	fun.SetBindCallback(BindReservoirQuantile);
	fun.SetSerializeCallback(ReservoirQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ReservoirQuantileBindData::Deserialize);
	// temporarily push an argument so we can bind the actual quantile
	fun.GetSignature().GetParameter(0).SetName("x");
	auto list_of_double = LogicalType::LIST(LogicalType::DOUBLE);
	fun.GetSignature().AddParameter("quantile", list_of_double);
	return fun;
}

void DefineReservoirQuantile(AggregateFunctionSet &set, const LogicalType &type) {
	//    Two versions: type, scalar/list
	auto fun = GetReservoirQuantileAggregate(type.InternalType());
	fun.GetSignature().AddParameter("sample_size", LogicalType::INTEGER, Value::INTEGER(DEFAULT_SAMPLE_SIZE));
	set.AddFunction(fun);

	// List variant
	fun = GetReservoirQuantileListAggregate(type);
	fun.GetSignature().AddParameter("sample_size", LogicalType::INTEGER, Value::INTEGER(DEFAULT_SAMPLE_SIZE));
	set.AddFunction(fun);
}

void GetReservoirQuantileDecimalFunction(AggregateFunctionSet &set, const vector<LogicalType> &arguments,
                                         const LogicalType &return_value) {
	auto fun = AggregateFunction::UnaryAggregate<ReservoirQuantileState<int64_t>, int64_t, int64_t,
	                                             ReservoirQuantileScalarOperation>(arguments[0], return_value);
	fun.SetBindCallback(BindReservoirQuantileDecimal);
	fun.SetSerializeCallback(ReservoirQuantileBindData::Serialize);
	fun.SetDeserializeCallback(DeserializeReservoirQuantileDecimal);

	fun.GetSignature().GetParameter(0).SetName("x");
	if (arguments[1].id() == LogicalTypeId::LIST) {
		fun.GetSignature().AddParameter("quantile", arguments[1]);
	} else {
		fun.GetSignature().AddParameter("quantile", arguments[1], Value::DOUBLE(DEFAULT_QUANTILE));
	}
	fun.GetSignature().AddParameter("sample_size", LogicalType::INTEGER, Value::INTEGER(DEFAULT_SAMPLE_SIZE));
	set.AddFunction(fun);
}

} // namespace

AggregateFunctionSet ReservoirQuantileFun::GetFunctions() {
	AggregateFunctionSet reservoir_quantile;

	// DECIMAL
	GetReservoirQuantileDecimalFunction(reservoir_quantile, {LogicalTypeId::DECIMAL, LogicalType::DOUBLE},
	                                    LogicalTypeId::DECIMAL);
	GetReservoirQuantileDecimalFunction(reservoir_quantile,
	                                    {LogicalTypeId::DECIMAL, LogicalType::LIST(LogicalType::DOUBLE)},
	                                    LogicalType::LIST(LogicalTypeId::DECIMAL));

	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::TINYINT);
	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::SMALLINT);
	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::INTEGER);
	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::BIGINT);
	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::HUGEINT);
	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::FLOAT);
	DefineReservoirQuantile(reservoir_quantile, LogicalTypeId::DOUBLE);
	return reservoir_quantile;
}

} // namespace duckdb
