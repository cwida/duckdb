#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"

#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb::capiv2 {

BaseStatistics &CV2Stats::GetMutableStats(const char *function_name) {
	if (!mutable_stats) {
		throw InvalidInputException("%s: the statistics are read-only", function_name);
	}
	return *mutable_stats;
}

//! Casts a bound value to the type of the statistics: a NULL stays NULL. Throws when the value does not cast.
static Value StatsValue(const char *function, const BaseStatistics &stats, const Value &input) {
	if (input.IsNull()) {
		return Value(stats.GetType());
	}
	string error;
	auto result = input.DefaultTryCastAs(stats.GetType(), &error, true);
	if (!result) {
		throw InvalidInputException("%s: the value '%s' of type %s does not cast to the type %s of the statistics",
		                            function, input.ToString(), input.type().ToString(), stats.GetType().ToString());
	}
	return std::move(*result);
}

//! Throws unless the statistics are of the expected kind (e.g. a numeric minimum needs numeric statistics).
static void ExpectStatsType(const char *function, const BaseStatistics &stats, StatisticsType expected,
                            const char *what) {
	if (stats.GetStatsType() != expected) {
		throw InvalidInputException("%s: statistics of type %s have no %s", function, stats.GetType().ToString(), what);
	}
}

static void CheckShreddedLayout(const BaseStatistics &layout, const BaseStatistics &node);

//! The fields of an OBJECT and the element of an ARRAY are shredded layout nodes again; anything else is a leaf.
static void CheckShreddedTypedValue(const BaseStatistics &layout, const BaseStatistics &typed_value) {
	auto &type = typed_value.GetType();
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		for (idx_t i = 0; i < StructType::GetChildCount(type); i++) {
			CheckShreddedLayout(layout, StructStats::GetChildStats(typed_value, i));
		}
		break;
	case LogicalTypeId::LIST:
		CheckShreddedLayout(layout, ListStats::GetChildStats(typed_value));
		break;
	default:
		break;
	}
}

