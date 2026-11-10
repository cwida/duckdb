#include "core_functions/aggregate/holistic_functions.hpp"
#include "core_functions/aggregate/quantile_state.hpp"
#include "duckdb/common/enums/quantile_enum.hpp"
#include "duckdb/common/operator/abs.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate/list_aggregate.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/planner/expression.hpp"

#include <cmath>

namespace duckdb {

// Descending order is encoded as a negative quantile parameter. A zero fraction
// is stored as negative zero, so use the sign bit to detect that case as well.
static bool QuantileDescending(const Value &q) {
	return std::signbit(q.GetValue<double>());
}

template <class INPUT_TYPE>
struct IndirectLess {
	inline explicit IndirectLess(const INPUT_TYPE *inputs_p) : inputs(inputs_p) {
	}

	inline bool operator()(const idx_t &lhi, const idx_t &rhi) const {
		return inputs[lhi] < inputs[rhi];
	}

	const INPUT_TYPE *inputs;
};

template <typename T>
static inline T QuantileAbs(const T &t) {
	return AbsOperator::Operation<T, T>(t);
}

template <>
inline Value QuantileAbs(const Value &v) {
	const auto &type = v.type();
	switch (type.id()) {
	case LogicalTypeId::DECIMAL: {
		const auto integral = IntegralValue::Get(v);
		const auto width = DecimalType::GetWidth(type);
		const auto scale = DecimalType::GetScale(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return Value::DECIMAL(QuantileAbs<int16_t>(Cast::Operation<hugeint_t, int16_t>(integral)), width, scale);
		case PhysicalType::INT32:
			return Value::DECIMAL(QuantileAbs<int32_t>(Cast::Operation<hugeint_t, int32_t>(integral)), width, scale);
		case PhysicalType::INT64:
			return Value::DECIMAL(QuantileAbs<int64_t>(Cast::Operation<hugeint_t, int64_t>(integral)), width, scale);
		case PhysicalType::INT128:
			return Value::DECIMAL(QuantileAbs<hugeint_t>(integral), width, scale);
		default:
			throw InternalException("Unknown DECIMAL type");
		}
	}
	default:
		return Value::DOUBLE(QuantileAbs<double>(v.GetValue<double>()));
	}
}

//===--------------------------------------------------------------------===//
// Quantile Bind Data
//===--------------------------------------------------------------------===//
QuantileBindData::QuantileBindData() {
}

QuantileBindData::QuantileBindData(const Value &quantile_p)
    : quantiles(1, QuantileValue(QuantileAbs(quantile_p))), order(1, 0), desc(QuantileDescending(quantile_p)) {
}

QuantileBindData::QuantileBindData(const vector<Value> &quantiles_p) {
	vector<Value> normalised;
	size_t pos = 0;
	size_t neg = 0;
	for (idx_t i = 0; i < quantiles_p.size(); ++i) {
		const auto &q = quantiles_p[i];
		pos += (q > 0);
		neg += QuantileDescending(q);
		normalised.push_back(QuantileAbs(q));
		order.push_back(i);
	}
	if (pos && neg) {
		throw BinderException("QUANTILE parameters must have consistent signs");
	}
	desc = (neg > 0);

	IndirectLess<Value> lt(normalised.data());
	std::sort(order.begin(), order.end(), lt);

	for (const auto &q : normalised) {
		quantiles.emplace_back(q);
	}
}

QuantileBindData::QuantileBindData(const QuantileBindData &other)
    : FunctionData(other), order(other.order), desc(other.desc) {
	for (const auto &q : other.quantiles) {
		quantiles.emplace_back(q);
	}
}

unique_ptr<FunctionData> QuantileBindData::Copy() const {
	return make_uniq<QuantileBindData>(*this);
}

bool QuantileBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<QuantileBindData>();
	return desc == other.desc && quantiles == other.quantiles && order == other.order;
}

void QuantileBindData::Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                                 const BoundAggregateFunction &function) {
	auto &bind_data = bind_data_p->Cast<QuantileBindData>();
	vector<Value> raw;
	for (const auto &q : bind_data.quantiles) {
		raw.emplace_back(q.val);
	}
	serializer.WriteProperty(100, "quantiles", raw);
	serializer.WriteProperty(101, "order", bind_data.order);
	serializer.WriteProperty(102, "desc", bind_data.desc);
}

