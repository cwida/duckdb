#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "core_functions/aggregate/holistic_functions.hpp"
#include "t_digest.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include <stdlib.h>

namespace duckdb {

namespace {

struct ApproxQuantileState {
	duckdb_tdigest::TDigest *h;
	idx_t pos;
};

struct ApproxQuantileCoding {
	template <typename INPUT_TYPE, typename SAVE_TYPE>
	static SAVE_TYPE Encode(const INPUT_TYPE &input) {
		return Cast::template Operation<INPUT_TYPE, SAVE_TYPE>(input);
	}

	template <typename SAVE_TYPE, typename TARGET_TYPE>
	static bool Decode(const SAVE_TYPE &source, TARGET_TYPE &target) {
		// The result is approximate, so clamp instead of overflowing.
		if (TryCast::Operation(source, target, false)) {
			return true;
		} else if (source < 0) {
			target = NumericLimits<TARGET_TYPE>::Minimum();
		} else {
			target = NumericLimits<TARGET_TYPE>::Maximum();
		}
		return false;
	}

	template <typename SAVE_TYPE, typename TARGET_TYPE>
	static void Decode(const SAVE_TYPE &source, TARGET_TYPE &target, const LogicalType &type) {
		Decode(source, target);
		if (type.id() != LogicalTypeId::DECIMAL) {
			return;
		}
		// The unscaled decimal range can be narrower than its physical integer range.
		const auto limit = Hugeint::POWERS_OF_TEN[DecimalType::GetWidth(type)] - 1;
		const auto value = Cast::Operation<TARGET_TYPE, hugeint_t>(target);
		if (value > limit) {
			target = Cast::Operation<hugeint_t, TARGET_TYPE>(limit);
		} else if (value < -limit) {
			target = Cast::Operation<hugeint_t, TARGET_TYPE>(-limit);
		}
	}
};

template <>
double ApproxQuantileCoding::Encode(const dtime_t &input) {
	return Encode<int64_t, double>(input.value);
}

template <>
bool ApproxQuantileCoding::Decode(const double &source, dtime_t &target) {
	int64_t micros;
	auto decoded = Decode<double, int64_t>(source, micros);
	if (micros < 0 || micros > Interval::MICROS_PER_DAY) {
		micros = MinValue<int64_t>(MaxValue<int64_t>(micros, 0), Interval::MICROS_PER_DAY);
		decoded = false;
	}
	target = dtime_t(micros);
	return decoded;
}

template <>
double ApproxQuantileCoding::Encode(const dtime_tz_t &input) {
	return Encode<uint64_t, double>(input.sort_key());
}

template <>
bool ApproxQuantileCoding::Decode(const double &source, dtime_tz_t &target) {
	uint64_t sort_key;
	const auto decoded = Decode<double, uint64_t>(source, sort_key);
	if (decoded) {
		//	The offset bits of an interpolated sort key are meaningless: return the normalized time at +00.
		auto micros = dtime_tz_t::decode_micros(sort_key);
		micros -= int64_t(dtime_tz_t::MAX_OFFSET * dtime_tz_t::OFFSET_MICROS);
		micros = (micros % Interval::MICROS_PER_DAY + Interval::MICROS_PER_DAY) % Interval::MICROS_PER_DAY;
		target = dtime_tz_t(dtime_t(micros), 0);
	} else if (source < 0) {
		target = Value::MinimumValue(LogicalTypeId::TIME_TZ).GetValue<dtime_tz_t>();
	} else {
		target = Value::MaximumValue(LogicalTypeId::TIME_TZ).GetValue<dtime_tz_t>();
	}

	return decoded;
}

template <>
double ApproxQuantileCoding::Encode(const timestamp_t &input) {
	return Encode<int64_t, double>(input.value);
}

template <>
bool ApproxQuantileCoding::Decode(const double &source, timestamp_t &target) {
	int64_t value;
	const auto decoded = Decode<double, int64_t>(source, value);
	// the infinities are the largest and smallest timestamps - do not return anything beyond them
	if (value >= timestamp_t::infinity().value) {
		target = timestamp_t::infinity();
	} else if (value <= timestamp_t::ninfinity().value) {
		target = timestamp_t::ninfinity();
	} else {
		target = timestamp_t(value);
	}
	return decoded;
}

struct ApproximateQuantileBindData : public FunctionData {
	ApproximateQuantileBindData() {
	}
	explicit ApproximateQuantileBindData(float quantile_p) : quantiles(1, quantile_p) {
	}

