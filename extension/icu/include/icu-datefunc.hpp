//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-datefunc.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/function.hpp"
#include "tz_calendar.hpp"

namespace duckdb {

struct ICUDateFunc {
	struct BindData : public FunctionData {
		explicit BindData(ClientContext &context);
		BindData(const string &tz_setting, const string &cal_setting);
		BindData(const BindData &other);

		string tz_setting;
		string cal_setting;
		CalendarPtr calendar;

		bool Equals(const FunctionData &other_p) const override;
		duckdb::unique_ptr<FunctionData> Copy() const override;

		void InitCalendar();
	};

	struct CastData : public BoundCastData {
		explicit CastData(duckdb::unique_ptr<FunctionData> info_p) : info(std::move(info_p)) {
		}

		duckdb::unique_ptr<BoundCastData> Copy() const override {
			return make_uniq<CastData>(info->Copy());
		}

		duckdb::unique_ptr<FunctionData> info;
	};

	template <typename SRC, typename DST>
	static inline DST TruncCast(const SRC &src) {
		DST dst;
		if (SRC::PRECISION == DST::PRECISION) {
			dst.value = src.value;
		} else {
			//	Truncate towards -infinity, not epoch.
			D_ASSERT(SRC::PRECISION > DST::PRECISION);
			const int64_t scaling = SRC::PRECISION / DST::PRECISION;
			int64_t frac;
			Timestamp::SplitTime(src.value, scaling, dst.value, frac);
		}
		return dst;
	}

	//! Per-thread cache of resolved time zones. Resolving a time zone name looks the name up and
	//! then builds a zone and a calendar for it, so functions with per-row time zone arguments
	//! must resolve each distinct zone only once per thread.
	struct CalendarCacheState : public FunctionLocalState {
		struct CacheEntry {
			//! The resolved time zone (nullptr if the name is invalid)
			duckdb::unique_ptr<TimeZone> tz;
			//! Copy of the base calendar with the time zone applied (nullptr if the name is invalid)
			CalendarPtr calendar;
			//! The resolution error for invalid time zone names
			string error;
		};

		explicit CalendarCacheState(Calendar &base) : base_calendar(base.Copy()) {
		}

		//! Returns a calendar for the time zone. Throws for unknown zones.
		Calendar *GetCalendar(const string_t &tz_id);
		//! Returns a calendar for the time zone, or nullptr for unknown zones (filling error_message if provided).
		Calendar *TryGetCalendar(const string_t &tz_id, string *error_message = nullptr);
		//! Sets the time zone for the calendar.
		//! For unknown zones, fills error_message if provided and throws otherwise.
		void SetTimeZone(Calendar *calendar, const string_t &tz_id, string *error_message = nullptr);
		//! Sets the time zone for the calendar and returns false if it is not valid.
		bool TrySetTimeZone(Calendar *calendar, const string_t &tz_id);

		Calendar *GetBaseCalendar() {
			return base_calendar.get();
		}

	private:
		const CacheEntry &GetEntry(const string_t &tz_id);

		CalendarPtr base_calendar;
		unordered_map<string, CacheEntry> cache;
		//! Exploit locality for timestamp rows
		const std::pair<const string, CacheEntry> *last = nullptr;
	};

	//! Binds a default calendar object for use by the function
	static duckdb::unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input);
	//! Binds the calendar of the TimeZone and Calendar settings for a cast, whose result then depends on them
	static duckdb::unique_ptr<CastData> BindCastData(BindCastInput &input);
	//! Initializes a CalendarCacheState from the function's BindData
	static duckdb::unique_ptr<FunctionLocalState>
	InitCalendarCache(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data);
	//! Initializes a CalendarCacheState from a cast's CastData
	static duckdb::unique_ptr<FunctionLocalState> InitCastCalendarCache(CastLocalStateParameters &parameters);

	//! Gets the timestamp from the calendar, throwing if it is not in range.
	static bool TryGetTime(Calendar *calendar, uint64_t micros, timestamp_tz_t &result);
	//! Gets the timestamp from the calendar, throwing if it is not in range.
	static timestamp_tz_t GetTime(Calendar *calendar, uint64_t micros = 0);
	//! Gets the timestamp from the calendar, throwing if it is not in range.
	static bool TryGetTimeNS(Calendar *calendar, uint64_t nanos, timestamp_tz_ns_t &result);
	//! Gets the timestamp from the calendar, throwing if it is not in range.
	static timestamp_tz_ns_t GetTimeNS(Calendar *calendar, uint64_t micros = 0);
	//! Gets the timestamp from the calendar, assuming it is in range.
	static timestamp_tz_t GetTimeUnsafe(Calendar *calendar, uint64_t micros = 0);
	//! Sets the calendar to the timestamp, returning the unused µs part
	static uint64_t SetTime(Calendar *calendar, timestamp_tz_t date);
	//! Sets the calendar to the timestamp, returning the unused ns part
	static uint64_t SetTimeNS(Calendar *calendar, timestamp_tz_ns_t date);
	//! Extracts the field from the calendar
	static int32_t ExtractField(Calendar *calendar, CalendarField field);
	//! Extracts the UTC offset (in seconds) of the calendar, returns false if a TIMETZ cannot represent it
	static bool TryGetTimeTZOffset(Calendar *calendar, int32_t &offset);
	//! Extracts the UTC offset (in seconds) of the calendar, throws if a TIMETZ cannot represent it
	static int32_t GetTimeTZOffset(Calendar *calendar);
	static string TimeTZOffsetError(int32_t offset);
	//! Subtracts the field of the given date from the calendar
	static int32_t SubtractField(Calendar *calendar, CalendarField field, timestamp_tz_t end_date);
	//! Adds the timestamp and the interval using the calendar
	static timestamp_tz_t Add(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval);
	//! Subtracts the interval from the timestamp using the calendar
	static timestamp_tz_t Sub(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval);
	//! Subtracts the latter timestamp from the former timestamp using the calendar
	static interval_t Sub(TZCalendar &calendar, timestamp_tz_t end_date, timestamp_tz_t start_date);
	//! Pulls out the bin values from the timestamp assuming it is an instant,
	//! constructs an ICU timestamp, and then converts that back to a DuckDB instant
	//! Adding offset doesn't really work around DST because the bin values are ambiguous
	static timestamp_tz_t FromNaive(Calendar *calendar, timestamp_t naive);
	//! Non-throwing form of FromNaive - sets the error and returns false if the result is out of range
	static bool TryFromNaive(Calendar *calendar, timestamp_t naive, timestamp_tz_t &result, string &error);

	//! Truncates the calendar time to the given part precision
	typedef void (*part_trunc_t)(Calendar *calendar, uint64_t &micros);
	static part_trunc_t TruncationFactory(DatePartSpecifier part);
	static timestamp_tz_t CurrentMidnight(Calendar *calendar, ExpressionState &state);

	//! Subtracts the two times at the given part precision
	typedef int64_t (*part_sub_t)(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date);
	static part_sub_t SubtractFactory(DatePartSpecifier part);
};

} // namespace duckdb
