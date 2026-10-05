//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-zone-offsets.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "tz_calendar.hpp"

#include <algorithm>

namespace duckdb {

//! The UTC offsets of a time zone as a table of constant-offset segments, so instants and wall times
//! (both in µs) can be converted with arithmetic instead of per-value calendar calls.
//!
//! A transition starts at the instant `utc` and at the wall time `utc + offset_after`. Wall times in a gap
//! therefore take the offset before the transition and wall times in an overlap the offset after it,
//! which is how the calendar resolves them.
class ZoneOffsets {
public:
	//! The last segment a lookup hit, so that clustered inputs skip the binary search.
	struct Cursor {
		int64_t lo = 0;
		int64_t hi = 0;
		int64_t offset = 0;
	};

	//! Builds the table, or returns nullptr if the transitions of the zone do not match its offsets or it has an
	//! offset of a day or more. Zones with recurring rules are only covered up to a fixed year.
	static shared_ptr<const ZoneOffsets> Build(const TimeZone &zone);

	//! The offset of the instant `utc`, or false past the covered range.
	bool OffsetAtInstant(int64_t utc, Cursor &cursor, int64_t &offset) const {
		return Lookup<&Segment::start_utc>(utc, cursor, offset);
	}

	//! The offset that resolves the wall time `local`, or false past the covered range.
	bool OffsetAtWallTime(int64_t local, Cursor &cursor, int64_t &offset) const {
		return Lookup<&Segment::start_local>(local, cursor, offset);
	}

private:
	struct Segment {
		int64_t start_utc;
		int64_t start_local;
		int64_t offset;
	};

	//! Both starts strictly increase. The first segment starts at INT64_MIN and the last one only marks
	//! the end of the covered range.
	vector<Segment> segments;

	template <int64_t Segment::*START>
	bool Lookup(int64_t key, Cursor &cursor, int64_t &offset) const {
		if (key < cursor.lo || key >= cursor.hi) {
			const auto next = std::upper_bound(segments.begin(), segments.end(), key,
			                                   [](int64_t k, const Segment &segment) { return k < segment.*START; });
			if (next == segments.end()) {
				return false;
			}
			const auto &segment = *(next - 1);
			cursor.lo = segment.*START;
			cursor.hi = (*next).*START;
			cursor.offset = segment.offset;
		}
		offset = cursor.offset;
		return true;
	}
};

} // namespace duckdb