unique_ptr<FunctionData> QuantileBindData::Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
	auto result = make_uniq<QuantileBindData>();
	vector<Value> raw;
	deserializer.ReadProperty(100, "quantiles", raw);
	deserializer.ReadProperty(101, "order", result->order);
	deserializer.ReadProperty(102, "desc", result->desc);
	QuantileSerializationType deserialization_type;
	deserializer.ReadPropertyWithExplicitDefault(103, "quantile_type", deserialization_type,
	                                             QuantileSerializationType::NON_DECIMAL);

	if (deserialization_type != QuantileSerializationType::NON_DECIMAL) {
		deserializer.ReadDeletedProperty<LogicalType>(104, "logical_type");
	}

	for (const auto &r : raw) {
		result->quantiles.emplace_back(r);
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Quantile Casts
//===--------------------------------------------------------------------===//
template <>
interval_t QuantileCast::Operation(const dtime_t &src, Vector &result) {
	return {0, 0, src.value};
}

template <>
string_t QuantileCast::Operation(const string_t &src, Vector &result) {
	return StringVector::AddStringOrBlob(result, src);
}

//===--------------------------------------------------------------------===//
// Scalar Quantile
//===--------------------------------------------------------------------===//
template <bool DISCRETE>
struct QuantileScalarOperation : public QuantileOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.linked_list.total_capacity == 0) {
			finalize_data.ReturnNull();
			return;
		}
		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->Cast<QuantileBindData>();
		D_ASSERT(bind_data.quantiles.size() == 1);
		auto &flattened = FlattenedQuantileValues<typename STATE::InputType>::Flatten(finalize_data, state.linked_list);
		QuantileInterpolator<DISCRETE> interp(bind_data.quantiles[0], state.linked_list.total_capacity, bind_data.desc);
		target = interp.template Operation<typename STATE::InputType, T>(flattened.Data(), finalize_data.result);
	}

	template <class STATE, class INPUT_TYPE, class RESULT_TYPE>
	static void Window(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
	                   const_data_ptr_t g_state, data_ptr_t l_state, const SubFrames *subframes_per_row, idx_t count,
	                   Vector &result, idx_t row_idx) {
		auto &state = *reinterpret_cast<STATE *>(l_state);
		auto gstate = reinterpret_cast<const STATE *>(g_state);

		auto &data = state.GetOrCreateWindowCursor(partition);
		const auto &fmask = partition.filter_mask;

		QuantileIncluded<INPUT_TYPE> included(fmask, data);

		D_ASSERT(aggr_input_data.bind_data);
		auto &bind_data = aggr_input_data.bind_data->Cast<QuantileBindData>();

		auto rdata = FlatVector::GetDataMutable<RESULT_TYPE>(result);
		auto &rmask = FlatVector::ValidityMutable(result);

		const auto &quantile = bind_data.quantiles[0];
		if (gstate && gstate->HasTree()) {
			for (idx_t ridx = 0; ridx < count; ++ridx) {
				const auto &frames = subframes_per_row[ridx];
				const auto n = FrameSize(included, frames);
				if (!n) {
					rmask.Set(ridx, false);
					continue;
				}

				rdata[ridx] = gstate->GetWindowState().template WindowScalar<RESULT_TYPE, DISCRETE>(
				    partition.context.client, data, frames, n, result, quantile);
			}
		} else {
			auto &window_state = state.GetOrCreateWindowState();

			for (idx_t ridx = 0; ridx < count; ++ridx) {
				const auto &frames = subframes_per_row[ridx];
				const auto n = FrameSize(included, frames);
				if (!n) {
					rmask.Set(ridx, false);
					continue;
				}

				//	Update the skip list
				window_state.UpdateSkip(data, frames, included);

				// Find the position(s) needed
				rdata[ridx] = window_state.template WindowScalar<RESULT_TYPE, DISCRETE>(partition.context.client, data,
				                                                                        frames, n, result, quantile);

				//	Save the previous state for next time
				window_state.prevs = frames;
			}
		}
	}
};

//! Buffers the sort key of each input row in the state's linked list - NULL inputs are skipped
//! The quantile parameter is folded into the bind data by the bind, but stays part of the expression tree - only the
//! leading input argument is consumed
static void QuantileSortKeyUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                                  Vector &states, idx_t count) {
	D_ASSERT(input_count >= 1);
	Vector sort_keys(LogicalType::BLOB);
	const OrderModifiers modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	CreateSortKeyHelpers::CreateSortKeyWithValidity(inputs[0], sort_keys, modifiers, count);
	ListUpdateFunction<true>(&sort_keys, aggr_input_data, 1, states, count);
}

struct QuantileScalarFallback : QuantileOperation {
	//! The fallback buffers sort keys instead of the input values
	static LogicalType GetElementType(AggregateInputData &) {
		return LogicalType::BLOB;
	}