	explicit ApproximateQuantileBindData(vector<float> quantiles_p) : quantiles(std::move(quantiles_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ApproximateQuantileBindData>(quantiles);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ApproximateQuantileBindData>();
		//		return quantiles == other.quantiles;
		if (quantiles != other.quantiles) {
			return false;
		}
		return true;
	}

	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
	                      const BoundAggregateFunction &function) {
		auto &bind_data = bind_data_p->Cast<ApproximateQuantileBindData>();
		serializer.WriteProperty(100, "quantiles", bind_data.quantiles);
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto result = make_uniq<ApproximateQuantileBindData>();
		deserializer.ReadProperty(100, "quantiles", result->quantiles);
		return std::move(result);
	}

	vector<float> quantiles;
};

struct ApproxQuantileOperation {
	using SAVE_TYPE = duckdb_tdigest::Value;

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		auto val = ApproxQuantileCoding::template Encode<INPUT_TYPE, SAVE_TYPE>(input);
		if (!Value::DoubleIsFinite(val)) {
			return;
		}
		if (!state.h) {
			// the digest and its buffers live in the aggregate's arena, so an aborted query does not leak them
			auto &allocator = unary_input.input.allocator;
			state.h = allocator.Make<duckdb_tdigest::TDigest>(allocator, 100);
		}
		state.h->add(val);
		state.pos++;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		if (source.pos == 0) {
			return;
		}
		D_ASSERT(source.h);
		auto &allocator = aggr_input_data.allocator;
		if (!target.h) {
			target.h = allocator.Make<duckdb_tdigest::TDigest>(allocator, 100);
		} else if (&target.h->getAllocator() != &allocator) {
			// Copied states can share a sink arena across partitions being combined in parallel.
			target.h = allocator.Make<duckdb_tdigest::TDigest>(allocator, *target.h);
		}
		if (!TryAddOperator::Operation(target.pos, source.pos, target.pos)) {
			throw OutOfRangeException("Overflow in aggregate state combine");
		}
		target.h->merge(source.h);
	}

