#include "include/icu-zone-offsets.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"

namespace duckdb {

static int64_t TotalOffset(const TimeZone &zone, int64_t millis) {
	int32_t raw = 0;
	int32_t dst = 0;
	zone.GetOffset(millis, raw, dst);
	return (int64_t(raw) + dst) * Interval::MICROS_PER_MSEC;
}

static bool IsUnderADay(int64_t offset) {
	return offset > -Interval::MICROS_PER_DAY && offset < Interval::MICROS_PER_DAY;
}

shared_ptr<const ZoneOffsets> ZoneOffsets::Build(const TimeZone &zone) {
	//	Recurring rules produce transitions forever, so stop at a year well past scan-scale data.
	const int64_t coverage_end = int64_t(Date::FromDate(2250, 1, 1).days) * Interval::MICROS_PER_DAY;
	//	Early enough to precede every tzdata transition, in milliseconds.
	int64_t search_from = -1000000000000000LL;

	auto offset = TotalOffset(zone, search_from);
	if (!IsUnderADay(offset)) {
		return nullptr;
	}

	auto result = make_shared_ptr<ZoneOffsets>();
	auto &segments = result->segments;
	segments.push_back(Segment {NumericLimits<int64_t>::Minimum(), NumericLimits<int64_t>::Minimum(), offset});
	int64_t transition;
	while (zone.TryGetNextTransition(search_from, transition)) {
		//	The offset must be constant up to the transition, or the zone skipped one.
		if (TotalOffset(zone, transition - 1) != segments.back().offset) {
			return nullptr;
		}
		search_from = transition;
		const auto utc = transition * Interval::MICROS_PER_MSEC;
		const auto offset_after = TotalOffset(zone, transition);
		const bool past_coverage = utc >= coverage_end;
		if (!past_coverage && offset_after == segments.back().offset) {
			continue;
		}
		if (!IsUnderADay(offset_after)) {
			return nullptr;
		}
		//	A fall-back longer than the time since the previous transition would make wall times non-monotonic.
		if (utc + offset_after <= segments.back().start_local) {
			return nullptr;
		}
		segments.push_back(Segment {utc, utc + offset_after, offset_after});
		if (past_coverage) {
			return std::move(result);
		}
	}
	segments.push_back(Segment {NumericLimits<int64_t>::Maximum(), NumericLimits<int64_t>::Maximum(), 0});
	return std::move(result);
}

} // namespace duckdb
