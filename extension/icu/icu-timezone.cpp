#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast_rules.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "include/icu-casts.hpp"
#include "include/icu-datefunc.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

struct ICUTimeZoneData : public GlobalTableFunctionState {
	ICUTimeZoneData() : now(Calendar::GetNow()) {
	}

	idx_t offset = 0;
	int64_t now;
};

static duckdb::unique_ptr<FunctionData> ICUTimeZoneBind(ClientContext &context, TableFunctionBindInput &input,
                                                        vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("abbrev");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("utc_offset");
	return_types.emplace_back(LogicalType::INTERVAL);
	names.emplace_back("is_dst");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return nullptr;
}

static duckdb::unique_ptr<GlobalTableFunctionState> ICUTimeZoneInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<ICUTimeZoneData>();
}

static void ICUTimeZoneFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ICUTimeZoneData>();
	const auto &names = TimeZone::GetAvailableIds();
	idx_t index = 0;

	// name, VARCHAR
	auto &name_col = output.data[0];
	// abbrev, VARCHAR
	auto &abbrev = output.data[1];
	// utc_offset, INTERVAL
	auto &utc_offset = output.data[2];
	// is_dst, BOOLEAN
	auto &is_dst = output.data[3];

	while (index < STANDARD_VECTOR_SIZE && data.offset < names.size()) {
		//	The LONG name is the one we looked up
		const auto &long_id = names[data.offset++];

		//	We don't have the zone tree for determining abbreviated names,
		//	so the SHORT name is the shortest, lexicographically first equivalent TZ without a slash.
		auto short_id = long_id;
		for (const auto &eid : TimeZone::GetEquivalentIds(long_id)) {
			if (eid.find('/') != string::npos) {
				continue;
			}
			if (eid.size() < short_id.size() || (eid.size() == short_id.size() && eid < short_id)) {
				short_id = eid;
			}
		}

		auto tz = TimeZone::TryCreate(long_id);
		int32_t raw_offset_ms;
		int32_t dst_offset_ms;
		tz->GetOffset(data.now, raw_offset_ms, dst_offset_ms);

		name_col.Append(Value(long_id));
		abbrev.Append(Value(short_id));
		//	What PG reports is the total offset for today,
		//	which is the total offset (i.e., "raw") plus the DST offset.
		raw_offset_ms += dst_offset_ms;
		utc_offset.Append(Value::INTERVAL(Interval::FromMicro(raw_offset_ms * Interval::MICROS_PER_MSEC)));
		is_dst.Append(Value::BOOLEAN(dst_offset_ms != 0));
		++index;
	}
}

struct ICUFromNaiveTimestamp : public ICUDateFunc {
	//! Non-throwing form, so a TRY_CAST can turn the failure into NULL rather than an error.
	static bool TryOperation(Calendar *calendar, timestamp_t naive, timestamp_tz_t &result, string &error) {
		if (!naive.IsFinite()) {
			result = timestamp_tz_t(naive);
			return true;
		}
		SetTimeParts(calendar, naive);
		if (!TryGetTime(calendar, GetMicrosPart(naive), result)) {
			error = "ICU date overflows timestamp range";
			return false;
		}
		return true;
	}

	static bool TryOperation(Calendar *calendar, timestamp_ns_t naive, timestamp_tz_ns_t &result, string &error) {
		if (!naive.IsFinite()) {
			result = timestamp_tz_ns_t(naive);
			return true;
		}

		auto nanos = UnsafeNumericCast<int32_t>(naive.value % Interval::NANOS_PER_MICRO);
		timestamp_t micros(naive.value / Interval::NANOS_PER_MICRO);
		timestamp_tz_t cast;
		if (!TryOperation(calendar, micros, cast, error)) {
			return false;
		}

		timestamp_ns_t scaled;
		if (!Timestamp::TryFromTimestampNanos(timestamp_t(cast), nanos, scaled)) {
			error = "ICU date overflows timestamp_ns range";
			return false;
		}
		result = timestamp_tz_ns_t(scaled);
		return true;
	}