	static bool IgnoreNull() {
		return true;
	}
};

struct ApproxQuantileScalarOperation : public ApproxQuantileOperation {
	template <class TARGET_TYPE, class STATE>
	static void Finalize(STATE &state, TARGET_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (state.pos == 0) {
			finalize_data.ReturnNull();
			return;
		}
		D_ASSERT(state.h);
		D_ASSERT(finalize_data.input.bind_data);
		state.h->compress();
		auto &bind_data = finalize_data.input.bind_data->template Cast<ApproximateQuantileBindData>();
		D_ASSERT(bind_data.quantiles.size() == 1);
		const auto source = state.h->quantile(bind_data.quantiles[0]);
		ApproxQuantileCoding::Decode(source, target, finalize_data.result.GetType());
	}
};

//===--------------------------------------------------------------------===//
// State Export
//===--------------------------------------------------------------------===//
//! Exported state: STRUCT(count, min, max, centroids) - the value count, exact min/max and the t-digest centroids.
LogicalType ApproxQuantileExportType() {
	child_list_t<LogicalType> centroid_children;
	centroid_children.emplace_back("mean", LogicalType::DOUBLE);
	centroid_children.emplace_back("weight", LogicalType::DOUBLE);

	child_list_t<LogicalType> children;
	children.emplace_back("count", LogicalType::UBIGINT);
	children.emplace_back("min", LogicalType::DOUBLE);
	children.emplace_back("max", LogicalType::DOUBLE);
	children.emplace_back("centroids", LogicalType::LIST(LogicalType::STRUCT(std::move(centroid_children))));
	return LogicalType::STRUCT(std::move(children));
}

//! Rebuilds the quantile parameter (e.g. 0.5 or [0.25, 0.75]) from the bind data so re-binding can supply it.
//! param_type is the declared type of the quantile argument.
Value ApproxQuantileParameterValue(const ApproximateQuantileBindData &bind_data, const LogicalType &param_type) {
	vector<Value> quantiles;
	for (auto &q : bind_data.quantiles) {
		quantiles.push_back(Value::FLOAT(q));
	}
	if (param_type.id() != LogicalTypeId::LIST && param_type.id() != LogicalTypeId::ARRAY) {
		D_ASSERT(quantiles.size() == 1);
		return quantiles[0];
	}
	return Value::LIST(LogicalType::FLOAT, std::move(quantiles));
}

AggregateStateLayout ApproxQuantileGetStateType(AggregateLayoutInput &input) {
	auto &function = input.function;
	AggregateStateLayout layout;
	layout.type = ApproxQuantileExportType();
	layout.total_state_size = AlignValue<idx_t>(sizeof(ApproxQuantileState));
	if (input.bind_data && function.GetArguments().size() == 2) {
		// the quantile parameter must be a constant at bind time (BindApproxQuantile folds it into the bind data) -
		// record its value so that re-binding the exported state can supply it and reconstruct the bind data
		auto &bind_data = input.bind_data->Cast<ApproximateQuantileBindData>();
		layout.constant_parameters.emplace(1, ApproxQuantileParameterValue(bind_data, function.GetArguments()[1]));
	}
	return layout;
}

//! The shape of the exported state: STRUCT(count, min, max, centroids LIST(STRUCT(mean, weight)))
using APPROX_QUANTILE_EXPORT_TYPE =
    VectorStructType<uint64_t, double, double, VectorListType<VectorStructType<double, double>>>;

void ApproxQuantileExportState(Vector &state_vector, AggregateFinalizeInputData &aggr_input_data, Vector &result,
                               idx_t count, idx_t offset) {
	auto states = state_vector.Values<ApproxQuantileState *>();
	auto writer = FlatVector::Writer<APPROX_QUANTILE_EXPORT_TYPE>(result, count, offset);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[i].GetValue();
		if (!state.h || state.pos == 0) {
			// no values have been added to this state - export NULL
			writer.WriteNull();
			continue;
		}
		// fold any unprocessed values into the centroids
		state.h->compress();
		writer.WriteValue([&](auto &count_writer, auto &min_writer, auto &max_writer, auto &centroids_writer) {
			count_writer.WriteValue(state.pos);
			min_writer.WriteValue(state.h->min());
			max_writer.WriteValue(state.h->max());
			auto &centroids = state.h->processed();
			idx_t centroid_idx = 0;
			for (auto &centroid_writer : centroids_writer.WriteList(centroids.size())) {
				auto &centroid = centroids[centroid_idx++];
				centroid_writer.WriteValue([&](auto &mean_writer, auto &weight_writer) {
					mean_writer.WriteValue(centroid.mean());
					weight_writer.WriteValue(centroid.weight());
				});
			}
		});
	}
}

