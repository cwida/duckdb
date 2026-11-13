#include "duckdb/function/scalar/geometry_functions.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

static void FromWKBFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	Geometry::FromBinary(input.data[0], result, input.size(), true);
}

static auto FromWKBStats(ClientContext &context, FunctionStatisticsInput &input) -> unique_ptr<BaseStatistics> {
	const auto &child_stats = input.child_stats[0];

	// Start from fully unknown geometry stats and copy over the validity (null-ness) of the input.
	auto result = GeometryStats::CreateUnknown(input.expr.GetReturnType());
	result.CopyValidity(child_stats);

	if (!StringStats::HasMinMax(child_stats)) {
		// No min/max available, we can't say anything about the types
		return result.ToUnique();
	}

	// The lexicographically smallest and largest WKB blobs. Any prefix shared by both is shared by every row,
	// so if the first 5 bytes (byte order + type meta) match, all rows have the exact same geometry type and ZM flags.
	const auto min_blob = StringStats::Min(child_stats);
	const auto max_blob = StringStats::Max(child_stats);

	constexpr idx_t WKB_HEADER_SIZE = sizeof(uint8_t) + sizeof(uint32_t);
	if (min_blob.size() < WKB_HEADER_SIZE || max_blob.size() < WKB_HEADER_SIZE) {
		return result.ToUnique();
	}
	if (memcmp(min_blob.data(), max_blob.data(), WKB_HEADER_SIZE) != 0) {
		// The headers differ, so multiple geometry types may be present
		return result.ToUnique();
	}

	// Now parse the 5-byte WKB header (byte order + type meta) into a geometry/vertex type.
	const auto header = const_data_ptr_cast(min_blob.data());
	const auto le = Load<uint8_t>(header);

	auto meta = Load<uint32_t>(header + sizeof(uint8_t));
	if (!le) {
		meta = BSwap(meta);
	}

	const auto type_id = (meta & 0x0000FFFF) % 1000;
	const auto flag_id = (meta & 0x0000FFFF) / 1000;
	if (type_id < 1 || type_id > 7 || flag_id > 3) {
		// Unsupported or invalid geometry type, we can't say anything about the types
		return result.ToUnique();
	}

	// Z/M may also be signalled through the Extended-WKB high bits (matching Geometry::FromBinary)
	const auto has_z = ((flag_id & 0x01) != 0) || ((meta & 0x80000000) != 0);
	const auto has_m = ((flag_id & 0x02) != 0) || ((meta & 0x40000000) != 0);

	const auto geom_type = static_cast<GeometryType>(type_id);
	const auto vert_type = static_cast<VertexType>((has_z ? 1 : 0) | (has_m ? 2 : 0));

	// The single inferred type implies a minimum body length: a POINT serializes all of its ordinates inline
	// (even when empty, encoded as NaNs); every other type serializes at least a 4-byte element count. If the
	// shortest row (the exact minimum over all rows) can't hold that, the column has a truncated blob, so bail.
	const idx_t vert_dims = 2 + (has_z ? 1 : 0) + (has_m ? 1 : 0);
	const auto min_valid_size = geom_type == GeometryType::POINT ? WKB_HEADER_SIZE + vert_dims * sizeof(double)
	                                                             : WKB_HEADER_SIZE + sizeof(uint32_t);

	const auto min_len = StringStats::MinStringLength(child_stats);
	if (!min_len.IsValid() || min_len.GetIndex() < min_valid_size) {
		// Only bounds the byte envelope, not that a declared element count matches the body.
		// In general, we cant guarantee that the WKB is valid without parsing every row.
		// But we're generally OK with optimizing assuming valid input - anything else is essentially UB.
		return result.ToUnique();
	}

	// All rows share this single geometry type. The extent and emptiness can't be inferred
	// from the truncated string stats, so those remain unknown.
	auto &types = GeometryStats::GetTypes(result);
	types = GeometryTypeSet::Empty();
	types.Add(geom_type, vert_type);

	return result.ToUnique();
}

ScalarFunction StGeomfromwkbFun::GetFunction() {
	ScalarFunction function({}, LogicalType::GEOMETRY(), FromWKBFunction);
	function.GetSignature().AddParameter("wkb", LogicalType::BLOB);
	function.SetStatisticsCallback(FromWKBStats);
	// throws when the input is not valid WKB
	function.SetFallible();
	return function;
}

static void ToWKBFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(input.data[0], result, [&](const string_t &geom) {
		// TODO: convert to internal representation
		return geom;
	});
	// Add a heap reference to the input WKB to prevent it from being freed
	StringVector::AddHeapReference(input.data[0], result);
}

