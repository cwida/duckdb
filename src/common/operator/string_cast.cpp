#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Cast Numeric -> String
//===--------------------------------------------------------------------===//
template <>
string_t StringCast::Operation(bool input, StringHeap &heap) {
	if (input) {
		return heap.AddString("true", 4);
	} else {
		return heap.AddString("false", 5);
	}
}

template <>
string_t StringCast::Operation(int8_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<int8_t>(input, heap);
}

template <>
string_t StringCast::Operation(int16_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<int16_t>(input, heap);
}
template <>
string_t StringCast::Operation(int32_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<int32_t>(input, heap);
}

template <>
string_t StringCast::Operation(int64_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<int64_t>(input, heap);
}
template <>
duckdb::string_t StringCast::Operation(uint8_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<uint8_t>(input, heap);
}
template <>
duckdb::string_t StringCast::Operation(uint16_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<uint16_t>(input, heap);
}
template <>
duckdb::string_t StringCast::Operation(uint32_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<uint32_t>(input, heap);
}
template <>
duckdb::string_t StringCast::Operation(uint64_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<uint64_t>(input, heap);
}
template <>
duckdb::string_t StringCast::Operation(hugeint_t input, StringHeap &heap) {
	return NumericHelper::FormatSigned<hugeint_t>(input, heap);
}

template <>
string_t StringCast::Operation(float input, StringHeap &heap) {
	std::string s = duckdb_fmt::format("{}", input);
	return heap.AddString(s);
}

template <>
string_t StringCast::Operation(double input, StringHeap &heap) {
	std::string s = duckdb_fmt::format("{}", input);
	return heap.AddString(s);
}

template <>
string_t StringCast::Operation(interval_t input, StringHeap &heap) {
	char buffer[70] = {};
	idx_t length = IntervalToStringCast::Format(input, buffer);
	return heap.AddString(buffer, length);
}

template <>
duckdb::string_t StringCast::Operation(uhugeint_t input, StringHeap &heap) {
	return UhugeintToStringCast::Format(input, heap);
}

template <>
duckdb::string_t StringCast::Operation(date_t input, StringHeap &heap) {
	if (!input.IsFinite()) {
		return heap.AddString(Date::ToInfinity(input));
	}
	int32_t date[3];
	Date::Convert(input, date[0], date[1], date[2]);

	idx_t year_length;
	bool add_bc;
	idx_t length = DateToStringCast::Length(date, year_length, add_bc);

	string_t result = heap.EmptyString(length);
	auto data = result.GetDataWriteable();

	DateToStringCast::Format(data, date, year_length, add_bc);

	result.Finalize();
	return result;
}

template <typename T>
duckdb::string_t StringAsTime(const T input, StringHeap &heap) {
	int32_t picos = 0;
	dtime_t micros(input.value);
	if (micros.PRECISION < input.PRECISION) {
		const int64_t scaling = input.PRECISION / micros.PRECISION;
		picos = UnsafeNumericCast<int32_t>(input.value % scaling);
		picos *= scaling;
		micros.value /= scaling;
	} else {
		const int64_t scaling = micros.PRECISION / input.PRECISION;
		micros.value *= scaling;
	}
	int32_t time[4];
	Time::Convert(micros, time[0], time[1], time[2], time[3]);

	char micro_buffer[10] = {};
	char nano_buffer[6] = {};
	idx_t time_length = TimeToStringCast::Length(time, micro_buffer);
	idx_t nano_length = 0;
	if (picos) {
		//	If there are ps, we need all the µs
		if (!time[3]) {
			TimeToStringCast::FormatMicros(time[3], micro_buffer);
		}
		time_length = 15;
		nano_length = 6;
		nano_length -= NumericCast<idx_t>(TimeToStringCast::FormatMicros(picos, nano_buffer));
	}
	const idx_t length = time_length + nano_length;

	string_t result = heap.EmptyString(length);
	auto data = result.GetDataWriteable();

	TimeToStringCast::Format(data, time_length, time, micro_buffer);
	data += time_length;
	memcpy(data, nano_buffer, nano_length);
	D_ASSERT(data + nano_length <= result.GetDataWriteable() + length);

	result.Finalize();
	return result;
}

template <>
duckdb::string_t StringCast::Operation(dtime_t input, StringHeap &heap) {
	return StringAsTime(input, heap);
}

template <>
duckdb::string_t StringCast::Operation(dtime_ns_t input, StringHeap &heap) {
	return StringAsTime(input, heap);
}