void ApproxQuantileImportState(AggregateImportInputData &input) {
	const auto &layout = input.layout;
	const auto &input_vec = input.input_vec;
	const auto count = input_vec.size();
	const auto dest_buffer = input.dest_buffer;
	auto entries = input_vec.Values<APPROX_QUANTILE_EXPORT_TYPE>();
	for (idx_t i = 0; i < count; i++) {
		auto &state = *reinterpret_cast<ApproxQuantileState *>(dest_buffer + i * layout.total_state_size);
		state.h = nullptr;
		state.pos = 0;
		const auto entry = entries[i];
		if (!entry.IsValid()) {
			// NULL input - leave the state empty
			continue;
		}
		const auto count_entry = entry.template GetChildValue<0>();
		const auto min_entry = entry.template GetChildValue<1>();
		const auto max_entry = entry.template GetChildValue<2>();
		const auto centroid_list = entry.template GetChildValue<3>();
		if (!count_entry.IsValid() || !min_entry.IsValid() || !max_entry.IsValid() || !centroid_list.IsValid()) {
			throw InvalidInputException("Invalid approx_quantile state - the state fields cannot be NULL");
		}
		if (count_entry.GetValue() != 0 && centroid_list.GetListLength() == 0) {
			throw InvalidInputException(
			    "Invalid approx_quantile state - non-zero count requires at least one centroid");
		}
		arena_vector<duckdb_tdigest::Centroid> centroids(input.allocator);
		centroids.reserve(centroid_list.GetListLength());
		double total_weight = 0;
		for (const auto centroid_entry : centroid_list.GetChildValues()) {
			const auto mean_entry = centroid_entry.template GetChildValue<0>();
			const auto weight_entry = centroid_entry.template GetChildValue<1>();
			if (!centroid_entry.IsValid() || !mean_entry.IsValid() || !weight_entry.IsValid()) {
				throw InvalidInputException("Invalid approx_quantile state - the centroids cannot be NULL");
			}
			const auto mean = mean_entry.GetValue();
			const auto weight = weight_entry.GetValue();
			if (Value::IsNan(mean) || !Value::IsFinite(weight) || weight <= 0) {
				throw InvalidInputException(
				    "Invalid approx_quantile state - centroids must have a mean and a positive finite weight");
			}
			if (!centroids.empty() && mean < centroids.back().mean()) {
				throw InvalidInputException("Invalid approx_quantile state - centroids must be sorted by their mean");
			}
			total_weight += weight;
			centroids.emplace_back(mean, weight);
		}
		if (!Value::IsFinite(total_weight)) {
			throw InvalidInputException("Invalid approx_quantile state - the total centroid weight must be finite");
		}
		if (Value::IsNan(min_entry.GetValue()) || Value::IsNan(max_entry.GetValue())) {
			throw InvalidInputException("Invalid approx_quantile state - min and max cannot be NaN");
		}
		auto digest = input.allocator.Make<duckdb_tdigest::TDigest>(
		    std::move(centroids), duckdb::arena_vector<duckdb_tdigest::Centroid>(input.allocator), 100, 0, 0);
		digest->setMinMax(min_entry.GetValue(), max_entry.GetValue());
		state.pos = count_entry.GetValue();
		state.h = digest;
	}
}

AggregateFunction GetApproximateQuantileAggregateFunction(const LogicalType &type) {
	//	Not binary comparable
	if (type == LogicalType::TIME_TZ) {
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, dtime_tz_t, dtime_tz_t,
		                                         ApproxQuantileScalarOperation>(type, type);
	}
	if (type == LogicalType::TIME) {
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, dtime_t, dtime_t, ApproxQuantileScalarOperation>(
		    type, type);
	}
	if (type.id() == LogicalTypeId::TIMESTAMP || type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, timestamp_t, timestamp_t,
		                                         ApproxQuantileScalarOperation>(type, type);
	}
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, int8_t, int8_t, ApproxQuantileScalarOperation>(
		    type, type);
	case PhysicalType::INT16:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, int16_t, int16_t, ApproxQuantileScalarOperation>(
		    type, type);
	case PhysicalType::INT32:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, int32_t, int32_t, ApproxQuantileScalarOperation>(
		    type, type);
	case PhysicalType::INT64:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, int64_t, int64_t, ApproxQuantileScalarOperation>(
		    type, type);
	case PhysicalType::INT128:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, hugeint_t, hugeint_t,
		                                         ApproxQuantileScalarOperation>(type, type);
	case PhysicalType::FLOAT:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, float, float, ApproxQuantileScalarOperation>(
		    type, type);
	case PhysicalType::DOUBLE:
		return AggregateFunction::UnaryAggregate<ApproxQuantileState, double, double, ApproxQuantileScalarOperation>(
		    type, type);
	default:
		throw InternalException("Unimplemented quantile aggregate");
	}
}

AggregateFunction GetApproximateQuantileDecimalAggregateFunction(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
		// keep the DECIMAL type as the input and return type so that the width and scale survive the bind
		return GetApproximateQuantileAggregateFunction(type);
	default:
		throw InternalException("Unimplemented quantile decimal aggregate");
	}
}

