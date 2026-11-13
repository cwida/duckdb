#include "core_functions/aggregate/holistic_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate/list_aggregate.hpp"

#include <cmath>

namespace duckdb {

namespace {

//! LTTB (Largest Triangle Three Buckets) downsampling. Buffers all (x, y) points as a linked list of STRUCT(x, y),
//! reusing the "list" aggregate callbacks, and selects the representative points at finalize time. Like other
//! order-sensitive aggregates, the points are expected to arrive ordered by x (use "lttb(x, y, n ORDER BY x)").
struct LTTBState : ListAggState {};

//! Holds the constant target point count folded into the bind data.
struct LTTBBindData : FunctionData {
	idx_t n;

	explicit LTTBBindData(const idx_t n) : n(n) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LTTBBindData>(n);
	}
	bool Equals(const FunctionData &other_p) const override {
		return n == other_p.Cast<LTTBBindData>().n;
	}
};

//! The element type buffered in the state and returned in the list: STRUCT(x, y)
auto LTTBStructType(const LogicalType &x_type, const LogicalType &y_type) -> LogicalType {
	child_list_t<LogicalType> children;
	children.emplace_back("x", x_type);
	children.emplace_back("y", y_type);
	return LogicalType::STRUCT(std::move(children));
}

struct LTTBFunction {
	//! The type of the values buffered in the linked list, used by ListCombineFunction
	static LogicalType GetElementType(AggregateInputData &aggr_input_data) {
		return ListType::GetChildType(aggr_input_data.function.GetReturnType());
	}
};

//! Packs the two (x, y) input columns into a single STRUCT vector.
//! Points with a NULL x or y are marked NULL so the list append can filter them out.
//! Returns true if any NULLs were found, so that the faster non-filtering path can be used otherwise.
auto LTTBPackPoints(Vector inputs[], idx_t count, Vector &packed) -> bool {
	auto &entries = StructVector::GetEntries(packed);
	entries[0].Reference(inputs[0]);
	entries[1].Reference(inputs[1]);
	FlatVector::SetSize(packed, count);

	const auto x_validity = entries[0].Validity();
	const auto y_validity = entries[1].Validity();
	if (!x_validity.CanHaveNull() && !y_validity.CanHaveNull()) {
		return false;
	}

	auto &point_validity = FlatVector::ValidityMutable(packed);
	point_validity.EnsureWritable();

	bool any_null = false;
	for (idx_t i = 0; i < count; i++) {
		if (!x_validity.IsValid(i) || !y_validity.IsValid(i)) {
			point_validity.SetInvalid(i);
			any_null = true;
		}
	}
	return any_null;
}

//! The number of points is folded into the bind data by the bind, but stays part of the expression tree - only the
//! leading x and y arguments are consumed
void LTTBUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &states, idx_t count) {
	D_ASSERT(input_count >= 2);
	if (count == 0) {
		return;
	}
	Vector packed(ListType::GetChildType(aggr_input_data.function.GetReturnType()), count);
	if (LTTBPackPoints(inputs, count, packed)) {
		ListUpdateFunction<true>(&packed, aggr_input_data, 1, states, count);
	} else {
		ListUpdateFunction<false>(&packed, aggr_input_data, 1, states, count);
	}
}

//! The clustered variant of LTTBUpdate
void LTTBClusterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count,
                       const ClusteredAggr &clustered, idx_t count) {
	D_ASSERT(input_count >= 2);
	if (count == 0) {
		return;
	}
	Vector packed(ListType::GetChildType(aggr_input_data.function.GetReturnType()), count);
	if (LTTBPackPoints(inputs, count, packed)) {
		ListClusterUpdate<true>(&packed, aggr_input_data, 1, clustered, count);
	} else {
		ListClusterUpdate<false>(&packed, aggr_input_data, 1, clustered, count);
	}
}

//! Converts axis values to double. Timestamp axes are normalized against an origin taken from the data.
//! Absolute epoch values do not fit the 53-bit double mantissa (nanoseconds around 2020 need ~61 bits).
//! Translating an axis does not change any triangle area.
template <class T>
struct LTTBAxis {
	explicit LTTBAxis(const T *) {
	}
	double operator()(const T value) const {
		return static_cast<double>(value);
	}
};

template <>
struct LTTBAxis<int64_t> {
	explicit LTTBAxis(const int64_t *values) : origin(values[0]) {
	}
	double operator()(const int64_t value) const {
		// infinity timestamps are INT64_MIN/MAX, so the subtraction can overflow int64_t
		return Hugeint::Cast<double>(hugeint_t(value) - hugeint_t(origin));
	}