	template <class STATE>
	static void Finalize(STATE &state, AggregateFinalizeData &finalize_data) {
		if (state.linked_list.total_capacity == 0) {
			finalize_data.ReturnNull();
			return;
		}
		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->Cast<QuantileBindData>();
		D_ASSERT(bind_data.quantiles.size() == 1);
		auto &flattened = FlattenedQuantileValues<string_t>::Flatten(finalize_data, state.linked_list);
		QuantileInterpolator<true> interp(bind_data.quantiles[0], state.linked_list.total_capacity, bind_data.desc);
		auto interpolation_result = interp.InterpolateInternal<string_t>(flattened.Data());
		CreateSortKeyHelpers::DecodeSortKey(interpolation_result, finalize_data.result, finalize_data.result_idx,
		                                    OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
	}
};

//===--------------------------------------------------------------------===//
// Quantile List
//===--------------------------------------------------------------------===//
template <class CHILD_TYPE, bool DISCRETE>
struct QuantileListOperation : QuantileOperation {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.linked_list.total_capacity == 0) {
			finalize_data.ReturnNull();
			return;
		}

		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->Cast<QuantileBindData>();

		auto &result = ListVector::GetChildMutable(finalize_data.result);
		auto ridx = ListVector::GetListSize(finalize_data.result);
		ListVector::Reserve(finalize_data.result, ridx + bind_data.quantiles.size());
		auto rdata = FlatVector::GetDataMutable<CHILD_TYPE>(result);

		auto &flattened = FlattenedQuantileValues<typename STATE::InputType>::Flatten(finalize_data, state.linked_list);
		auto v_t = flattened.Data();
		D_ASSERT(v_t);

		auto &entry = target;
		entry.offset = ridx;
		idx_t lower = 0;
		for (const auto &q : bind_data.order) {
			const auto &quantile = bind_data.quantiles[q];
			QuantileInterpolator<DISCRETE> interp(quantile, state.linked_list.total_capacity, bind_data.desc);
			interp.begin = lower;
			rdata[ridx + q] = interp.template Operation<typename STATE::InputType, CHILD_TYPE>(v_t, result);
			lower = interp.FRN;
		}
		entry.length = bind_data.quantiles.size();

		ListVector::SetListSize(finalize_data.result, entry.offset + entry.length);
	}

	template <class STATE, class INPUT_TYPE, class RESULT_TYPE>
	static void Window(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
	                   const_data_ptr_t g_state, data_ptr_t l_state, const SubFrames *subframes_per_row, idx_t count,
	                   Vector &list, idx_t row_idx) {
		auto &state = *reinterpret_cast<STATE *>(l_state);
		auto gstate = reinterpret_cast<const STATE *>(g_state);

		auto &data = state.GetOrCreateWindowCursor(partition);
		const auto &fmask = partition.filter_mask;

		D_ASSERT(aggr_input_data.bind_data);
		auto &bind_data = aggr_input_data.bind_data->Cast<QuantileBindData>();

		QuantileIncluded<INPUT_TYPE> included(fmask, data);

		// Result is a constant LIST<RESULT_TYPE> with a fixed length
		auto &lmask = FlatVector::ValidityMutable(list);

		if (gstate && gstate->HasTree()) {
			for (idx_t lidx = 0; lidx < count; ++lidx) {
				const auto &frames = subframes_per_row[lidx];
				const auto n = FrameSize(included, frames);
				if (!n) {
					lmask.Set(lidx, false);
					continue;
				}

				gstate->GetWindowState().template WindowList<CHILD_TYPE, DISCRETE>(partition.context.client, data,
				                                                                   frames, n, list, lidx, bind_data);
			}
		} else {
			auto &window_state = state.GetOrCreateWindowState();
			for (idx_t lidx = 0; lidx < count; ++lidx) {
				const auto &frames = subframes_per_row[lidx];
				const auto n = FrameSize(included, frames);
				if (!n) {
					lmask.Set(lidx, false);
					continue;
				}

				window_state.UpdateSkip(data, frames, included);
				window_state.template WindowList<CHILD_TYPE, DISCRETE>(partition.context.client, data, frames, n, list,
				                                                       lidx, bind_data);
				window_state.prevs = frames;
			}
		}
	}
};