float CheckApproxQuantile(const Value &quantile_val) {
	if (quantile_val.IsNull()) {
		throw BinderException("APPROXIMATE QUANTILE parameter cannot be NULL");
	}
	auto quantile = quantile_val.GetValue<float>();
	if (Value::IsNan(quantile)) {
		throw BinderException("APPROXIMATE QUANTILE parameter cannot be NaN");
	}
	if (quantile < 0 || quantile > 1) {
		throw BinderException("APPROXIMATE QUANTILE can only take parameters in range [0, 1]");
	}

	return quantile;
}

//! Binds the quantile parameter into the bind data. It stays part of the expression tree, and the aggregate is
//! handed it along with the input - the update callbacks only consume the leading input argument
unique_ptr<FunctionData> BindApproxQuantile(BindAggregateFunctionInput &input) {
	auto quantile_val = input.GetNonNullConstant(1);

	vector<float> quantiles;
	switch (quantile_val.type().id()) {
	case LogicalTypeId::LIST:
		for (const auto &element_val : ListValue::GetChildren(quantile_val)) {
			quantiles.push_back(CheckApproxQuantile(element_val));
		}
		break;
	case LogicalTypeId::ARRAY:
		for (const auto &element_val : ArrayValue::GetChildren(quantile_val)) {
			quantiles.push_back(CheckApproxQuantile(element_val));
		}
		break;
	default:
		quantiles.push_back(CheckApproxQuantile(quantile_val));
		break;
	}

	return make_uniq<ApproximateQuantileBindData>(quantiles);
}

AggregateFunction ApproxQuantileDecimalFunction(const LogicalType &type) {
	auto function = GetApproximateQuantileDecimalAggregateFunction(type);
	function.SetName("approx_quantile");
	function.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	function.SetDeserializeCallback(ApproximateQuantileBindData::Deserialize);
	function.SetStateExportCallbacks(ApproxQuantileGetStateType, ApproxQuantileExportState, ApproxQuantileImportState);
	return function;
}

//! Specialises the (stub) DECIMAL function to the implementation over the DECIMAL's physical type. The implementation
//! only declares the input argument, so the quantile argument that BindApproxQuantile folded into the bind data is
//! restored afterwards.
void ReplaceApproxQuantileDecimal(BoundAggregateFunction &function, const AggregateFunction &implementation) {
	auto declared_arguments = function.GetArguments();
	function.ReplaceImplementation(implementation);
	for (idx_t i = function.GetArguments().size(); i < declared_arguments.size(); i++) {
		function.GetArguments().push_back(declared_arguments[i]);
	}
}

unique_ptr<FunctionData> BindApproxQuantileDecimal(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto decimal_type = arguments[0]->GetReturnType();
	auto bind_data = BindApproxQuantile(input);
	ReplaceApproxQuantileDecimal(function, ApproxQuantileDecimalFunction(decimal_type));
	return bind_data;
}

AggregateFunction GetApproximateQuantileAggregate(const LogicalType &type) {
	auto fun = GetApproximateQuantileAggregateFunction(type);
	fun.SetBindCallback(BindApproxQuantile);
	fun.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ApproximateQuantileBindData::Deserialize);
	fun.SetStateExportCallbacks(ApproxQuantileGetStateType, ApproxQuantileExportState, ApproxQuantileImportState);
	// temporarily push an argument so we can bind the actual quantile
	fun.GetSignature().GetParameter(0).SetName("x");
	fun.GetSignature().AddParameter("pos", LogicalType::FLOAT);
	return fun;
}

template <class CHILD_TYPE>
struct ApproxQuantileListOperation : public ApproxQuantileOperation {
	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (state.pos == 0) {
			finalize_data.ReturnNull();
			return;
		}

		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->template Cast<ApproximateQuantileBindData>();

		auto &result = ListVector::GetChildMutable(finalize_data.result);
		auto ridx = ListVector::GetListSize(finalize_data.result);
		ListVector::Reserve(finalize_data.result, ridx + bind_data.quantiles.size());
		auto rdata = FlatVector::GetDataMutable<CHILD_TYPE>(result);

		D_ASSERT(state.h);
		state.h->compress();