//! Throws unless the node is a primitive or a STRUCT(typed_value, untyped_value_index UINTEGER), as the engine expects.
static void CheckShreddedLayout(const BaseStatistics &layout, const BaseStatistics &node) {
	auto &type = node.GetType();
	if (!type.IsNested()) {
		return;
	}
	bool valid = type.id() == LogicalTypeId::STRUCT;
	if (valid) {
		auto &children = StructType::GetChildTypes(type);
		valid = (children.size() == 1 || children.size() == 2) &&
		        children[VariantStats::TYPED_VALUE_INDEX].first == "typed_value";
		if (valid && children.size() == 2) {
			auto &untyped = children[VariantStats::UNTYPED_VALUE_INDEX];
			valid = untyped.first == "untyped_value_index" && untyped.second.id() == LogicalTypeId::UINTEGER;
		}
	}
	if (!valid) {
		throw InvalidInputException("duckdb_v2_stats_create_variant_shredded: statistics of type %s do not describe a "
		                            "shredded layout: %s is not a primitive type or a STRUCT(typed_value, "
		                            "untyped_value_index UINTEGER), derive the layout with "
		                            "duckdb_v2_stats_variant_shredding_type",
		                            layout.GetType().ToString(), type.ToString());
	}
	CheckShreddedTypedValue(layout, StructStats::GetChildStats(node, VariantStats::TYPED_VALUE_INDEX));
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

using duckdb::ArrayStats;
using duckdb::BaseStatistics;
using duckdb::GeometryStats;
using duckdb::GeometryTypeSet;
using duckdb::InvalidInputException;
using duckdb::ListStats;
using duckdb::NumericStats;
using duckdb::StatisticsType;
using duckdb::StatsInfo;
using duckdb::StringStats;
using duckdb::StringStatsType;
using duckdb::StructStats;
using duckdb::StructType;
using duckdb::VariantStats;

//----------------------------------------------------------------------------------------------------------------------
// Validity
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_can_have_null(duckdb_v2_stats_handle stats, bool *out,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = Convert(stats)->GetStats().CanHaveNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_can_have_valid(duckdb_v2_stats_handle stats, bool *out,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(out);
	return WithErrorHandler(err, [&]() { *out = Convert(stats)->GetStats().CanHaveNoNull(); });
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_can_have_null(duckdb_v2_stats_handle stats, bool value,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_can_have_null");
		result.Set(value ? StatsInfo::CAN_HAVE_NULL_VALUES : StatsInfo::CANNOT_HAVE_NULL_VALUES);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_can_have_valid(duckdb_v2_stats_handle stats, bool value,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_can_have_valid");
		result.Set(value ? StatsInfo::CAN_HAVE_VALID_VALUES : StatsInfo::CANNOT_HAVE_VALID_VALUES);
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Construction
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_create(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle type,
                                       duckdb_v2_stats_handle *stats, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(stats);
	*stats = nullptr;
	return WithErrorHandler(err, [&]() {
		auto type_ref = Convert(type);
		const auto &logical_type = *type_ref;
		if (logical_type.id() == duckdb::LogicalTypeId::INVALID) {
			throw InvalidInputException("duckdb_v2_stats_create: cannot create statistics of an INVALID type");
		}
		*stats = Convert(CV2Stats::Owned(BaseStatistics::CreateUnknown(logical_type)).release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_create_variant_shredded(duckdb_v2_factory_handle factory,
                                                        duckdb_v2_stats_handle shredded_stats,
                                                        duckdb_v2_stats_handle *stats,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(shredded_stats);
	DUCKDB_CHECK_ARG(stats);
	*stats = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &shredded = Convert(shredded_stats)->GetStats();
		CheckShreddedLayout(shredded, shredded);
		auto result = VariantStats::CreateShredded(shredded.GetType());
		// The shredded statistics start out empty: like duckdb_v2_stats_create, claim nothing about NULLs
		result.SetHasNull();
		result.SetHasNoNull();
		VariantStats::SetShreddedStats(result, shredded);
		*stats = Convert(CV2Stats::Owned(std::move(result)).release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_variant_shredding_type(duckdb_v2_factory_handle factory,
                                                       duckdb_v2_logical_type_handle structured_type,
                                                       duckdb_v2_logical_type_handle *type,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(structured_type);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() {
		auto structured_ref = Convert(structured_type);
		*type = Convert(VariantStats::GetShreddingType(*structured_ref));
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_destroy(duckdb_v2_stats_handle *stats) {
	return WithErrorHandler(nullptr, [&]() {
		if (!stats || !*stats) {
			return;
		}
		auto handle = Convert(*stats);
		if (!handle->IsOwned()) {
			throw InvalidInputException("duckdb_v2_stats_destroy: the statistics are borrowed, only statistics from "
			                            "duckdb_v2_stats_create can be destroyed");
		}
		delete handle;
		*stats = nullptr;
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Bounds and values
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_set_min(duckdb_v2_stats_handle stats, duckdb_v2_value_handle value,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_min");
		ExpectStatsType("duckdb_v2_stats_set_min", result, StatisticsType::NUMERIC_STATS, "numeric minimum");
		NumericStats::SetMin(result, StatsValue("duckdb_v2_stats_set_min", result, *Convert(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_max(duckdb_v2_stats_handle stats, duckdb_v2_value_handle value,
                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_max");
		ExpectStatsType("duckdb_v2_stats_set_max", result, StatisticsType::NUMERIC_STATS, "numeric maximum");
		NumericStats::SetMax(result, StatsValue("duckdb_v2_stats_set_max", result, *Convert(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_string_min(duckdb_v2_stats_handle stats, duckdb_v2_str *value, bool is_exact,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_string_min");
		ExpectStatsType("duckdb_v2_stats_set_string_min", result, StatisticsType::STRING_STATS, "string minimum");
		auto bytes = Convert(value);
		duckdb::string_t text(bytes.data(), duckdb::UnsafeNumericCast<uint32_t>(bytes.size()));
		StringStats::SetMin(result, text, is_exact ? StringStatsType::EXACT_STATS : StringStatsType::TRUNCATED_STATS);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_string_max(duckdb_v2_stats_handle stats, duckdb_v2_str *value, bool is_exact,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_string_max");
		ExpectStatsType("duckdb_v2_stats_set_string_max", result, StatisticsType::STRING_STATS, "string maximum");
		auto bytes = Convert(value);
		duckdb::string_t text(bytes.data(), duckdb::UnsafeNumericCast<uint32_t>(bytes.size()));
		StringStats::SetMax(result, text, is_exact ? StringStatsType::EXACT_STATS : StringStatsType::TRUNCATED_STATS);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_constant(duckdb_v2_stats_handle stats, duckdb_v2_value_handle value,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_constant");
		result = BaseStatistics::FromConstant(StatsValue("duckdb_v2_stats_set_constant", result, *Convert(value)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_distinct_count(duckdb_v2_stats_handle stats, idx_t count,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(
	    err, [&]() { Convert(stats)->GetMutableStats("duckdb_v2_stats_set_distinct_count").SetDistinctCount(count); });
}

//----------------------------------------------------------------------------------------------------------------------
// Nested types
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_set_child(duckdb_v2_stats_handle stats, idx_t child_index, duckdb_v2_stats_handle child,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		const char *const function = "duckdb_v2_stats_set_child";
		auto &result = Convert(stats)->GetMutableStats(function);
		duckdb::optional_ptr<const BaseStatistics> new_child;
		if (child) {
			new_child = Convert(child)->GetStats();
		}
		const auto stats_type = result.GetStatsType();

		// The current child statistics give the type the new child must have (and a null child is reset to)
		duckdb::optional_ptr<const BaseStatistics> current;
		switch (stats_type) {
		case StatisticsType::LIST_STATS:
		case StatisticsType::ARRAY_STATS:
			if (child_index != 0) {
				throw InvalidInputException("%s: statistics of type %s have a single child (index 0), not child %llu",
				                            function, result.GetType().ToString(),
				                            static_cast<unsigned long long>(child_index));
			}
			current = stats_type == StatisticsType::LIST_STATS ? ListStats::GetChildStats(result)
			                                                   : ArrayStats::GetChildStats(result);
			break;
		case StatisticsType::STRUCT_STATS:
			if (child_index >= StructType::GetChildCount(result.GetType())) {
				throw InvalidInputException("%s: statistics of type %s have no child %llu", function,
				                            result.GetType().ToString(), static_cast<unsigned long long>(child_index));
			}
			current = StructStats::GetChildStats(result, child_index);
			break;
		default:
			throw InvalidInputException("%s: statistics of type %s have no children", function,
			                            result.GetType().ToString());
		}

		if (new_child && new_child->GetType() != current->GetType()) {
			throw InvalidInputException("%s: child %llu of statistics of type %s is of type %s, not %s", function,
			                            static_cast<unsigned long long>(child_index), result.GetType().ToString(),
			                            current->GetType().ToString(), new_child->GetType().ToString());
		}

		auto replacement =
		    new_child ? new_child->ToUnique() : BaseStatistics::CreateUnknown(current->GetType()).ToUnique();
		switch (stats_type) {
		case StatisticsType::LIST_STATS:
			ListStats::SetChildStats(result, std::move(replacement));
			break;
		case StatisticsType::ARRAY_STATS:
			ArrayStats::SetChildStats(result, std::move(replacement));
			break;
		default:
			StructStats::SetChildStats(result, child_index, std::move(replacement));
			break;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_merge(duckdb_v2_stats_handle stats, duckdb_v2_stats_handle other,
                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(other);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_merge");
		auto &other_stats = Convert(other)->GetStats();
		if (result.GetType() != other_stats.GetType()) {
			throw InvalidInputException(
			    "duckdb_v2_stats_merge: cannot merge statistics of type %s into statistics of type %s",
			    other_stats.GetType().ToString(), result.GetType().ToString());
		}
		result.Merge(other_stats);
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_assign(duckdb_v2_stats_handle stats, duckdb_v2_stats_handle other,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(other);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_assign");
		auto &other_stats = Convert(other)->GetStats();
		if (result.GetType() != other_stats.GetType()) {
			throw InvalidInputException(
			    "duckdb_v2_stats_assign: cannot assign statistics of type %s to statistics of type %s",
			    other_stats.GetType().ToString(), result.GetType().ToString());
		}
		result = other_stats.Copy();
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Geometry
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_set_geometry_extent(duckdb_v2_stats_handle stats, double x_min, double x_max,
                                                    double y_min, double y_max, bool has_z, double z_min, double z_max,
                                                    bool has_m, double m_min, double m_max,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_geometry_extent");
		ExpectStatsType("duckdb_v2_stats_set_geometry_extent", result, StatisticsType::GEOMETRY_STATS, "bounding box");
		if (x_min > x_max || y_min > y_max || (has_z && z_min > z_max) || (has_m && m_min > m_max)) {
			throw InvalidInputException(
			    "duckdb_v2_stats_set_geometry_extent: a minimum of the bounding box is larger than its maximum");
		}
		auto &extent = GeometryStats::GetExtent(result);
		const auto unknown = duckdb::GeometryExtent::Unknown();
		extent.x_min = x_min;
		extent.x_max = x_max;
		extent.y_min = y_min;
		extent.y_max = y_max;
		// bounds that are not given are unknown, also when an earlier call set them
		extent.z_min = has_z ? z_min : unknown.z_min;
		extent.z_max = has_z ? z_max : unknown.z_max;
		extent.m_min = has_m ? m_min : unknown.m_min;
		extent.m_max = has_m ? m_max : unknown.m_max;
	});
}

DUCKDB_V2_ERROR duckdb_v2_stats_set_geometry_types(duckdb_v2_stats_handle stats, const int32_t *wkb_types, idx_t count,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	if (count > 0) {
		DUCKDB_CHECK_ARG(wkb_types);
	}
	return WithErrorHandler(err, [&]() {
		auto &result = Convert(stats)->GetMutableStats("duckdb_v2_stats_set_geometry_types");
		ExpectStatsType("duckdb_v2_stats_set_geometry_types", result, StatisticsType::GEOMETRY_STATS, "geometry types");
		if (count == 0) {
			// No types given: the column may hold a geometry of any type
			GeometryStats::GetTypes(result) = GeometryTypeSet::Unknown();
			return;
		}
		// collect into a fresh set first so a refused code leaves the statistics untouched
		GeometryTypeSet types = GeometryTypeSet::Empty();
		for (idx_t i = 0; i < count; i++) {
			if (!types.TryAddWKBType(wkb_types[i])) {
				throw InvalidInputException("duckdb_v2_stats_set_geometry_types: %d is not an ISO WKB geometry type",
				                            wkb_types[i]);
			}
		}
		GeometryStats::GetTypes(result) = types;
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Type
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_stats_get_type(duckdb_v2_stats_handle stats, duckdb_v2_logical_type_handle *type,
                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(stats);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() { *type = Convert(Convert(stats)->GetStats().GetType()); });
}