	//! Shared by both forms: push the naive parts into the calendar, and recover the sub-millisecond remainder.
	static void SetTimeParts(Calendar *calendar, timestamp_t naive) {
		date_t local_date;
		dtime_t local_time;
		Timestamp::Convert(naive, local_date, local_time);

		int32_t year, mm, dd;
		Date::Convert(local_date, year, mm, dd);

		int32_t hr, mn, secs, frac;
		Time::Convert(local_time, hr, mn, secs, frac);
		const int32_t millis = frac / int32_t(Interval::MICROS_PER_MSEC);

		calendar->Set(CAL_YEAR, year);
		calendar->Set(CAL_MONTH, int32_t(mm - 1));
		calendar->Set(CAL_DATE, dd);
		calendar->Set(CAL_HOUR_OF_DAY, hr);
		calendar->Set(CAL_MINUTE, mn);
		calendar->Set(CAL_SECOND, secs);
		calendar->Set(CAL_MILLISECOND, millis);
	}

	static uint64_t GetMicrosPart(timestamp_t naive) {
		date_t local_date;
		dtime_t local_time;
		Timestamp::Convert(naive, local_date, local_time);
		int32_t hr, mn, secs, frac;
		Time::Convert(local_time, hr, mn, secs, frac);
		return uint64_t(frac % Interval::MICROS_PER_MSEC);
	}

	static inline timestamp_tz_t Operation(Calendar *calendar, timestamp_t naive) {
		if (!naive.IsFinite()) {
			return timestamp_tz_t(naive);
		}

		// Extract the parts from the "instant"
		date_t local_date;
		dtime_t local_time;
		Timestamp::Convert(naive, local_date, local_time);

		int32_t year;
		int32_t mm;
		int32_t dd;
		Date::Convert(local_date, year, mm, dd);

		int32_t hr;
		int32_t mn;
		int32_t secs;
		int32_t frac;
		Time::Convert(local_time, hr, mn, secs, frac);
		int32_t millis = frac / int32_t(Interval::MICROS_PER_MSEC);
		uint64_t micros = frac % Interval::MICROS_PER_MSEC;

		// Use them to set the time in the time zone
		calendar->Set(CAL_YEAR, year);
		calendar->Set(CAL_MONTH, int32_t(mm - 1));
		calendar->Set(CAL_DATE, dd);
		calendar->Set(CAL_HOUR_OF_DAY, hr);
		calendar->Set(CAL_MINUTE, mn);
		calendar->Set(CAL_SECOND, secs);
		calendar->Set(CAL_MILLISECOND, millis);

		return GetTime(calendar, micros);
	}

	static inline timestamp_tz_ns_t Operation(Calendar *calendar, timestamp_ns_t naive) {
		if (!naive.IsFinite()) {
			return timestamp_tz_ns_t(naive);
		}

		auto nanos = UnsafeNumericCast<int32_t>(naive.value % Interval::NANOS_PER_MICRO);
		timestamp_t micros(naive.value / Interval::NANOS_PER_MICRO);
		timestamp_t cast(Operation(calendar, micros));

		timestamp_ns_t result;
		if (!Timestamp::TryFromTimestampNanos(cast, nanos, result)) {
			throw ConversionException("ICU date overflows timestamp_ns range");
		}
		return timestamp_tz_ns_t(result);
	}

	template <class SRC, class DST>
	static bool CastFromNaive(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &cast_data = parameters.cast_data->Cast<CastData>();
		auto &info = cast_data.info->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		bool all_converted = true;
		UnaryExecutor::Execute<SRC, DST>(source, result, count, [&](SRC input) -> optional<DST> {
			using NAIVE = timestamp_base_t<DST::PRECISION, false>;
			NAIVE naive;
			if (!TryCast::Operation(input, naive)) {
				HandleCastError::AssignError(CastExceptionText<SRC, NAIVE>(input), parameters);
				all_converted = false;
				return nullopt;
			}
			// the zone shift can overflow, and a TRY_CAST must see that as NULL rather than an exception
			DST shifted;
			string error;
			if (!TryOperation(calendar.get(), naive, shifted, error)) {
				HandleCastError::AssignError(error, parameters);
				all_converted = false;
				return nullopt;
			}
			return shifted;
		});
		return all_converted;
	}