struct QuantileListFallback : QuantileOperation {
	//! The fallback buffers sort keys instead of the input values
	static LogicalType GetElementType(AggregateInputData &) {
		return LogicalType::BLOB;
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (state.linked_list.total_capacity == 0) {
			finalize_data.ReturnNull();
			return;
		}

		D_ASSERT(finalize_data.input.bind_data);
		auto &bind_data = finalize_data.input.bind_data->Cast<QuantileBindData>();

		auto &result = ListVector::GetChildMutable(finalize_data.result);
		auto ridx = ListVector::GetListSize(finalize_data.result);
		ListVector::Reserve(finalize_data.result, ridx + bind_data.quantiles.size());

		auto &flattened = FlattenedQuantileValues<string_t>::Flatten(finalize_data, state.linked_list);

		auto &entry = target;
		entry.offset = ridx;
		idx_t lower = 0;
		for (const auto &q : bind_data.order) {
			const auto &quantile = bind_data.quantiles[q];
			QuantileInterpolator<true> interp(quantile, state.linked_list.total_capacity, bind_data.desc);
			interp.begin = lower;
			auto interpolation_result = interp.InterpolateInternal<string_t>(flattened.Data());
			CreateSortKeyHelpers::DecodeSortKey(interpolation_result, result, ridx + q,
			                                    OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
			lower = interp.FRN;
		}
		entry.length = bind_data.quantiles.size();

		ListVector::SetListSize(finalize_data.result, entry.offset + entry.length);
	}
};

//===--------------------------------------------------------------------===//
// Discrete Quantiles
//===--------------------------------------------------------------------===//
template <class OP>
AggregateFunction GetDiscreteQuantileTemplated(const LogicalType &type) {
	if (type.id() == LogicalTypeId::TIME_TZ) {
		// TIMETZ is ordered by its normalized time, not by its raw bits
		return OP::template GetFunction<dtime_tz_t>(type);
	}
	switch (type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(quantile_types)
	case PhysicalType::INT8:
		return OP::template GetFunction<int8_t>(type);
	case PhysicalType::INT16:
		return OP::template GetFunction<int16_t>(type);
	case PhysicalType::INT32:
		return OP::template GetFunction<int32_t>(type);
	case PhysicalType::INT64:
		return OP::template GetFunction<int64_t>(type);
	case PhysicalType::INT128:
		return OP::template GetFunction<hugeint_t>(type);
	case PhysicalType::FLOAT:
		return OP::template GetFunction<float>(type);
	case PhysicalType::DOUBLE:
		return OP::template GetFunction<double>(type);
	case PhysicalType::INTERVAL:
		return OP::template GetFunction<interval_t>(type);
	case PhysicalType::VARCHAR:
		return OP::template GetFunction<string_t>(type);
#endif
	default:
		return OP::GetFallback(type);
	}
}

struct ScalarDiscreteQuantile {
	template <typename INPUT_TYPE>
	static AggregateFunction GetFunction(const LogicalType &type) {
		using STATE = QuantileState<INPUT_TYPE>;
		using OP = QuantileScalarOperation<true>;
		auto fun = QuantileBufferingAggregate<STATE, INPUT_TYPE, OP>(type, type);
		fun.SetStructStateExport(QuantileStateLayout<STATE>);
#if !DUCKDB_SMALLER_BINARY(quantile_window)
		fun.SetWindowBatchCallback(OP::Window<STATE, INPUT_TYPE, INPUT_TYPE>);
		fun.SetWindowInitCallback(OP::WindowInit<STATE, INPUT_TYPE>);
#endif
		return fun;
	}

	static AggregateFunction GetFallback(const LogicalType &type) {
		using STATE = QuantileState<string_t>;
		using OP = QuantileScalarFallback;

		AggregateFunction fun({type}, type, AggregateFunction::StateSize<STATE>,
		                      AggregateFunction::StateInitialize<STATE, OP, AggregateDestructorType::LEGACY>,
		                      QuantileSortKeyUpdate, ListCombineFunction<OP>,
		                      AggregateFunction::StateVoidFinalize<STATE, OP>, nullptr, nullptr,
		                      AggregateFunction::StateDestroy<STATE, OP>);
		fun.SetInitLocalStateFinalizeCallback(FlattenedQuantileValues<string_t>::Init);
		fun.SetStructStateExport(QuantileStateLayout<STATE, StateListType<StateSortKey<StateInputType<0>>>>);
		return fun;
	}
};

struct ListDiscreteQuantile {
	template <typename INPUT_TYPE>
	static AggregateFunction GetFunction(const LogicalType &type) {
		using STATE = QuantileState<INPUT_TYPE>;
		using OP = QuantileListOperation<INPUT_TYPE, true>;
		auto fun = QuantileBufferingAggregate<STATE, list_entry_t, OP>(type, LogicalType::LIST(type));
		fun.SetStructStateExport(QuantileStateLayout<STATE>);
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
#if !DUCKDB_SMALLER_BINARY(quantile_window)
		fun.SetWindowBatchCallback(OP::template Window<STATE, INPUT_TYPE, list_entry_t>);
		fun.SetWindowInitCallback(OP::template WindowInit<STATE, INPUT_TYPE>);
#endif
		return fun;
	}