		auto &entry = target;
		entry.offset = ridx;
		entry.length = bind_data.quantiles.size();
		for (size_t q = 0; q < entry.length; ++q) {
			const auto &quantile = bind_data.quantiles[q];
			const auto &source = state.h->quantile(quantile);
			auto &target = rdata[ridx + q];
			ApproxQuantileCoding::Decode(source, target, result.GetType());
		}

		ListVector::SetListSize(finalize_data.result, entry.offset + entry.length);
	}
};

template <class STATE, class INPUT_TYPE, class RESULT_TYPE, class OP>
AggregateFunction ApproxQuantileListAggregate(const LogicalType &input_type, const LogicalType &child_type) {
	LogicalType result_type = LogicalType::LIST(child_type);
	return AggregateFunction(
	    {input_type}, result_type, AggregateFunction::StateSize<STATE>, AggregateFunction::StateInitialize<STATE, OP>,
	    AggregateFunction::UnaryScatterUpdate<STATE, INPUT_TYPE, OP>, AggregateFunction::StateCombine<STATE, OP>,
	    AggregateFunction::StateFinalize<STATE, RESULT_TYPE, OP>, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	    AggregateFunction::NoClusterUpdate(), AggregateFunction::NoBind());
}

template <typename INPUT_TYPE, typename SAVE_TYPE>
AggregateFunction GetTypedApproxQuantileListAggregateFunction(const LogicalType &type) {
	using STATE = ApproxQuantileState;
	using OP = ApproxQuantileListOperation<INPUT_TYPE>;
	auto fun = ApproxQuantileListAggregate<STATE, INPUT_TYPE, list_entry_t, OP>(type, type);
	fun.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ApproximateQuantileBindData::Deserialize);
	return fun;
}

AggregateFunction GetApproxQuantileListAggregateFunction(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return GetTypedApproxQuantileListAggregateFunction<int8_t, int8_t>(type);
	case LogicalTypeId::SMALLINT:
		return GetTypedApproxQuantileListAggregateFunction<int16_t, int16_t>(type);
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::DATE:
		return GetTypedApproxQuantileListAggregateFunction<int32_t, int32_t>(type);
	case LogicalTypeId::BIGINT:
		return GetTypedApproxQuantileListAggregateFunction<int64_t, int64_t>(type);
	case LogicalTypeId::TIME:
		return GetTypedApproxQuantileListAggregateFunction<dtime_t, dtime_t>(type);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return GetTypedApproxQuantileListAggregateFunction<timestamp_t, timestamp_t>(type);
	case LogicalTypeId::TIME_TZ:
		//	Not binary comparable
		return GetTypedApproxQuantileListAggregateFunction<dtime_tz_t, dtime_tz_t>(type);
	case LogicalTypeId::HUGEINT:
		return GetTypedApproxQuantileListAggregateFunction<hugeint_t, hugeint_t>(type);
	case LogicalTypeId::FLOAT:
		return GetTypedApproxQuantileListAggregateFunction<float, float>(type);
	case LogicalTypeId::DOUBLE:
		return GetTypedApproxQuantileListAggregateFunction<double, double>(type);
	case LogicalTypeId::DECIMAL:
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return GetTypedApproxQuantileListAggregateFunction<int16_t, int16_t>(type);
		case PhysicalType::INT32:
			return GetTypedApproxQuantileListAggregateFunction<int32_t, int32_t>(type);
		case PhysicalType::INT64:
			return GetTypedApproxQuantileListAggregateFunction<int64_t, int64_t>(type);
		case PhysicalType::INT128:
			return GetTypedApproxQuantileListAggregateFunction<hugeint_t, hugeint_t>(type);
		default:
			throw NotImplementedException("Unimplemented approximate quantile list decimal aggregate");
		}
	default:
		throw NotImplementedException("Unimplemented approximate quantile list aggregate");
	}
}

AggregateFunction ApproxQuantileDecimalListFunction(const LogicalType &type) {
	auto function = GetApproxQuantileListAggregateFunction(type);
	function.SetName("approx_quantile");
	function.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	function.SetDeserializeCallback(ApproximateQuantileBindData::Deserialize);
	function.SetStateExportCallbacks(ApproxQuantileGetStateType, ApproxQuantileExportState, ApproxQuantileImportState);
	return function;
}