ScalarFunction StAswkbFun::GetFunction() {
	ScalarFunction function({}, LogicalType::BLOB, ToWKBFunction);
	function.GetSignature().AddParameter("geom", LogicalType::GEOMETRY());
	return function;
}

static void ToWKTFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(input.data[0], result,
	                                           [&](const string_t &geom) { return Geometry::ToString(heap, geom); });
}

ScalarFunction StAstextFun::GetFunction() {
	ScalarFunction function({}, LogicalType::VARCHAR, ToWKTFunction);
	function.GetSignature().AddParameter("geom", LogicalType::GEOMETRY());
	return function;
}

static void IntersectsExtentFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    input.data[0], input.data[1], result, [](const string_t &lhs_geom, const string_t &rhs_geom) {
		    auto lhs_extent = GeometryExtent::Empty();
		    auto rhs_extent = GeometryExtent::Empty();

		    const auto lhs_is_empty = Geometry::GetExtent(lhs_geom, lhs_extent) == 0;
		    const auto rhs_is_empty = Geometry::GetExtent(rhs_geom, rhs_extent) == 0;

		    if (lhs_is_empty || rhs_is_empty) {
			    // One of the geometries is empty
			    return false;
		    }

		    // Don't take Z and M into account for intersection test
		    return lhs_extent.IntersectsXY(rhs_extent);
	    });
}