	static AggregateFunction GetFallback(const LogicalType &type) {
		using STATE = QuantileState<string_t>;
		using OP = QuantileListFallback;

		AggregateFunction fun({type}, LogicalType::LIST(type), AggregateFunction::StateSize<STATE>,
		                      AggregateFunction::StateInitialize<STATE, OP, AggregateDestructorType::LEGACY>,
		                      QuantileSortKeyUpdate, ListCombineFunction<OP>,
		                      AggregateFunction::StateFinalize<STATE, list_entry_t, OP>, nullptr, nullptr,
		                      AggregateFunction::StateDestroy<STATE, OP>);
		fun.SetInitLocalStateFinalizeCallback(FlattenedQuantileValues<string_t>::Init);
		fun.SetStructStateExport(QuantileStateLayout<STATE, StateListType<StateSortKey<StateInputType<0>>>>);
		return fun;
	}
};

AggregateFunction GetDiscreteQuantile(const LogicalType &type) {
	auto fun = GetDiscreteQuantileTemplated<ScalarDiscreteQuantile>(type);
	fun.SetStatisticsCallback(AggregateFunction::PropagateInputValueStats);
	return fun;
}

AggregateFunction GetDiscreteQuantileList(const LogicalType &type) {
	return GetDiscreteQuantileTemplated<ListDiscreteQuantile>(type);
}

//===--------------------------------------------------------------------===//
// Continuous Quantiles
//===--------------------------------------------------------------------===//
template <class OP>
AggregateFunction GetContinuousQuantileTemplated(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return OP::template GetFunction<int8_t, double>(type, LogicalType::DOUBLE);
	case LogicalTypeId::SMALLINT:
		return OP::template GetFunction<int16_t, double>(type, LogicalType::DOUBLE);
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::INTEGER:
		return OP::template GetFunction<int32_t, double>(type, LogicalType::DOUBLE);
	case LogicalTypeId::BIGINT:
		return OP::template GetFunction<int64_t, double>(type, LogicalType::DOUBLE);
	case LogicalTypeId::HUGEINT:
		return OP::template GetFunction<hugeint_t, double>(type, LogicalType::DOUBLE);
	case LogicalTypeId::FLOAT:
		return OP::template GetFunction<float, float>(type, type);
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DOUBLE:
		return OP::template GetFunction<double, double>(LogicalType::DOUBLE, LogicalType::DOUBLE);
	case LogicalTypeId::DECIMAL:
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return OP::template GetFunction<int16_t, int16_t>(type, type);
		case PhysicalType::INT32:
			return OP::template GetFunction<int32_t, int32_t>(type, type);
		case PhysicalType::INT64:
			return OP::template GetFunction<int64_t, int64_t>(type, type);
		case PhysicalType::INT128:
			return OP::template GetFunction<hugeint_t, hugeint_t>(type, type);
		default:
			throw NotImplementedException("Unimplemented continuous quantile DECIMAL aggregate");
		}
	case LogicalTypeId::DATE: {
		auto fun = OP::template GetFunction<date_t, timestamp_t>(type, LogicalType::TIMESTAMP);
		fun.SetFallible(); // the TIMESTAMP cast throws for out-of-range dates, interpolation for the infinities
		return fun;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS: {
		auto fun = OP::template GetFunction<timestamp_t, timestamp_t>(type, type);
		fun.SetFallible(); // interpolation throws for the infinities
		return fun;
	}
	case LogicalTypeId::TIME:
		return OP::template GetFunction<dtime_t, dtime_t>(type, type);
	case LogicalTypeId::TIME_TZ:
		// TIMETZ is ordered and interpolated by its normalized time, not by its raw bits
		return OP::template GetFunction<dtime_tz_t, dtime_tz_t>(type, type);
	case LogicalTypeId::INTERVAL: {
		auto fun = OP::template GetFunction<interval_t, interval_t>(type, type);
		fun.SetFallible(); // interpolation converts to microseconds, which throws for extreme intervals
		return fun;
	}
	default:
		throw NotImplementedException("Unimplemented continuous quantile aggregate");
	}
}