	template <typename SRC>
	static BoundCastInfo BindCastFromNaiveType(BindCastInput &input, const LogicalType &target) {
		auto cast_data = BindCastData(input);
		switch (target.id()) {
		case LogicalTypeId::TIMESTAMP_TZ:
			return BoundCastInfo(CastFromNaive<SRC, timestamp_tz_t>, std::move(cast_data));
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return BoundCastInfo(CastFromNaive<SRC, timestamp_tz_ns_t>, std::move(cast_data));
		default:
			throw InternalException("Type %s not handled in BindCastFromNaiveType", LogicalTypeIdToString(target.id()));
		}
	}

	static BoundCastInfo BindCastFromNaive(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
		if (!input.context) {
			throw InternalException("Missing context for TIMESTAMP to TIMESTAMPTZ cast.");
		}
		if (Settings::Get<DisableTimestamptzCastsSetting>(*input.context)) {
			throw BinderException("Casting from TIMESTAMP to TIMESTAMP WITH TIME ZONE without an explicit time zone "
			                      "has been disabled  - use \"AT TIME ZONE ...\"");
		}

		switch (source.id()) {
		case LogicalTypeId::TIMESTAMP:
			return BindCastFromNaiveType<timestamp_t>(input, target);
		case LogicalTypeId::TIMESTAMP_MS:
			return BindCastFromNaiveType<timestamp_ms_t>(input, target);
		case LogicalTypeId::TIMESTAMP_NS:
			return BindCastFromNaiveType<timestamp_ns_t>(input, target);
		case LogicalTypeId::TIMESTAMP_SEC:
			return BindCastFromNaiveType<timestamp_sec_t>(input, target);
		case LogicalTypeId::DATE:
			return BindCastFromNaiveType<date_t>(input, target);
		default:
			throw InternalException("Type %s not handled in BindCastFromNaive", LogicalTypeIdToString(source.id()));
		}
	}
	static void AddCast(CastFunctionSet &casts, const LogicalType &source, const LogicalType &target) {
		const auto implicit_cost = CastRules::ImplicitCast(source, target);
		casts.RegisterCastFunction(source, target, BindCastFromNaive, implicit_cost);
	}

	static void AddCasts(ExtensionLoader &loader) {
		auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
		auto &casts = config.GetCastFunctions();

		AddCast(casts, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP_TZ);
		AddCast(casts, LogicalType::TIMESTAMP_MS, LogicalType::TIMESTAMP_TZ);
		AddCast(casts, LogicalType::TIMESTAMP_NS, LogicalType::TIMESTAMP_TZ);
		AddCast(casts, LogicalType::TIMESTAMP_NS, LogicalType::TIMESTAMP_TZ_NS);
		AddCast(casts, LogicalType::TIMESTAMP_S, LogicalType::TIMESTAMP_TZ);
		AddCast(casts, LogicalType::DATE, LogicalType::TIMESTAMP_TZ);
	}
};

struct ICUToNaiveTimestamp : public ICUDateFunc {
	//! Non-throwing form, so a TRY_CAST can turn the failure into NULL rather than an error. The error text is
	//! returned for the throwing form and for CastParameters, keeping both spellings identical.
	static bool TryOperation(Calendar *calendar, timestamp_tz_t instant, timestamp_t &naive, string &error) {
		if (!instant.IsFinite()) {
			naive = timestamp_t(instant);
			return true;
		}

		// Extract the time zone parts
		auto micros = int32_t(SetTime(calendar, instant));
		const auto era = ExtractField(calendar, CAL_ERA);
		const auto year = ExtractField(calendar, CAL_YEAR);
		const auto mm = ExtractField(calendar, CAL_MONTH) + 1;
		const auto dd = ExtractField(calendar, CAL_DATE);

		const auto yyyy = era ? year : (-year + 1);
		date_t local_date;
		if (!Date::TryFromDate(yyyy, mm, dd, local_date)) {
			error = "Unable to convert TIMESTAMPTZ to local date";
			return false;
		}

		const auto hr = ExtractField(calendar, CAL_HOUR_OF_DAY);
		const auto mn = ExtractField(calendar, CAL_MINUTE);
		const auto secs = ExtractField(calendar, CAL_SECOND);
		const auto millis = ExtractField(calendar, CAL_MILLISECOND);

		micros += millis * int32_t(Interval::MICROS_PER_MSEC);
		dtime_t local_time = Time::FromTime(hr, mn, secs, micros);

		if (!Timestamp::TryFromDatetime(local_date, local_time, naive)) {
			error = "Unable to convert TIMESTAMPTZ to local TIMESTAMP";
			return false;
		}

		return true;
	}

