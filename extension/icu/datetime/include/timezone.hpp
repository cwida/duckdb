//===----------------------------------------------------------------------===//
//                         DuckDB
//
// timezone.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"
#include "tz_data.hpp"

namespace duckdb {
namespace datetime {

//! How a local time that either does not exist or occurs twice is resolved.
//! FORMER picks the offset that was in effect before the transition, LATTER the one after it.
enum class LocalOption : uint8_t { FORMER, LATTER };

//! A time zone, i.e. a mapping between instants and the UTC offsets that apply to them.
//! All offsets are expressed in milliseconds, and all times in milliseconds since 1970-01-01 UTC.
class TimeZone {
public:
	virtual ~TimeZone() = default;

	//! Looks up a time zone by identifier - returns nullptr if it is not a known zone.
	//! Identifiers of the form GMT[+-]hh[:mm[:ss]] describe a zone with a fixed offset.
	static unique_ptr<TimeZone> TryCreate(const string &id);
	//! The identifiers of all known time zones, in lexicographic order
	static const vector<string> &GetAvailableIds();
	//! The identifiers that refer to the same data as the given zone, including the zone itself.
	//! Returns an empty list if the zone has no aliases.
	static vector<string> GetEquivalentIds(const string &id);
	//! The time zone of the host, or nullptr if it cannot be determined
	static unique_ptr<TimeZone> TryCreateDefault();

	const string &GetId() const {
		return id;
	}
	//! The offsets that apply at an instant
	virtual void GetOffset(int64_t millis, int32_t &raw_offset, int32_t &dst_offset) const = 0;
	//! The offsets that apply at a local (wall clock) time
	virtual void GetOffsetFromLocal(int64_t millis, LocalOption non_existing, LocalOption duplicated,
	                                int32_t &raw_offset, int32_t &dst_offset) const = 0;
	//! The first instant after 'millis' at which the offsets can change, or false if there is none
	virtual bool TryGetNextTransition(int64_t millis, int64_t &transition) const = 0;
	//! Whether the two zones describe the same offsets. Aliases of one another are equivalent.
	bool Equals(const TimeZone &other) const {
		return id == other.id;
	}
	virtual unique_ptr<TimeZone> Copy() const = 0;

protected:
	explicit TimeZone(string id_p) : id(std::move(id_p)) {
	}

	string id;
};

//! A zone with a fixed standard offset and at most one recurring daylight savings rule.
//! This is used both for the GMT[+-]hh:mm zones and for extrapolating beyond the last
//! transition that the time zone database has data for.
class SimpleTimeZone : public TimeZone {
public:
	SimpleTimeZone(string id, int32_t raw_offset);
	SimpleTimeZone(string id, int32_t raw_offset, const TZRule &rule);

	void GetOffset(int64_t millis, int32_t &raw_offset, int32_t &dst_offset) const override;
	void GetOffsetFromLocal(int64_t millis, LocalOption non_existing, LocalOption duplicated, int32_t &raw_offset,
	                        int32_t &dst_offset) const override;
	bool TryGetNextTransition(int64_t millis, int64_t &transition) const override;
	unique_ptr<TimeZone> Copy() const override;

private:
	//! How the time of a rule is expressed
	enum class TimeMode : uint8_t { WALL = 0, STANDARD = 1, UTC = 2 };
	//! How the day of a rule is selected
	enum class DayMode : uint8_t { DAY_OF_MONTH, DAY_OF_WEEK_IN_MONTH, DAY_OF_WEEK_GE_DOM, DAY_OF_WEEK_LE_DOM };

	//! One end of the daylight savings period
	struct Boundary {
		int8_t month;
		int8_t day;
		int8_t day_of_week;
		int32_t time;
		TimeMode time_mode;
		DayMode day_mode;
	};

	static Boundary DecodeRule(int8_t month, int8_t day, int8_t day_of_week, int32_t time, int8_t time_mode);
	//! Compares a date within a year to a rule: 1 if it is after, 0 if equal and -1 if before
	static int32_t CompareToRule(int8_t month, int8_t month_len, int8_t prev_month_len, int8_t dom, int8_t dow,
	                             int32_t millis, int32_t millis_delta, const Boundary &rule);
	//! The total offset that applies to a local standard time
	int32_t GetOffsetForFields(int32_t year, int8_t month, int8_t dom, int8_t dow, int32_t millis) const;
	//! The instant at which a rule takes effect in a year
	int64_t GetRuleTime(int32_t year, const Boundary &rule, bool is_start) const;

	int32_t raw_offset;
	bool use_daylight;
	int32_t dst_savings;
	Boundary start;
	Boundary end;
};

//! A zone backed by the time zone database: a list of transitions between offsets, optionally
//! followed by a recurring rule that applies from the last transition onwards.
class OlsonTimeZone : public TimeZone {
public:
	OlsonTimeZone(string id, const TZZoneData &data);

	void GetOffset(int64_t millis, int32_t &raw_offset, int32_t &dst_offset) const override;
	void GetOffsetFromLocal(int64_t millis, LocalOption non_existing, LocalOption duplicated, int32_t &raw_offset,
	                        int32_t &dst_offset) const override;
	bool TryGetNextTransition(int64_t millis, int64_t &transition) const override;
	unique_ptr<TimeZone> Copy() const override;

private:
	//! The offsets around a transition. Index -1 refers to the offsets before the first transition.
	const TZTypeOffset &OffsetsAt(int32_t transition_index) const;
	//! The offsets at an instant (local == false) or a local time (local == true)
	void GetHistoricalOffset(int64_t millis, bool local, LocalOption non_existing, LocalOption duplicated,
	                         int32_t &raw_offset, int32_t &dst_offset) const;

	const TZZoneData &data;
	//! The rule that applies after the transition data runs out, if any
	unique_ptr<SimpleTimeZone> final_zone;
	//! The first instant that final_zone applies to
	int64_t final_start_millis;
};

} // namespace datetime
} // namespace duckdb