	int64_t origin;
};

//! Compacts the materialized points down to the non-NULL ones, returning the remaining count.
//! The update path already drops NULL points, but an imported state can still contain them, so we do this to be safe.
template <class XTYPE, class YTYPE>
auto LTTBDropNullPoints(Vector &points, XTYPE *vx, YTYPE *vy, const idx_t v) -> idx_t {
	auto &axes = StructVector::GetEntries(points);
	auto &point_validity = FlatVector::Validity(points);
	auto &x_validity = FlatVector::Validity(axes[0]);
	auto &y_validity = FlatVector::Validity(axes[1]);

	// the validity is reset (and so allocated) for every group, so the bits have to be checked rather than the mask
	if (point_validity.CheckAllValid(v) && x_validity.CheckAllValid(v) && y_validity.CheckAllValid(v)) {
		return v;
	}

	idx_t valid = 0;
	for (idx_t i = 0; i < v; i++) {
		if (!point_validity.RowIsValid(i) || !x_validity.RowIsValid(i) || !y_validity.RowIsValid(i)) {
			continue;
		}
		vx[valid] = vx[i];
		vy[valid] = vy[i];
		valid++;
	}

	// the compacted prefix is fully valid
	FlatVector::ValidityMutable(points).SetAllValid(valid);
	for (auto &axis : axes) {
		FlatVector::ValidityMutable(axis).SetAllValid(valid);
	}
	return valid;
}

