#include "duckdb/function/cast/cast_statistics.hpp"

#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"

namespace duckdb {

static bool IsPlainTimestamp(const LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

static bool IsTzTimestamp(const LogicalTypeId id) {
	return id == LogicalTypeId::TIMESTAMP_TZ || id == LogicalTypeId::TIMESTAMP_TZ_NS;
}

static bool HasInfinities(const LogicalType &type) {
	return type.id() == LogicalTypeId::DATE || IsPlainTimestamp(type.id()) || IsTzTimestamp(type.id());
}

//! Date and timestamp casts are discontinuous at -infinity, which sorts above the smallest finite value
static bool WidenByInfinities(const BaseStatistics &input, const LogicalType &target, Value &min, Value &max) {
	auto &source = input.GetType();
	if (!HasInfinities(source)) {
		return true;
	}
	if (max < min) {
		std::swap(min, max);
	}
	auto source_min = NumericStats::Min(input);
	auto source_max = NumericStats::Max(input);
	for (auto &infinity : {Value::NegativeInfinity(source), Value::Infinity(source)}) {
		if (infinity < source_min || infinity > source_max) {
			continue;
		}
		auto cast_infinity = infinity.DefaultTryCastAs(target);
		if (!cast_infinity) {
			return false;
		}
		NumericStats::UpdateValue(*cast_infinity, min, max);
	}
	return true;
}

static unique_ptr<BaseStatistics> StatisticsOperationsNumericNumericCast(const BaseStatistics &input,
                                                                         const LogicalType &target) {
	// Bail out if the stats are not numeric
	if (input.GetStatsType() != StatisticsType::NUMERIC_STATS) {
		return nullptr;
	}
	if (!input.CanHaveNoNull()) {
		auto result = NumericStats::CreateEmpty(target);
		result.CopyBase(input);
		return result.ToUnique();
	}
	if (!NumericStats::HasMinMax(input)) {
		return nullptr;
	}
	auto min = NumericStats::Min(input).DefaultTryCastAs(target);
	auto max = NumericStats::Max(input).DefaultTryCastAs(target);
	if (!min || !max || !WidenByInfinities(input, target, *min, *max)) {
		// overflow in cast: bailout
		return nullptr;
	}
	auto result = NumericStats::CreateEmpty(target);
	result.CopyBase(input);
	NumericStats::SetMin(result, *min);
	NumericStats::SetMax(result, *max);
	return result.ToUnique();
}

bool CastStatistics::CanPropagate(const LogicalType &source, const LogicalType &target) {
	if (source == target) {
		return true;
	}
	if (source.id() == LogicalTypeId::ENUM || target.id() == LogicalTypeId::ENUM) {
		return false;
	}
	// we can only propagate numeric -> numeric
	switch (source.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		break;
	default:
		return false;
	}
	switch (target.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		break;
	default:
		return false;
	}
	// for time/timestamps/dates - there are various limitations on what we can propagate
	//	Downcasting timestamps to times is not a truncation operation
	switch (target.id()) {
	case LogicalTypeId::TIME: {
		switch (source.id()) {
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return false;
		default:
			break;
		}
		break;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS: {
		if (IsTzTimestamp(source.id())) {
			return false;
		}
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS: {
		if (IsPlainTimestamp(source.id())) {
			return false;
		}
		break;
	}
	case LogicalTypeId::TIME_TZ: {
		// Casts to TIMETZ from TIME or TIMESTAMPTZ are session-TimeZone dependent
		// (the ICU extension overrides them at execution time) but Value::DefaultTryCastAs
		// uses the static, UTC-only operator, so propagated min/max would diverge from
		// runtime values. See issue #22235.
		switch (source.id()) {
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIMESTAMP_TZ:
			return false;
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
	// we can propagate!
	return true;
}

static unique_ptr<BaseStatistics> StatisticsPropagateVariant(const BaseStatistics &input, const LogicalType &target) {
	if (target.IsNested() || target.id() == LogicalTypeId::VARIANT) {
		// only try this for non-nested
		return nullptr;
	}
	if (input.GetStatsType() != StatisticsType::VARIANT_STATS) {
		// e.g. the unknown statistics of a prepared statement parameter
		return nullptr;
	}
	if (!VariantStats::IsShredded(input)) {
		// not shredded
		return nullptr;
	}
	auto structured_type = VariantStats::GetShreddedStructuredType(input);
	auto &shredded_stats = VariantStats::GetShreddedStats(input);
	if (!VariantShreddedStats::IsFullyShredded(shredded_stats)) {
		// this field might be partially shredded - skip stats propagation
		return nullptr;
	}
	// extract the typed stats
	auto &typed_stats = VariantStats::GetTypedStats(shredded_stats);
	unique_ptr<BaseStatistics> result;
	if (structured_type == target) {
		// type matches - return stats directly
		result = typed_stats.ToUnique();
	} else {
		// typed stats don't match - try to cast
		result = CastStatistics::TryPropagate(typed_stats, structured_type, target);
	}
	if (result && input.CanHaveNull()) {
		result->SetHasNull();
	}
	return result;
}

static unique_ptr<BaseStatistics> StatisticsPropagateArrayToList(const BaseStatistics &input, const LogicalType &source,
                                                                 const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::ARRAY);
	D_ASSERT(target.id() == LogicalTypeId::LIST);

	auto &source_child_type = ArrayType::GetChildType(source);
	auto &target_child_type = ListType::GetChildType(target);
	if (source_child_type != target_child_type || input.GetStatsType() != StatisticsType::ARRAY_STATS) {
		return nullptr;
	}

	auto result = ListStats::CreateEmpty(target);
	result.CopyBase(input);
	ListStats::GetChildStats(result).Copy(ArrayStats::GetChildStats(input));
	return result.ToUnique();
}

unique_ptr<BaseStatistics> CastStatistics::TryPropagate(const BaseStatistics &stats, const LogicalType &source,
                                                        const LogicalType &target) {
	if (source.id() == LogicalTypeId::VARIANT) {
		return StatisticsPropagateVariant(stats, target);
	}
	if (target.id() == LogicalTypeId::VARIANT) {
		// the cast shreds every value into a single bucket - mirror the (possibly nested) source as typed stats
		return VariantStats::StatisticsPropagateToVariant(source, stats);
	}
	if (source.id() == LogicalTypeId::GEOMETRY && target.id() == LogicalTypeId::GEOMETRY) {
		// A geometry -> geometry cast only changes CRS metadata, not coordinates, so the bounding box,
		// type set and null-ness are unchanged: propagate the statistics as-is.
		return stats.Copy().ToUnique();
	}
	if (source.id() == LogicalTypeId::ARRAY && target.id() == LogicalTypeId::LIST) {
		return StatisticsPropagateArrayToList(stats, source, target);
	}
	if (!CanPropagate(source, target)) {
		return nullptr;
	}
	return StatisticsOperationsNumericNumericCast(stats, target);
}

unique_ptr<BaseStatistics> CastStatistics::Propagate(CastStatisticsInput &input) {
	return TryPropagate(input.child_stats, input.source_type, input.target_type);
}

} // namespace duckdb