struct ScalarContinuousQuantile {
	template <typename INPUT_TYPE, typename TARGET_TYPE>
	static AggregateFunction GetFunction(const LogicalType &input_type, const LogicalType &target_type) {
		using STATE = QuantileState<INPUT_TYPE>;
		using OP = QuantileScalarOperation<false>;
		auto fun = QuantileBufferingAggregate<STATE, TARGET_TYPE, OP>(input_type, target_type);
		fun.SetStructStateExport(QuantileStateLayout<STATE>);
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
#if !DUCKDB_SMALLER_BINARY(quantile_window)
		fun.SetWindowBatchCallback(OP::template Window<STATE, INPUT_TYPE, TARGET_TYPE>);
		fun.SetWindowInitCallback(OP::template WindowInit<STATE, INPUT_TYPE>);
#endif
		return fun;
	}
};

struct ListContinuousQuantile {
	template <typename INPUT_TYPE, typename TARGET_TYPE>
	static AggregateFunction GetFunction(const LogicalType &input_type, const LogicalType &target_type) {
		using STATE = QuantileState<INPUT_TYPE>;
		using OP = QuantileListOperation<TARGET_TYPE, false>;
		auto fun = QuantileBufferingAggregate<STATE, list_entry_t, OP>(input_type, LogicalType::LIST(target_type));
		fun.SetStructStateExport(QuantileStateLayout<STATE>);
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
#if !DUCKDB_SMALLER_BINARY(quantile_window)
		fun.SetWindowBatchCallback(OP::template Window<STATE, INPUT_TYPE, list_entry_t>);
		fun.SetWindowInitCallback(OP::template WindowInit<STATE, INPUT_TYPE>);
#endif
		return fun;
	}
};

AggregateFunction GetContinuousQuantile(const LogicalType &type) {
	auto fun = GetContinuousQuantileTemplated<ScalarContinuousQuantile>(type);
	fun.SetStatisticsCallback(AggregateFunction::PropagateInputValueStats);
	return fun;
}

AggregateFunction GetContinuousQuantileList(const LogicalType &type) {
	return GetContinuousQuantileTemplated<ListContinuousQuantile>(type);
}

//===--------------------------------------------------------------------===//
// Quantile binding
//===--------------------------------------------------------------------===//
static Value CheckQuantile(const Value &quantile_val) {
	if (quantile_val.IsNull()) {
		throw BinderException("QUANTILE parameter cannot be NULL");
	}
	auto quantile = quantile_val.GetValue<double>();
	if (quantile < -1 || quantile > 1) {
		throw BinderException("QUANTILE can only take parameters in the range [-1, 1]");
	}
	if (Value::IsNan(quantile)) {
		throw BinderException("QUANTILE parameter cannot be NaN");
	}

	return quantile_val;
}

//! The exact DECIMAL rank arithmetic of discrete quantiles multiplies the row count by 10^scale with overflow
//! checks - overflowing needs at least 10^(38 - scale) rows, which is reachable for scales above 18. Continuous
//! quantiles compute their rank in DOUBLE and are unaffected.
static bool HasExtremeDecimalQuantile(const QuantileBindData &bind_data) {
	for (auto &quantile : bind_data.quantiles) {
		const auto &quantile_type = quantile.val.type();
		if (quantile_type.id() == LogicalTypeId::DECIMAL && DecimalType::GetScale(quantile_type) > 18) {
			return true;
		}
	}
	return false;
}

//! Binds the quantile parameter into the bind data. It stays part of the expression tree, and the aggregate is
//! handed it along with the input - the update callbacks only consume the leading input argument
unique_ptr<FunctionData> BindQuantile(BindAggregateFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.size() < 2) {
		throw BinderException("QUANTILE requires a range argument between [0, 1]");
	}
	auto quantile_val = input.GetNonNullConstant(1);
	vector<Value> quantiles;
	switch (quantile_val.type().id()) {
	case LogicalTypeId::LIST:
		for (const auto &element_val : ListValue::GetChildren(quantile_val)) {
			quantiles.push_back(CheckQuantile(element_val));
		}
		break;
	case LogicalTypeId::ARRAY:
		for (const auto &element_val : ArrayValue::GetChildren(quantile_val)) {
			quantiles.push_back(CheckQuantile(element_val));
		}
		break;
	default:
		quantiles.push_back(CheckQuantile(quantile_val));
		break;
	}

	return make_uniq<QuantileBindData>(quantiles);
}

//===--------------------------------------------------------------------===//
// Function definitions
//===--------------------------------------------------------------------===//
static bool CanInterpolate(const LogicalType &type) {
	if (type.HasAlias()) {
		return false;
	}
	switch (type.id()) {
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::INTERVAL:
		return true;
	default:
		return false;
	}
}