	static inline timestamp_t Operation(Calendar *calendar, timestamp_tz_t instant) {
		timestamp_t naive;
		string error;
		if (!TryOperation(calendar, instant, naive, error)) {
			throw ConversionException(error);
		}
		return naive;
	}

	static bool TryOperation(Calendar *calendar, timestamp_tz_ns_t instant, timestamp_ns_t &result, string &error) {
		if (!instant.IsFinite()) {
			result = timestamp_ns_t(instant);
			return true;
		}

		auto nanos = UnsafeNumericCast<int32_t>(instant.value % Interval::NANOS_PER_MICRO);
		timestamp_tz_t micros(instant.value / Interval::NANOS_PER_MICRO);
		timestamp_t cast;
		if (!TryOperation(calendar, micros, cast, error)) {
			return false;
		}

		if (!Timestamp::TryFromTimestampNanos(cast, nanos, result)) {
			error = "ICU date overflows timestamp_ns range";
			return false;
		}
		return true;
	}

	static inline timestamp_ns_t Operation(Calendar *calendar, timestamp_tz_ns_t instant) {
		timestamp_ns_t result;
		string error;
		if (!TryOperation(calendar, instant, result, error)) {
			throw ConversionException(error);
		}
		return result;
	}

	template <class SRC, class DST>
	static bool CastToNaive(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &cast_data = parameters.cast_data->Cast<CastData>();
		auto &info = cast_data.info->Cast<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		bool all_converted = true;
		UnaryExecutor::Execute<SRC, DST>(source, result, count, [&](SRC input) -> optional<DST> {
			using NAIVE = timestamp_base_t<SRC::PRECISION, false>;
			// the zone shift itself can fail, and a TRY_CAST must see that as NULL rather than an exception
			NAIVE naive;
			string error;
			if (!TryOperation(calendar.get(), input, naive, error)) {
				HandleCastError::AssignError(error, parameters);
				all_converted = false;
				return nullopt;
			}
			DST output;
			if (!TryCast::Operation(naive, output)) {
				HandleCastError::AssignError("Could not convert Timestamp to higher precision.", parameters);
				all_converted = false;
				return nullopt;
			}
			return output;
		});
		return all_converted;
	}

	static BoundCastInfo BindCastToNaive(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
		if (!input.context) {
			throw InternalException("Missing context for TIMESTAMPTZ to %s cast.", LogicalTypeIdToString(target.id()));
		}
		if (Settings::Get<DisableTimestamptzCastsSetting>(*input.context)) {
			throw BinderException("Casting from TIMESTAMP WITH TIME ZONE to %s without an explicit time zone "
			                      "has been disabled  - use \"AT TIME ZONE ...\"",
			                      LogicalTypeIdToString(target.id()));
		}

		auto cast_data = BindCastData(input);

		switch (source.id()) {
		case LogicalTypeId::TIMESTAMP_TZ:
			switch (target.id()) {
			case LogicalType::TIMESTAMP:
				return BoundCastInfo(CastToNaive<timestamp_tz_t, timestamp_t>, std::move(cast_data));
			case LogicalType::TIMESTAMP_MS:
				return BoundCastInfo(CastToNaive<timestamp_tz_t, timestamp_ms_t>, std::move(cast_data));
			case LogicalType::TIMESTAMP_NS:
				return BoundCastInfo(CastToNaive<timestamp_tz_t, timestamp_ns_t>, std::move(cast_data));
			case LogicalType::TIMESTAMP_S:
				return BoundCastInfo(CastToNaive<timestamp_tz_t, timestamp_sec_t>, std::move(cast_data));
			default:
				throw InternalException("Type %s not handled in BindCastToNaive", LogicalTypeIdToString(target.id()));
			}
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			switch (target.id()) {
			case LogicalType::TIMESTAMP_NS:
				return BoundCastInfo(CastToNaive<timestamp_tz_ns_t, timestamp_ns_t>, std::move(cast_data));
			default:
				throw InternalException("Type %s not handled in BindCastToNaive", LogicalTypeIdToString(target.id()));
			}
		default:
			throw InternalException("Type %s not handled in BindCastToNaive", LogicalTypeIdToString(source.id()));
		}
	}