//! XTYPE/YTYPE are the physical C++ types of the x and y columns: float/double for a FLOAT/DOUBLE axis, int64_t for
//! any of the TIMESTAMP axes. We perform all computations with doubles, but we need the templates to cast the input
//! correctly.
template <class XTYPE, class YTYPE>
auto LTTBFinalize(Vector &vec, AggregateFinalizeInputData &data, Vector &result, idx_t count, idx_t offset) -> void {
	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);

	const auto states = vec.Values<LTTBState *>();
	const auto &bdata = data.bind_data->Cast<LTTBBindData>();

	const auto n = bdata.n;
	const auto struct_type = ListType::GetChildType(result.GetType());

	ListSegmentFunctions functions;
	GetSegmentDataFunctions(functions, struct_type);

	const auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);

	// The reusable selection vector is only needed if any group downsamples (v > n). In that case n is smaller than
	// the largest buffered group, so the allocation is bounded by the points themselves even for an oversized n.
	idx_t max_v = 0;

	// Figure out the largest list size
	for (idx_t state_idx = 0; state_idx < count; state_idx++) {
		max_v = MaxValue(max_v, states[state_idx].GetValue()->linked_list.total_capacity);
	}

	SelectionVector sel;

	// If a list contains more points than the target, we will downsample
	if (max_v > n) {
		// The selected point indices must fit in the selection vector entries
		if (max_v > NumericLimits<sel_t>::Maximum()) {
			throw OutOfRangeException("lttb: cannot downsample a group of more than %llu points",
			                          static_cast<uint64_t>(NumericLimits<sel_t>::Maximum()));
		}

		// Initialize the selection vector
		sel.Initialize(reinterpret_cast<sel_t *>(data.allocator.AllocateAligned(n * sizeof(sel_t))), n);
	}

	// Reusable vector for materializing each groups points, sized up-front to the largest group.
	// A group can be the whole input, so the vector is allocated through the (tracked) base allocator.
	DataChunk points_chunk;
	points_chunk.Initialize(data.allocator.GetUnderlyingAllocator(), {struct_type}, MaxValue<idx_t>(max_v, 1));
	auto &points = points_chunk.data[0];

	// Now do the selection for each list
	for (idx_t state_idx = 0; state_idx < count; state_idx++) {
		const auto rid = offset + state_idx;
		const auto &state = *states[state_idx].GetValue();

		auto v = state.linked_list.total_capacity;

		if (v == 0) {
			// Empty list (or all points filtered out as NULL)
			FlatVector::SetNull(result, rid, true);
			continue;
		}

		// Materialize the buffered (x, y) points (kept in insertion order, i.e. ordered by x).
		// The scan only ever marks entries invalid, so the reused validity has to be cleared for each group.
		FlatVector::ValidityMutable(points).SetAllValid(v);
		for (auto &child : StructVector::GetEntries(points)) {
			FlatVector::ValidityMutable(child).SetAllValid(v);
		}
		functions.BuildListVector(state.linked_list, points, 0);

		auto &axes = StructVector::GetEntries(points);
		const auto vx = FlatVector::GetDataMutable<XTYPE>(axes[0]);
		const auto vy = FlatVector::GetDataMutable<YTYPE>(axes[1]);

		// Imported states may contain NULL points, drop them before reading any of the axis data
		v = LTTBDropNullPoints(points, vx, vy, v);
		if (v == 0) {
			FlatVector::SetNull(result, rid, true);
			continue;
		}

		const auto old_size = ListVector::GetListSize(result);

		if (v <= n) {
			// Too few points to downsample, keep all points
			ListVector::Append(result, points, v);
			list_entries[rid] = list_entry_t(old_size, v);
			continue;
		}

		if (n < 3) {
			// n == 2: Keep only the first and the last point
			sel[0] = 0;
			sel[1] = v - 1;
			ListVector::Append(result, points, sel, 2);
			list_entries[rid] = list_entry_t(old_size, 2);
			continue;
		}

		// Downsample the v points into n buckets: the first point, n - 2 "middle" buckets, and the last point
		// Functors to normalize the axis values, if required.
		const LTTBAxis<XTYPE> to_x(vx);
		const LTTBAxis<YTYPE> to_y(vy);

		// Always keep the first and the last point
		sel[0] = 0;
		sel[n - 1] = v - 1;

		// The "width" of each bucket in points, except the first and last buckets which are fixed to a single point.
		const auto width = static_cast<double>(v - 2) / static_cast<double>(n - 2);

		// index of the previously selected point
		idx_t prev_idx = 0;

		// sel[0] and sel[n - 1] are the fixed endpoints; fill the n - 2 "middle" buckets in sel[1, ..., n - 2]
		for (idx_t i = 1; i < n - 1; i++) {
			auto bucket_index = [&](const double multiplier) {
				return static_cast<idx_t>(std::floor(multiplier * width)) + 1;
			};

			// Range of points in the next bucket
			const auto next_beg = bucket_index(i);
			const auto next_end = MinValue(bucket_index(i + 1), v);

			// Range of points in the current bucket
			const auto curr_beg = bucket_index(i - 1);
			const auto curr_end = MinValue(bucket_index(i), v);

			// Average point in the next bucket (never empty: v > n implies width > 1)
			D_ASSERT(next_end > next_beg);
			const auto next_len = next_end - next_beg;

			double avg_x = 0;
			double avg_y = 0;
			for (idx_t j = next_beg; j < next_end; j++) {
				avg_x += to_x(vx[j]);
				avg_y += to_y(vy[j]);
			}
			avg_x /= static_cast<double>(next_len);
			avg_y /= static_cast<double>(next_len);

			// Pick the point C in this bucket forming the largest triangle between
			// - A: the previously selected point
			// - B: the average point in the next bucket

			double max_area = -1.0;
			idx_t best_idx = curr_beg;

			// A
			const auto ax = to_x(vx[prev_idx]);
			const auto ay = to_y(vy[prev_idx]);

			// B
			const auto bx = avg_x;
			const auto by = avg_y;

			for (idx_t curr_idx = curr_beg; curr_idx < curr_end; curr_idx++) {
				// C
				const auto cx = to_x(vx[curr_idx]);
				const auto cy = to_y(vy[curr_idx]);

				// Compute the area of the triangle formed by points A, B, C using the shoelace formula
				const auto area = std::fabs((ax - bx) * (cy - ay) - (ax - cx) * (by - ay)) * 0.5;

				if (area > max_area) {
					max_area = area;
					best_idx = curr_idx;
				}
			}

			sel[i] = best_idx;
			prev_idx = best_idx;
		}

		// Append the selected points to the result
		ListVector::Append(result, points, sel, n);
		list_entries[rid] = list_entry_t(old_size, n);
	}
}