struct MedianFunction {
	static AggregateFunction GetAggregate(const LogicalType &type) {
		auto fun = CanInterpolate(type) ? GetContinuousQuantile(type) : GetDiscreteQuantile(type);
		fun.SetName("median");
		fun.SetSerializeCallback(QuantileBindData::Serialize);
		fun.SetDeserializeCallback(Deserialize);
		return fun;
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto bind_data = QuantileBindData::Deserialize(deserializer, function);

		auto &input_type = function.GetArguments()[0];
		function.ReplaceImplementation(GetAggregate(input_type));
		return bind_data;
	}

	static unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
		auto &function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		function.ReplaceImplementation(GetAggregate(arguments[0]->GetReturnType()));
		return make_uniq<QuantileBindData>(Value::DECIMAL(int16_t(5), 2, 1));
	}
};

struct DiscreteQuantileListFunction {
	static AggregateFunction GetAggregate(const LogicalType &type) {
		auto fun = GetDiscreteQuantileList(type);
		fun.SetName("quantile_disc");
		fun.SetBindCallback(Bind);
		fun.SetSerializeCallback(QuantileBindData::Serialize);
		fun.SetDeserializeCallback(Deserialize);
		// temporarily push an argument so we can bind the actual quantile
		fun.GetSignature().AddParameter("quantile", LogicalType::LIST(LogicalType::DOUBLE));
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
		return fun;
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto bind_data = QuantileBindData::Deserialize(deserializer, function);

		auto &input_type = function.GetArguments()[0];
		function.ReplaceImplementation(GetAggregate(input_type));
		if (HasExtremeDecimalQuantile(bind_data->Cast<QuantileBindData>())) {
			function.SetFallible();
		}
		return bind_data;
	}

	static unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
		auto &function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		function.ReplaceImplementation(GetAggregate(arguments[0]->GetReturnType()));
		auto bind_data = BindQuantile(input);
		if (HasExtremeDecimalQuantile(bind_data->Cast<QuantileBindData>())) {
			function.SetFallible();
		}
		return bind_data;
	}
};

struct DiscreteQuantileFunction {
	static AggregateFunction GetAggregate(const LogicalType &type) {
		auto fun = GetDiscreteQuantile(type);
		fun.SetName("quantile_disc");
		fun.SetBindCallback(Bind);
		fun.SetSerializeCallback(QuantileBindData::Serialize);
		fun.SetDeserializeCallback(Deserialize);
		// temporarily push an argument so we can bind the actual quantile
		fun.GetSignature().AddParameter("quantile", LogicalType::DOUBLE);
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
		return fun;
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto bind_data = QuantileBindData::Deserialize(deserializer, function);
		auto &quantile_data = bind_data->Cast<QuantileBindData>();

		auto &input_type = function.GetArguments()[0];
		if (quantile_data.quantiles.size() == 1) {
			function.ReplaceImplementation(GetAggregate(input_type));
		} else {
			function.ReplaceImplementation(DiscreteQuantileListFunction::GetAggregate(input_type));
		}
		if (HasExtremeDecimalQuantile(quantile_data)) {
			function.SetFallible();
		}
		return bind_data;
	}

	static unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
		auto &function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		function.ReplaceImplementation(GetAggregate(arguments[0]->GetReturnType()));
		auto bind_data = BindQuantile(input);
		if (HasExtremeDecimalQuantile(bind_data->Cast<QuantileBindData>())) {
			function.SetFallible();
		}
		return bind_data;
	}
};

struct ContinuousQuantileFunction {
	static AggregateFunction GetAggregate(const LogicalType &type) {
		auto fun = GetContinuousQuantile(type);
		fun.SetName("quantile_cont");
		fun.SetBindCallback(Bind);
		fun.SetSerializeCallback(QuantileBindData::Serialize);
		fun.SetDeserializeCallback(Deserialize);
		// temporarily push an argument so we can bind the actual quantile
		fun.GetSignature().AddParameter("quantile", LogicalType::DOUBLE);
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
		return fun;
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto bind_data = QuantileBindData::Deserialize(deserializer, function);

		auto &input_type = function.GetArguments()[0];
		function.ReplaceImplementation(GetAggregate(input_type));
		return bind_data;
	}

	static unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
		auto &function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		auto impl =
		    GetAggregate(function.GetArguments()[0].id() == LogicalTypeId::DECIMAL ? arguments[0]->GetReturnType()
		                                                                           : function.GetArguments()[0]);
		function.ReplaceImplementation(impl);
		return BindQuantile(input);
	}
};