	static void AddCast(CastFunctionSet &casts, const LogicalType &source, const LogicalType &target) {
		const auto implicit_cost = CastRules::ImplicitCast(source, target);
		casts.RegisterCastFunction(source, target, BindCastToNaive, implicit_cost);
	}

	static void AddCasts(ExtensionLoader &loader) {
		auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
		auto &casts = config.GetCastFunctions();

		AddCast(casts, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP);
		AddCast(casts, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_MS);
		AddCast(casts, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_NS);
		AddCast(casts, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_S);

		AddCast(casts, LogicalType::TIMESTAMP_TZ_NS, LogicalType::TIMESTAMP_NS);
	}
};

struct ICULocalTimestampFunc : public ICUDateFunc {
	struct BindDataNow : public BindData {
		explicit BindDataNow(ClientContext &context) : BindData(context) {
			now = MetaTransaction::Get(context).start_timestamp;
		}

		BindDataNow(const BindDataNow &other) : BindData(other), now(other.now) {
		}

		bool Equals(const FunctionData &other_p) const override {
			auto &other = other_p.Cast<const BindDataNow>();
			if (now != other.now) {
				return false;
			}

			return BindData::Equals(other_p);
		}

		duckdb::unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindDataNow>(*this);
		}

		timestamp_t now;
	};

	static duckdb::unique_ptr<FunctionData> BindNow(BindScalarFunctionInput &input) {
		return make_uniq<BindDataNow>(input.GetClientContext());
	}

	static timestamp_t GetLocalTimestamp(ExpressionState &state) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindDataNow>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		const auto now = timestamp_tz_t(info.now);
		return ICUToNaiveTimestamp::Operation(calendar, now);
	}

	static void Execute(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() == 0);
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		auto rdata = ConstantVector::GetData<timestamp_t>(result);
		rdata[0] = GetLocalTimestamp(state);
	}

	static void AddFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(ScalarFunction({}, LogicalType::TIMESTAMP, Execute, BindNow));
		// the bind reads the start of the transaction
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}
};

struct ICULocalTimeFunc : public ICUDateFunc {
	static void Execute(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() == 0);
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		auto rdata = ConstantVector::GetData<dtime_t>(result);
		const auto local = ICULocalTimestampFunc::GetLocalTimestamp(state);
		rdata[0] = Timestamp::GetTime(local);
	}

	static void AddFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(ScalarFunction({}, LogicalType::TIME, Execute, ICULocalTimestampFunc::BindNow));
		// the bind reads the start of the transaction
		set.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
		loader.RegisterFunction(set);
	}
};

dtime_tz_t ICUToTimeTZ::Operation(Calendar *calendar, dtime_tz_t timetz) {
	// Normalise to +00:00, add TZ offset, then set offset to TZ
	auto time = Time::NormalizeTimeTZ(timetz);

	auto offset = GetTimeTZOffset(calendar);

	date_t date(0);
	time = Interval::Add(time, {0, 0, offset * Interval::MICROS_PER_SEC}, date);
	return dtime_tz_t(time, offset);
}

bool ICUToTimeTZ::ToTimeTZ(Calendar *calendar, timestamp_tz_t instant, dtime_tz_t &result,
                           optional_ptr<string> error_message) {
	if (!instant.IsFinite()) {
		if (error_message) {
			*error_message = "Can't get TIME of infinite TIMESTAMP";
		}
		return false;
	}

	//	Time in current TZ
	auto micros = int32_t(SetTime(calendar, instant));
	const auto hour = ExtractField(calendar, CAL_HOUR_OF_DAY);
	const auto minute = ExtractField(calendar, CAL_MINUTE);
	const auto second = ExtractField(calendar, CAL_SECOND);
	const auto millis = ExtractField(calendar, CAL_MILLISECOND);
	micros += millis * int32_t(Interval::MICROS_PER_MSEC);
	if (!Time::IsValidTime(hour, minute, second, micros)) {
		return false;
	}
	const auto time = Time::FromTime(hour, minute, second, micros);

	//	Offset in current TZ
	int32_t offset;
	if (!error_message) {
		offset = GetTimeTZOffset(calendar);
	} else if (!TryGetTimeTZOffset(calendar, offset)) {
		*error_message = TimeTZOffsetError(offset);
		return false;
	}

	result = dtime_tz_t(time, offset);
	return true;
}