template <typename T>
duckdb::string_t StringFromTimestamp(const T &input, StringHeap &heap) {
	if (!input.IsFinite()) {
		return heap.AddString(Date::ToInfinity(input));
	}

	date_t date_entry;
	dtime_t time_entry;
	int32_t picos = 0;
	if (T::PRECISION == timestamp_ns_t::PRECISION) {
		auto ns = Cast::Operation<T, timestamp_ns_t>(input);
		Timestamp::Convert(ns, date_entry, time_entry, picos);
		// Use picoseconds so we have 6 digits
		picos *= 1000;
	} else {
		auto us = Cast::Operation<T, timestamp_us_t>(input);
		Timestamp::Convert(us, date_entry, time_entry);
	}

	int32_t date[3], time[4];
	Date::Convert(date_entry, date[0], date[1], date[2]);
	Time::Convert(time_entry, time[0], time[1], time[2], time[3]);

	// format for timestamp is DATE TIME (separated by space)
	idx_t year_length;
	bool add_bc;
	char micro_buffer[6] = {};
	char nano_buffer[6] = {};
	idx_t date_length = DateToStringCast::Length(date, year_length, add_bc);
	idx_t time_length = TimeToStringCast::Length(time, micro_buffer);
	idx_t nano_length = 0;
	if (picos) {
		//	If there are ps, we need all the µs
		if (!time[3]) {
			TimeToStringCast::FormatMicros(time[3], micro_buffer);
		}
		time_length = 15;
		nano_length = 6;
		nano_length -= NumericCast<idx_t>(TimeToStringCast::FormatMicros(picos, nano_buffer));
	}
	const idx_t tz_length = T::TIMEZONED ? 3 : 0;
	const idx_t length = date_length + 1 + time_length + nano_length + tz_length;

	string_t result = heap.EmptyString(length);
	auto data = result.GetDataWriteable();

	DateToStringCast::Format(data, date, year_length, add_bc);
	data += date_length;
	*data++ = ' ';
	TimeToStringCast::Format(data, time_length, time, micro_buffer);
	data += time_length;
	memcpy(data, nano_buffer, nano_length);
	D_ASSERT(data + nano_length <= result.GetDataWriteable() + length);
	data += nano_length;

	if (T::TIMEZONED) {
		*data++ = '+';
		*data++ = '0';
		*data++ = '0';
	}

	result.Finalize();
	return result;
}

template <>
duckdb::string_t StringCast::Operation(timestamp_t input, StringHeap &heap) {
	return StringFromTimestamp<timestamp_t>(input, heap);
}

template <>
duckdb::string_t StringCast::Operation(timestamp_ns_t input, StringHeap &heap) {
	return StringFromTimestamp<timestamp_ns_t>(input, heap);
}

template <>
duckdb::string_t StringCast::Operation(timestamp_ms_t input, StringHeap &heap) {
	// Finite ms values scale losslessly to us; infinity is the same sentinel in both types.
	return StringCast::Operation(timestamp_t(input.IsFinite() ? input.value * 1000 : input.value), heap);
}

template <>
duckdb::string_t StringCast::Operation(timestamp_sec_t input, StringHeap &heap) {
	// Finite sec values scale losslessly to us; infinity is the same sentinel in both types.
	return StringCast::Operation(timestamp_t(input.IsFinite() ? input.value * 1000000 : input.value), heap);
}

template <>
duckdb::string_t StringCast::Operation(duckdb::string_t input, StringHeap &heap) {
	return heap.AddBlob(input);
}

template <>
string_t StringCast::Operation(dtime_tz_t input, StringHeap &heap) {
	int32_t time[4];
	Time::Convert(input.time(), time[0], time[1], time[2], time[3]);

	char micro_buffer[10] = {};
	const auto time_length = TimeToStringCast::Length(time, micro_buffer);
	idx_t length = time_length;

	const auto offset = input.offset();
	const bool negative = (offset < 0);
	++length;

	auto ss = std::abs(offset);
	const auto hh = ss / Interval::SECS_PER_HOUR;

	const auto hh_length = UnsafeNumericCast<idx_t>((hh < 100) ? 2 : NumericHelper::UnsignedLength(uint32_t(hh)));
	length += hh_length;

	ss %= Interval::SECS_PER_HOUR;
	const auto mm = ss / Interval::SECS_PER_MINUTE;
	ss %= Interval::SECS_PER_MINUTE;
	if (mm || ss) {
		length += 3;
	}

	if (ss) {
		length += 3;
	}

	string_t result = heap.EmptyString(length);
	auto data = result.GetDataWriteable();

	idx_t pos = 0;
	TimeToStringCast::Format(data + pos, time_length, time, micro_buffer);
	pos += time_length;

	data[pos++] = negative ? '-' : '+';
	if (hh < 100) {
		TimeToStringCast::FormatTwoDigits(data + pos, hh);
	} else {
		NumericHelper::FormatUnsigned(hh, data + pos + hh_length);
	}
	pos += hh_length;

	if (mm || ss) {
		data[pos++] = ':';
		TimeToStringCast::FormatTwoDigits(data + pos, mm);
		pos += 2;
	}

	if (ss) {
		data[pos++] = ':';
		TimeToStringCast::FormatTwoDigits(data + pos, ss);
		pos += 2;
	}

	result.Finalize();
	return result;
}

template <>
string_t StringCast::Operation(timestamp_tz_t input, StringHeap &heap) {
	return StringFromTimestamp<timestamp_tz_t>(input, heap);
}

template <>
string_t StringCast::Operation(timestamp_tz_ns_t input, StringHeap &heap) {
	return StringFromTimestamp<timestamp_tz_ns_t>(input, heap);
}

} // namespace duckdb