struct ContinuousQuantileListFunction {
	static AggregateFunction GetAggregate(const LogicalType &type) {
		auto fun = GetContinuousQuantileList(type);
		fun.SetName("quantile_cont");
		fun.SetBindCallback(Bind);
		fun.SetSerializeCallback(QuantileBindData::Serialize);
		fun.SetDeserializeCallback(Deserialize);
		// temporarily push an argument so we can bind the actual quantile
		auto list_of_double = LogicalType::LIST(LogicalType::DOUBLE);
		fun.GetSignature().AddParameter("quantile", list_of_double);
		fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		fun.SetIsHolistic(true);
		return fun;
	}

	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
		auto bind_data = QuantileBindData::Deserialize(deserializer, function);

		auto &input_type = function.GetArguments()[0];
		function.ReplaceImplementation(GetAggregate(input_type));
		return bind_data;
	}

	static unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
		auto &function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		auto impl =
		    GetAggregate(function.GetArguments()[0].id() == LogicalTypeId::DECIMAL ? arguments[0]->GetReturnType()
		                                                                           : function.GetArguments()[0]);
		function.ReplaceImplementation(impl);
		return BindQuantile(input);
	}
};

template <class OP>
static AggregateFunction EmptyQuantileFunction(LogicalType input, const LogicalType &result,
                                               const LogicalType &extra_arg) {
	AggregateFunction fun({std::move(input)}, result, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, OP::Bind);
	fun.GetSignature().GetParameter(0).SetName("x");
	if (extra_arg.id() != LogicalTypeId::INVALID) {
		fun.GetSignature().AddParameter("quantile", extra_arg);
	}
	fun.SetSerializeCallback(QuantileBindData::Serialize);
	fun.SetDeserializeCallback(OP::Deserialize);
	fun.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	fun.SetIsHolistic(true);
	return fun;
}

AggregateFunctionSet MedianFun::GetFunctions() {
	AggregateFunctionSet set("median");
	set.AddFunction(EmptyQuantileFunction<MedianFunction>(LogicalType::ANY, LogicalType::ANY, LogicalTypeId::INVALID));
	return set;
}

AggregateFunctionSet QuantileDiscFun::GetFunctions() {
	AggregateFunctionSet set("quantile_disc");
	set.AddFunction(
	    EmptyQuantileFunction<DiscreteQuantileFunction>(LogicalType::ANY, LogicalType::ANY, LogicalType::DOUBLE));
	set.AddFunction(EmptyQuantileFunction<DiscreteQuantileListFunction>(LogicalType::ANY, LogicalType::ANY,
	                                                                    LogicalType::LIST(LogicalType::DOUBLE)));
	// this function is here for deserialization - it cannot be called by users
	set.AddFunction(
	    EmptyQuantileFunction<DiscreteQuantileFunction>(LogicalType::ANY, LogicalType::ANY, LogicalType::INVALID));
	return set;
}

static vector<LogicalType> GetContinuousQuantileTypes() {
	return {LogicalType::TINYINT,   LogicalType::SMALLINT, LogicalType::INTEGER,      LogicalType::BIGINT,
	        LogicalType::HUGEINT,   LogicalType::FLOAT,    LogicalType::DOUBLE,       LogicalType::DATE,
	        LogicalType::TIMESTAMP, LogicalType::TIME,     LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ_NS,
	        LogicalType::TIME_TZ};
}

AggregateFunctionSet QuantileContFun::GetFunctions() {
	AggregateFunctionSet quantile_cont("quantile_cont");
	quantile_cont.AddFunction(EmptyQuantileFunction<ContinuousQuantileFunction>(
	    LogicalTypeId::DECIMAL, LogicalTypeId::DECIMAL, LogicalType::DOUBLE));
	quantile_cont.AddFunction(EmptyQuantileFunction<ContinuousQuantileListFunction>(
	    LogicalTypeId::DECIMAL, LogicalTypeId::DECIMAL, LogicalType::LIST(LogicalType::DOUBLE)));
	for (const auto &type : GetContinuousQuantileTypes()) {
		quantile_cont.AddFunction(EmptyQuantileFunction<ContinuousQuantileFunction>(type, type, LogicalType::DOUBLE));
		quantile_cont.AddFunction(
		    EmptyQuantileFunction<ContinuousQuantileListFunction>(type, type, LogicalType::LIST(LogicalType::DOUBLE)));
	}
	return quantile_cont;
}

} // namespace duckdb