bool ICUToTimeTZ::CastToTimeTZ(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<CastData>();
	auto &info = cast_data.info->Cast<BindData>();
	CalendarPtr calendar(info.calendar->Copy());

	UnaryExecutor::Execute<timestamp_tz_t, dtime_tz_t>(source, result, count,
	                                                   [&](timestamp_tz_t input) -> optional<dtime_tz_t> {
		                                                   dtime_tz_t output;
		                                                   string error_message;
		                                                   if (ToTimeTZ(calendar.get(), input, output, error_message)) {
			                                                   return output;
		                                                   }
		                                                   if (!error_message.empty()) {
			                                                   HandleCastError::AssignError(error_message, parameters);
		                                                   }
		                                                   return nullopt;
	                                                   });
	return true;
}

BoundCastInfo ICUToTimeTZ::BindCastToTimeTZ(BindCastInput &input, const LogicalType &source,
                                            const LogicalType &target) {
	if (!input.context) {
		throw InternalException("Missing context for TIMESTAMPTZ to TIMETZ cast.");
	}

	auto cast_data = BindCastData(input);

	return BoundCastInfo(CastToTimeTZ, std::move(cast_data));
}

bool ICUToTimeTZ::CastFromTime(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<CastData>();
	auto &info = cast_data.info->Cast<BindData>();
	CalendarPtr calendar_ptr(info.calendar->Copy());
	auto calendar = calendar_ptr.get();

	// Read the session UTC offset (with DST) from the calendar.
	// This mirrors the no-offset branch in ICUStrptime::VarcharToTimeTZ so that
	// '00:00:00'::TIME::TIMETZ matches '00:00:00'::TIMETZ.
	int32_t offset;
	if (!TryGetTimeTZOffset(calendar, offset)) {
		auto error_message = TimeTZOffsetError(offset);
		UnaryExecutor::Execute<dtime_t, dtime_tz_t>(source, result, count, [&](dtime_t input) -> optional<dtime_tz_t> {
			HandleCastError::AssignError(error_message, parameters);
			return nullopt;
		});
		return false;
	}

	UnaryExecutor::Execute<dtime_t, dtime_tz_t>(source, result, count,
	                                            [&](dtime_t input) { return dtime_tz_t(input, offset); });
	return true;
}

BoundCastInfo ICUToTimeTZ::BindCastFromTime(BindCastInput &input, const LogicalType &source,
                                            const LogicalType &target) {
	if (!input.context) {
		throw InternalException("Missing context for TIME to TIMETZ cast.");
	}

	auto cast_data = BindCastData(input);

	return BoundCastInfo(CastFromTime, std::move(cast_data));
}

void ICUToTimeTZ::AddCasts(ExtensionLoader &loader) {
	const auto implicit_cost = CastRules::ImplicitCast(LogicalType::TIMESTAMP_TZ, LogicalType::TIME_TZ);
	loader.RegisterCastFunction(LogicalType::TIMESTAMP_TZ, LogicalType::TIME_TZ, BindCastToTimeTZ, implicit_cost);

	const auto time_implicit_cost = CastRules::ImplicitCast(LogicalType::TIME, LogicalType::TIME_TZ);
	loader.RegisterCastFunction(LogicalType::TIME, LogicalType::TIME_TZ, BindCastFromTime, time_implicit_cost);
}