auto LTTBBind(BindAggregateFunctionInput &input) -> unique_ptr<FunctionData> {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	D_ASSERT(arguments.size() == 3);

	if (arguments[2]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (!arguments[2]->IsFoldable()) {
		throw BinderException("lttb: the number of points (third argument) must be a constant");
	}
	const auto n_val = ExpressionExecutor::EvaluateScalar(context, *arguments[2]);
	if (n_val.IsNull()) {
		throw BinderException("lttb: the number of points must not be NULL");
	}
	const auto n = n_val.GetValue<int64_t>();
	if (n < 2) {
		throw BinderException("lttb: the number of points must be at least 2");
	}
	return make_uniq<LTTBBindData>(NumericCast<idx_t>(n));
}

auto LTTBSerialize(Serializer &ser, const optional_ptr<FunctionData> bdata, const BoundAggregateFunction &) -> void {
	auto &bind_data = bdata->Cast<LTTBBindData>();
	ser.WriteProperty<idx_t>(100, "n", bind_data.n);
}

auto LTTBDeserialize(Deserializer &ser, BoundAggregateFunction &) -> unique_ptr<FunctionData> {
	auto n = ser.ReadProperty<idx_t>(100, "n");
	return make_uniq<LTTBBindData>(n);
}

//! Exports the buffered points as a LIST(STRUCT(x, y)), reusing the generic list-state export.
//! Records the folded constant "n" so the exported state can be re-bound.
auto LTTBStateLayout(AggregateLayoutInput &input) -> AggregateStateLayout {
	auto &function = input.function;
	using ST = LTTBState::STATE_TYPE;
	AggregateStateLayout layout;
	if (function.GetReturnType().IsAggregateState()) {
		// The function has been modified for state export, its return type IS the state type already
		layout.type = function.GetReturnType();
	} else {
		layout.type = AggregateFunction::BuildStateLogical<ST, LTTBState>(function);
	}
	layout.total_state_size = AlignValue<idx_t>(sizeof(LTTBState));
	layout.field = BuildStateField<ST>();
	AggregateStateField::PopulateListFunctions(layout.type, layout.field);
	if (function.GetArguments().size() == 3) {
		auto &bind_data = input.bind_data->Cast<LTTBBindData>();
		layout.constant_parameters.emplace(2, Value::BIGINT(NumericCast<int64_t>(bind_data.n)));
	}
	return layout;
}

//! Builds one lttb overload: x of x_type (physical XTYPE), y of y_type (physical YTYPE), n BIGINT
//! -> LIST(STRUCT(x, y))
template <class XTYPE, class YTYPE>
AggregateFunction GetLTTBFunction(const LogicalType &x_type, const LogicalType &y_type) {
	AggregateFunction fun("lttb", {}, LogicalType::LIST(LTTBStructType(x_type, y_type)),
	                      AggregateFunction::StateSize<LTTBState>,
	                      AggregateFunction::StateInitialize<LTTBState, LTTBFunction>, LTTBUpdate,
	                      ListCombineFunction<LTTBFunction>, LTTBFinalize<XTYPE, YTYPE>, LTTBClusterUpdate, LTTBBind);
	fun.GetSignature().AddParameter("x", x_type).AddParameter("y", y_type).AddParameter("n", LogicalType::BIGINT);
	fun.SetSerializeCallback(LTTBSerialize);
	fun.SetDeserializeCallback(LTTBDeserialize);
	fun.SetStructStateExport(LTTBStateLayout);
	fun.SetFallible(); // finalize throws when a group exceeds the selection vector limit
	return fun;
}

//! Adds the lttb overloads for one x type: the y axis is always a measurement, so only FLOAT and DOUBLE
template <class XTYPE>
void AddLTTBFunctions(AggregateFunctionSet &set, const LogicalType &x_type) {
	set.AddFunction(GetLTTBFunction<XTYPE, float>(x_type, LogicalType::FLOAT));
	set.AddFunction(GetLTTBFunction<XTYPE, double>(x_type, LogicalType::DOUBLE));
}

} // namespace

AggregateFunctionSet LttbFun::GetFunctions() {
	AggregateFunctionSet lttb;
	AddLTTBFunctions<float>(lttb, LogicalType::FLOAT);
	AddLTTBFunctions<double>(lttb, LogicalType::DOUBLE);
	AddLTTBFunctions<int64_t>(lttb, LogicalType::TIMESTAMP);
	AddLTTBFunctions<int64_t>(lttb, LogicalType::TIMESTAMP_S);
	AddLTTBFunctions<int64_t>(lttb, LogicalType::TIMESTAMP_MS);
	AddLTTBFunctions<int64_t>(lttb, LogicalType::TIMESTAMP_NS);
	AddLTTBFunctions<int64_t>(lttb, LogicalType::TIMESTAMP_TZ);
	AddLTTBFunctions<int64_t>(lttb, LogicalType::TIMESTAMP_TZ_NS);
	return lttb;
}

} // namespace duckdb