// Prune row groups for `geom1 && geom2` (a.k.a. ST_Intersects_Extent), which is true iff the two bounding
// boxes intersect. One argument must be a constant geometry; the other is the column we prune against. Both
// operands' statistics are derived for us (the constant's geometry stats already carry its bounding box, and
// the column's look through any CRS-only cast), so this works regardless of which side the constant is on.
static FilterPropagateResult IntersectsExtentFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto &children = input.function.GetChildren();
	if (children.size() != 2) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	// Constants are folded by the time we get here, so the constant operand is a plain BoundConstantExpression.
	const auto lhs_is_const = children[0]->GetExpressionType() == ExpressionType::VALUE_CONSTANT;
	const auto rhs_is_const = children[1]->GetExpressionType() == ExpressionType::VALUE_CONSTANT;
	if (lhs_is_const == rhs_is_const) {
		// Need exactly one constant operand and one column operand.
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	const idx_t constant_idx = lhs_is_const ? 0 : 1;

	auto column_stats = input.ChildStats(1 - constant_idx);
	auto constant_stats = input.ChildStats(constant_idx);
	if (!column_stats || column_stats->GetStatsType() != StatisticsType::GEOMETRY_STATS || !constant_stats ||
	    constant_stats->GetStatsType() != StatisticsType::GEOMETRY_STATS) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!column_stats->CanHaveNoNull()) {
		// no non-null values are possible: always false
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	const auto &col_extent = GeometryStats::GetExtent(*column_stats);
	if (!col_extent.CanPruneXY()) {
		// If neither axis is set (the extent is empty or fully unknown), we cannot prune.
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	const auto &const_extent = GeometryStats::GetExtent(*constant_stats);
	if (!const_extent.CanPruneXY()) {
		// An empty constant geometry never intersects anything.
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (!const_extent.IntersectsXY(col_extent)) {
		// The column zonemap does not intersect the constant: no row can match.
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (const_extent.ContainsXY(col_extent)) {
		// The constant fully covers the column zonemap, so every row that is represented in the zonemap
		// matches. Rows that are not represented in it do not: NULL rows (the predicate evaluates to NULL
		// for them) and empty geometries (which contribute no vertices, but never intersect anything).
		// Only when neither can be present may we prune the filter away entirely.
		if (column_stats->CanHaveNull() || GeometryStats::GetFlags(*column_stats).HasEmptyGeometry()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

ScalarFunction StIntersectsExtentFun::GetFunction() {
	ScalarFunction function({}, LogicalType::BOOLEAN, IntersectsExtentFunction);
	function.GetSignature()
	    .AddParameter("geom1", LogicalType::GEOMETRY())
	    .AddParameter("geom2", LogicalType::GEOMETRY());
	function.SetFilterPruneCallback(IntersectsExtentFilterPrune);
	return function;
}

static Value GetCRSValue(const LogicalType &logical_type) {
	if (!GeoType::HasCRS(logical_type)) {
		// Return null
		return Value(LogicalTypeId::VARCHAR);
	}
	auto &crs = GeoType::GetCRS(logical_type);
	return Value(crs.GetDefinition());
}

static void CRSFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &type = args.data[0].GetType();
	result.Reference(GetCRSValue(type), count_t(args.size()));
}

static unique_ptr<Expression> BindCRSFunctionExpression(FunctionBindExpressionInput &input) {
	const auto &return_type = input.children[0]->GetReturnType();
	if (return_type.id() != LogicalTypeId::GEOMETRY) {
		// parameter - unknown return type
		return nullptr;
	}

	return make_uniq<BoundConstantExpression>(GetCRSValue(return_type));
}

static void CRSFunctionResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &argument_type = input.GetArgumentType(0);
	if (argument_type.id() != LogicalTypeId::GEOMETRY) {
		return;
	}

	// Propagate the CRS from the input argument to the parameter type
	input.GetBoundFunction().GetArguments()[0] = argument_type;
}

ScalarFunction StCrsFun::GetFunction() {
	ScalarFunction geom_func({}, LogicalType::VARCHAR, CRSFunction);
	geom_func.SetResolveTypesCallback(CRSFunctionResolveTypes);
	geom_func.GetSignature().AddParameter("geom", LogicalType::GEOMETRY());
	geom_func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom_func.SetBindExpressionCallback(BindCRSFunctionExpression);
	return geom_func;
}

static unique_ptr<FunctionData> SetCRSBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();

	// Check if the CRS is set in the second argument
	const auto crs_val = input.GetConstant(1);
	if (crs_val.IsNull()) {
		return nullptr;
	}

	const auto &crs_str = StringValue::Get(crs_val);

	// Try to convert to identify
	const auto lookup = CoordinateReferenceSystem::TryIdentify(context, crs_str);
	if (lookup) {
		bound_function.SetReturnType(LogicalType::GEOMETRY(lookup->GetDefinition()));
	} else {
		// Pass on the raw string (better than nothing)
		bound_function.SetReturnType(LogicalType::GEOMETRY(crs_str));
	}
	return nullptr;
}

static void SetCRSFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reinterpret(args.data[0]);
}

ScalarFunction StSetcrsFun::GetFunction() {
	ScalarFunction geom_func({}, LogicalType::GEOMETRY(), SetCRSFunction, SetCRSBind);
	geom_func.GetSignature().AddParameter("geom", LogicalType::GEOMETRY()).AddParameter("crs", LogicalType::VARCHAR);
	return geom_func;
}

namespace {

struct VertexExtractBindData final : public FunctionData {
	explicit VertexExtractBindData(idx_t vertex_index) : vertex_index(vertex_index) {
	}

	idx_t vertex_index;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<VertexExtractBindData>(vertex_index);
	}

	auto Equals(const FunctionData &other) const -> bool override {
		auto &other_bind = other.Cast<VertexExtractBindData>();
		return vertex_index == other_bind.vertex_index;
	}
};

} // namespace

static auto VertexExtractBind(BindScalarFunctionInput &input) -> unique_ptr<FunctionData> {
	const auto vertex_val = input.GetNonNullConstant(1);
	const auto vertex_str = StringUtil::Lower(StringValue::Get(vertex_val));
	if (vertex_str == "x") {
		return make_uniq<VertexExtractBindData>(static_cast<idx_t>(0));
	}
	if (vertex_str == "y") {
		return make_uniq<VertexExtractBindData>(static_cast<idx_t>(1));
	}
	if (vertex_str == "z") {
		return make_uniq<VertexExtractBindData>(static_cast<idx_t>(2));
	}
	if (vertex_str == "m") {
		return make_uniq<VertexExtractBindData>(static_cast<idx_t>(3));
	}
	throw BinderException("vertex_extract: invalid vertex argument '%s', expected one of 'x', 'y', 'z', or 'm'",
	                      vertex_str);
}

static auto VertexExtractStats(ClientContext &context, FunctionStatisticsInput &input) -> unique_ptr<BaseStatistics> {
	const auto &child_stats = input.child_stats;
	const auto &bind_data = input.bind_data->Cast<VertexExtractBindData>();

	const auto &extent = GeometryStats::GetExtent(child_stats[0]);
	const auto &types = GeometryStats::GetTypes(child_stats[0]);
	const auto &flags = GeometryStats::GetFlags(child_stats[0]);

	auto new_stats = NumericStats::CreateUnknown(LogicalType::DOUBLE);

	if (!types.Has(GeometryType::POINT) || !flags.HasNonEmptyGeometry()) {
		// No non-empty points present, so vertex extraction will always return NULL
		*input.expr_ptr = make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE));
		new_stats.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
		new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		return new_stats.ToUnique();
	}

	new_stats.CopyValidity(child_stats[0]);

	if (!types.HasOnly(GeometryType::POINT)) {
		// If there are non-point geometries, we cannot guarantee that all rows will yield a valid value
		new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}

	// If there are empty geometries, we cannot guarantee that all rows will yield a valid value
	if (flags.HasEmptyGeometry()) {
		new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}

	if (bind_data.vertex_index == 2) {
		// Z is absent on XY and XYM points
		if (types.Has(VertexType::XY) || types.Has(VertexType::XYM)) {
			new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
		if (!types.Has(VertexType::XYZ) && !types.Has(VertexType::XYZM)) {
			// If there are no vertex types with Z, we can guarantee that all rows will yield NULL
			*input.expr_ptr = make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE));
			new_stats.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
			new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			return new_stats.ToUnique();
		}
	}

	if (bind_data.vertex_index == 3) {
		// M is absent on XY and XYZ points
		if (types.Has(VertexType::XY) || types.Has(VertexType::XYZ)) {
			new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
		if (!types.Has(VertexType::XYM) && !types.Has(VertexType::XYZM)) {
			// If there are no vertex types with M, we can guarantee that all rows will yield NULL
			*input.expr_ptr = make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE));
			new_stats.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
			new_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			return new_stats.ToUnique();
		}
	}

	if (bind_data.vertex_index == 0 && extent.HasXY()) { // X
		NumericStats::SetMin(new_stats, extent.x_min);
		NumericStats::SetMax(new_stats, extent.x_max);
		return new_stats.ToUnique();
	}
	if (bind_data.vertex_index == 1 && extent.HasXY()) { // Y
		NumericStats::SetMin(new_stats, extent.y_min);
		NumericStats::SetMax(new_stats, extent.y_max);
		return new_stats.ToUnique();
	}
	if (bind_data.vertex_index == 2 && extent.HasZ()) { // Z
		NumericStats::SetMin(new_stats, extent.z_min);
		NumericStats::SetMax(new_stats, extent.z_max);
		return new_stats.ToUnique();
	}
	if (bind_data.vertex_index == 3 && extent.HasM()) { // M
		NumericStats::SetMin(new_stats, extent.m_min);
		NumericStats::SetMax(new_stats, extent.m_max);
		return new_stats.ToUnique();
	}

	return nullptr;
}