struct ICUTimeZoneFunc : public ICUDateFunc {
	template <typename OP, typename SRC, typename DST>
	static void Execute(DataChunk &input, ExpressionState &state, Vector &result) {
		auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<CalendarCacheState>();

		// Two cases: constant TZ, variable TZ
		D_ASSERT(input.ColumnCount() == 2);
		const auto &tz_vec = input.data[0];
		const auto &ts_vec = input.data[1];
		if (tz_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (ConstantVector::IsNull(tz_vec)) {
				throw InternalException("ICUTimeZone called with constant NULL tz");
			}
			auto calendar = cache.GetCalendar(*ConstantVector::GetData<string_t>(tz_vec));
			UnaryExecutor::Execute<SRC, DST>(ts_vec, result, [&](SRC ts) { return OP::Operation(calendar, ts); });
		} else {
			BinaryExecutor::Execute<string_t, SRC, DST>(tz_vec, ts_vec, result, [&](string_t tz_id, SRC ts) {
				if (ts.IsFinite()) {
					return OP::Operation(cache.GetCalendar(tz_id), ts);
				} else {
					return Cast::Operation<SRC, DST>(ts);
				}
			});
		}
	}

	static void AddFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		ScalarFunction ts_fun({}, LogicalType::TIMESTAMP_TZ,
		                      Execute<ICUFromNaiveTimestamp, timestamp_t, timestamp_tz_t>, Bind);
		ts_fun.GetSignature()
		    .AddParameter("timezone", LogicalType::VARCHAR)
		    .AddParameter("timestamp", LogicalType::TIMESTAMP);
		set.AddFunction(ts_fun);
		ScalarFunction tstz_fun({}, LogicalType::TIMESTAMP, Execute<ICUToNaiveTimestamp, timestamp_tz_t, timestamp_t>,
		                        Bind);
		tstz_fun.GetSignature()
		    .AddParameter("timezone", LogicalType::VARCHAR)
		    .AddParameter("timestamp", LogicalType::TIMESTAMP_TZ);
		set.AddFunction(tstz_fun);

		ScalarFunction timetz_fun({}, LogicalType::TIME_TZ, Execute<ICUToTimeTZ, dtime_tz_t, dtime_tz_t>, Bind);
		timetz_fun.GetSignature()
		    .AddParameter("timezone", LogicalType::VARCHAR)
		    .AddParameter("timestamp", LogicalType::TIME_TZ);
		set.AddFunction(timetz_fun);

		ScalarFunction timens_fun({}, LogicalType::TIMESTAMP_TZ_NS,
		                          Execute<ICUFromNaiveTimestamp, timestamp_ns_t, timestamp_tz_ns_t>, Bind);
		timens_fun.GetSignature()
		    .AddParameter("timezone", LogicalType::VARCHAR)
		    .AddParameter("timestamp", LogicalType::TIMESTAMP_NS);
		set.AddFunction(timens_fun);

		ScalarFunction timetzns_fun({}, LogicalType::TIMESTAMP_NS,
		                            Execute<ICUToNaiveTimestamp, timestamp_tz_ns_t, timestamp_ns_t>, Bind);
		timetzns_fun.GetSignature()
		    .AddParameter("timezone", LogicalType::VARCHAR)
		    .AddParameter("timestamp", LogicalType::TIMESTAMP_TZ_NS);
		set.AddFunction(timetzns_fun);

		set.ApplyToFunctions([](ScalarFunction &func) {
			func.SetFallible();
			func.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
			func.SetInitStateCallback(InitCalendarCache);
		});
		loader.RegisterFunction(set);
	}
};

timestamp_tz_t ICUDateFunc::FromNaive(Calendar *calendar, timestamp_t naive) {
	return ICUFromNaiveTimestamp::Operation(calendar, naive);
}

bool ICUDateFunc::TryFromNaive(Calendar *calendar, timestamp_t naive, timestamp_tz_t &result, string &error) {
	return ICUFromNaiveTimestamp::TryOperation(calendar, naive, result, error);
}

void RegisterICUTimeZoneFunctions(ExtensionLoader &loader) {
	//	Table functions
	TableFunction tz_names("pg_timezone_names", {}, ICUTimeZoneFunction, ICUTimeZoneBind, ICUTimeZoneInit);
	loader.RegisterFunction(tz_names);

	//	Scalar functions
	ICUTimeZoneFunc::AddFunction("timezone", loader);
	ICULocalTimestampFunc::AddFunction("current_localtimestamp", loader);
	ICULocalTimeFunc::AddFunction("current_localtime", loader);

	// 	Casts
	ICUFromNaiveTimestamp::AddCasts(loader);
	ICUToNaiveTimestamp::AddCasts(loader);
	ICUToTimeTZ::AddCasts(loader);
}

} // namespace duckdb