unique_ptr<FunctionData> BindApproxQuantileDecimalList(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto decimal_type = arguments[0]->GetReturnType();
	auto bind_data = BindApproxQuantile(input);
	ReplaceApproxQuantileDecimal(function, ApproxQuantileDecimalListFunction(decimal_type));
	return bind_data;
}

AggregateFunction GetApproxQuantileListAggregate(const LogicalType &type) {
	auto fun = GetApproxQuantileListAggregateFunction(type);
	fun.SetBindCallback(BindApproxQuantile);
	fun.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ApproximateQuantileBindData::Deserialize);
	fun.SetStateExportCallbacks(ApproxQuantileGetStateType, ApproxQuantileExportState, ApproxQuantileImportState);
	// temporarily push an argument so we can bind the actual quantile
	fun.GetSignature().GetParameter(0).SetName("x");
	auto list_of_float = LogicalType::LIST(LogicalType::FLOAT);
	fun.GetSignature().AddParameter("pos", list_of_float);
	return fun;
}

unique_ptr<FunctionData> ApproxQuantileDecimalDeserialize(Deserializer &deserializer,
                                                          BoundAggregateFunction &function) {
	auto bind_data = ApproximateQuantileBindData::Deserialize(deserializer, function);
	auto &return_type = deserializer.Get<const LogicalType &>();
	if (return_type.id() == LogicalTypeId::LIST) {
		ReplaceApproxQuantileDecimal(function, ApproxQuantileDecimalListFunction(function.GetArguments()[0]));
	} else {
		ReplaceApproxQuantileDecimal(function, ApproxQuantileDecimalFunction(function.GetArguments()[0]));
	}
	return bind_data;
}

AggregateFunction GetApproxQuantileDecimal() {
	// stub function - the actual function is set during bind or deserialize
	AggregateFunction fun({}, LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      BindApproxQuantileDecimal);
	fun.GetSignature().AddParameter("x", LogicalTypeId::DECIMAL).AddParameter("pos", LogicalType::FLOAT);
	fun.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ApproxQuantileDecimalDeserialize);
	return fun;
}

AggregateFunction GetApproxQuantileDecimalList() {
	// stub function - the actual function is set during bind or deserialize
	AggregateFunction fun({}, LogicalType::LIST(LogicalTypeId::DECIMAL), nullptr, nullptr, nullptr, nullptr, nullptr,
	                      nullptr, BindApproxQuantileDecimalList);
	fun.GetSignature()
	    .AddParameter("x", LogicalTypeId::DECIMAL)
	    .AddParameter("pos", LogicalType::LIST(LogicalType::FLOAT));
	fun.SetSerializeCallback(ApproximateQuantileBindData::Serialize);
	fun.SetDeserializeCallback(ApproxQuantileDecimalDeserialize);
	return fun;
}
} // namespace

AggregateFunctionSet ApproxQuantileFun::GetFunctions() {
	AggregateFunctionSet approx_quantile;
	approx_quantile.AddFunction(GetApproxQuantileDecimal());

	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::SMALLINT));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::INTEGER));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::BIGINT));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::HUGEINT));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::DOUBLE));

	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::DATE));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIME));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIME_NS));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIME_TZ));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIMESTAMP));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIMESTAMP_NS));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIMESTAMP_TZ));
	approx_quantile.AddFunction(GetApproximateQuantileAggregate(LogicalType::TIMESTAMP_TZ_NS));

	// List variants
	approx_quantile.AddFunction(GetApproxQuantileDecimalList());

	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::TINYINT));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::SMALLINT));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::INTEGER));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::BIGINT));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::HUGEINT));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::FLOAT));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalTypeId::DOUBLE));

	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalType::DATE));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalType::TIME));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalType::TIME_TZ));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalType::TIMESTAMP));
	approx_quantile.AddFunction(GetApproxQuantileListAggregate(LogicalType::TIMESTAMP_TZ));

	return approx_quantile;
}

} // namespace duckdb