static auto VertexExtractFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	const auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &bind_data = func_expr.BindInfo()->Cast<VertexExtractBindData>();

	UnaryExecutor::Execute<string_t, double>(input.data[0], result, [&](const string_t &geom_str) {
		const auto data = const_data_ptr_cast(geom_str.GetData());
		const auto size = geom_str.GetSize();
		const auto meta = Load<uint32_t>(data + sizeof(uint8_t));

		const auto type_id = (meta & 0x0000FFFF) % 1000;
		const auto flag_id = (meta & 0x0000FFFF) / 1000;
		const auto has_z = ((flag_id & 0x01) != 0);
		const auto has_m = ((flag_id & 0x02) != 0);

		if (type_id != 1) {
			return optional<double>();
		}

		auto value = std::numeric_limits<double>::quiet_NaN();

		if (bind_data.vertex_index == 0) { // X
			constexpr auto offset = sizeof(uint8_t) + sizeof(uint32_t);
			if (size < offset + sizeof(double)) {
				return optional<double>();
			}
			value = Load<double>(data + offset);
		} else if (bind_data.vertex_index == 1) { // Y
			constexpr auto offset = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(double);
			if (size < offset + sizeof(double)) {
				return optional<double>();
			}
			value = Load<double>(data + offset);
		} else if (bind_data.vertex_index == 2) { // Z
			if (!has_z) {
				return optional<double>();
			}
			constexpr auto offset = sizeof(uint8_t) + sizeof(uint32_t) + 2 * sizeof(double);
			if (size < offset + sizeof(double)) {
				return optional<double>();
			}
			value = Load<double>(data + offset);
		} else if (bind_data.vertex_index == 3) { // M
			if (!has_m) {
				return optional<double>();
			}
			const auto offset = sizeof(uint8_t) + sizeof(uint32_t) + (2 + has_z) * sizeof(double);
			if (size < offset + sizeof(double)) {
				return optional<double>();
			}
			value = Load<double>(data + offset);
		}

		if (std::isnan(value)) {
			return optional<double>();
		}

		return optional<double>(value);
	});
}

ScalarFunction VertexExtractFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalTypeId::DOUBLE, VertexExtractFunction, VertexExtractBind, VertexExtractStats);
	fun.GetSignature()
	    .AddParameter("geom", LogicalType::GEOMETRY())
	    .AddParameter("coordinate", LogicalTypeId::VARCHAR)
	    .SetReturnType(LogicalType::DOUBLE);

	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
