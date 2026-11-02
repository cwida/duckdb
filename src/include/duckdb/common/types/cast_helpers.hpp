//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/cast_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "fmt/format.h"

namespace duckdb {

//! NumericHelper is a static class that holds helper functions for integers/doubles
class NumericHelper {
public:
	static constexpr uint8_t CACHED_POWERS_OF_TEN = 19;
	static const int64_t POWERS_OF_TEN[CACHED_POWERS_OF_TEN];
	static const double DOUBLE_POWERS_OF_TEN[40];

public:
	template <class T>
	static int UnsignedLength(T value);

	template <class SIGNED, class UNSIGNED>
	static int SignedLength(SIGNED value) {
		int sign = -(value < 0);
		UNSIGNED unsigned_value = UNSIGNED(value ^ sign) + UNSIGNED(AbsValue(sign));
		return UnsignedLength(unsigned_value) - sign;
	}

	// Formats value in reverse and returns a pointer to the beginning.
	template <class T>
	static char *FormatUnsigned(T value, char *ptr) {
		while (value >= 100) {
			// Integer division is slow so do it for a group of two digits instead
			// of for every digit. The idea comes from the talk by Alexandrescu
			// "Three Optimization Tips for C++".
			auto index = NumericCast<unsigned>((value % 100) * 2);
			value /= 100;
			*--ptr = duckdb_fmt::internal::data::digits[index + 1];
			*--ptr = duckdb_fmt::internal::data::digits[index];
		}
		if (value < 10) {
			*--ptr = NumericCast<char>('0' + value);
			return ptr;
		}
		auto index = NumericCast<unsigned>(value * 2);
		*--ptr = duckdb_fmt::internal::data::digits[index + 1];
		*--ptr = duckdb_fmt::internal::data::digits[index];
		return ptr;
	}

	template <class T>
	static string_t FormatSigned(T value, StringHeap &heap) {
		typedef typename MakeUnsigned<T>::type unsigned_t;
		int8_t sign = -(value < 0);
		unsigned_t unsigned_value = unsigned_t(value ^ T(sign)) + unsigned_t(AbsValue(sign));
		auto length = UnsafeNumericCast<idx_t>(UnsignedLength<unsigned_t>(unsigned_value) + AbsValue(sign));
		string_t result = heap.EmptyString(length);
		auto dataptr = result.GetDataWriteable();
		auto endptr = dataptr + length;
		endptr = FormatUnsigned(unsigned_value, endptr);
		if (sign) {
			*--endptr = '-';
		}
		result.Finalize();
		return result;
	}

	template <class T>
	static std::string ToString(T value) {
		return std::to_string(value);
	}
};

template <>
int NumericHelper::UnsignedLength(uint8_t value);
template <>
int NumericHelper::UnsignedLength(uint16_t value);
template <>
int NumericHelper::UnsignedLength(uint32_t value);
template <>
int NumericHelper::UnsignedLength(uint64_t value);
template <>
int NumericHelper::UnsignedLength(hugeint_t value);

template <>
char *NumericHelper::FormatUnsigned(hugeint_t value, char *ptr);

template <>
std::string NumericHelper::ToString(hugeint_t value);

template <>
std::string NumericHelper::ToString(uhugeint_t value);

template <>
string_t NumericHelper::FormatSigned(hugeint_t value, StringHeap &heap);

struct DecimalToString {
	template <class SIGNED>
	static int DecimalLength(SIGNED value, uint8_t width, uint8_t scale) {
		using UNSIGNED = typename MakeUnsigned<SIGNED>::type;
		if (scale == 0) {
			// scale is 0: regular number
			return NumericHelper::SignedLength<SIGNED, UNSIGNED>(value);
		}
		// length is max of either:
		// scale + 2 OR
		// integer length + 1
		// scale + 2 happens when the number is in the range of (-1, 1)
		// in that case we print "0.XXX", which is the scale, plus "0." (2 chars)
		// integer length + 1 happens when the number is outside of that range
		// in that case we print the integer number, but with one extra character ('.')
		auto extra_characters = width > scale ? 2 : 1;
		return MaxValue(scale + extra_characters + (value < 0 ? 1 : 0),
		                NumericHelper::SignedLength<SIGNED, UNSIGNED>(value) + 1);
	}

	template <class SIGNED>
	static void FormatDecimal(SIGNED value, uint8_t width, uint8_t scale, char *dst, idx_t len) {
		using UNSIGNED = typename MakeUnsigned<SIGNED>::type;
		char *end = dst + len;
		if (value < 0) {
			value = static_cast<SIGNED>(-value);
			*dst = '-';
		}
		if (scale == 0) {
			NumericHelper::FormatUnsigned<UNSIGNED>(UnsafeNumericCast<UNSIGNED>(value), end);
			return;
		}
		// we write two numbers:
		// the numbers BEFORE the decimal (major)
		// and the numbers AFTER the decimal (minor)
		auto minor =
		    UnsafeNumericCast<UNSIGNED>(value) % UnsafeNumericCast<UNSIGNED>(NumericHelper::POWERS_OF_TEN[scale]);
		auto major =
		    UnsafeNumericCast<UNSIGNED>(value) / UnsafeNumericCast<UNSIGNED>(NumericHelper::POWERS_OF_TEN[scale]);
		// write the number after the decimal
		dst = NumericHelper::FormatUnsigned<UNSIGNED>(UnsafeNumericCast<UNSIGNED>(minor), end);
		// (optionally) pad with zeros and add the decimal point
		while (dst > (end - scale)) {
			*--dst = '0';
		}
		*--dst = '.';
		// now write the part before the decimal
		D_ASSERT(width > scale || major == 0);
		if (width > scale) {
			// there are numbers after the comma
			dst = NumericHelper::FormatUnsigned<UNSIGNED>(UnsafeNumericCast<UNSIGNED>(major), dst);
		}
	}

	template <class SIGNED>
	static string_t Format(SIGNED value, uint8_t width, uint8_t scale, StringHeap &heap) {
		int len = DecimalLength<SIGNED>(value, width, scale);
		string_t result = heap.EmptyString(NumericCast<size_t>(len));
		FormatDecimal<SIGNED>(value, width, scale, result.GetDataWriteable(), UnsafeNumericCast<idx_t>(len));
		result.Finalize();
		return result;
	}
};

template <>
int DecimalToString::DecimalLength(hugeint_t value, uint8_t width, uint8_t scale);

template <>
string_t DecimalToString::Format(hugeint_t value, uint8_t width, uint8_t scale, StringHeap &heap);

template <>
void DecimalToString::FormatDecimal(hugeint_t value, uint8_t width, uint8_t scale, char *dst, idx_t len);

struct UhugeintToStringCast {
	static string_t Format(uhugeint_t value, StringHeap &heap) {
		std::string str = value.ToString();
		string_t result = heap.EmptyString(str.length());
		auto data = result.GetDataWriteable();

		memcpy(data, str.data(), str.length()); // NOLINT: null-termination not required
		result.Finalize();
		return result;
	}
};

struct DateToStringCast {
	static idx_t YearLength(int32_t &year, idx_t &year_length, bool &add_bc) {
		// format is YYYY-MM-DD with optional (BC) at the end
		// regular length is 10
		idx_t length = 6;
		year_length = 4;
		add_bc = false;
		if (year <= 0) {
			// add (BC) suffix
			length += 5;
			year = -year + 1;
			add_bc = true;
		}

		// potentially add extra characters depending on length of year
		year_length += year >= 10000;
		year_length += year >= 100000;
		year_length += year >= 1000000;
		year_length += year >= 10000000;
		length += year_length;
		return length;
	}

	static idx_t Length(int32_t date[], idx_t &year_length, bool &add_bc) {
		return YearLength(date[0], year_length, add_bc);
	}

	static void FormatComponent(char *&ptr, int32_t number) {
		ptr[0] = '-';
		if (number < 10) {
			ptr[1] = '0';
			ptr[2] = UnsafeNumericCast<char>('0' + number);
		} else {
			auto index = UnsafeNumericCast<idx_t>(number * 2);
			ptr[1] = duckdb_fmt::internal::data::digits[index];
			ptr[2] = duckdb_fmt::internal::data::digits[index + 1];
		}
		ptr += 3;
	}

	static void Format(char *data, int32_t year, int32_t month, int32_t day, idx_t year_length, bool add_bc) {
		// now we write the string, first write the year
		auto endptr = data + year_length;
		endptr = NumericHelper::FormatUnsigned(year, endptr);
		// add optional leading zeros
		while (endptr > data) {
			*--endptr = '0';
		}
		// now write the month and day
		auto ptr = data + year_length;
		FormatComponent(ptr, month);
		FormatComponent(ptr, day);
		// optionally add BC to the end of the date
		if (add_bc) {
			memcpy(ptr, " (BC)", 5); // NOLINT
		}
	}

	static void Format(char *data, int32_t date[], idx_t year_length, bool add_bc) {
		Format(data, date[0], date[1], date[2], year_length, add_bc);
	}
};

struct TimeToStringCast {
	//! Format microseconds to a buffer of length 6. Returns the number of trailing zeros
	static int32_t FormatMicros(int32_t microseconds, char micro_buffer[], int32_t nanos = 0) {
		idx_t buf_len = 6;
		if (nanos) {
			microseconds *= 1000;
			microseconds += nanos;
			buf_len += 3;
		}
		char *endptr = micro_buffer + buf_len;
		endptr = NumericHelper::FormatUnsigned<int32_t>(microseconds, endptr);
		while (endptr > micro_buffer) {
			*--endptr = '0';
		}
		idx_t trailing_zeros = 0;
		for (idx_t i = buf_len - 1; i > 0; i--) {
			if (micro_buffer[i] != '0') {
				break;
			}
			trailing_zeros++;
		}
		return UnsafeNumericCast<int32_t>(trailing_zeros);
	}

	static idx_t MicrosLength(int32_t micros, char micro_buffer[], int32_t nanos = 0) {
		// format is HH:MM:DD.MS
		// microseconds come after the time with a period separator
		idx_t length;
		if (micros == 0 && nanos == 0) {
			// no microseconds
			// format is HH:MM:DD
			length = 8;
		} else {
			length = 15;
			if (nanos) {
				length += 3;
			}
			// for microseconds, we truncate any trailing zeros (i.e. "90000" becomes ".9")
			// first write the microseconds to the microsecond buffer
			// we write backwards and pad with zeros to the left
			// now we figure out how many digits we need to include by looking backwards
			// and checking how many zeros we encounter
			length -= NumericCast<idx_t>(FormatMicros(micros, micro_buffer, nanos));
		}
		return length;
	}

	static idx_t Length(int32_t time[], char micro_buffer[], int32_t nanos = 0) {
		return MicrosLength(time[3], micro_buffer, nanos);
	}

	static void FormatTwoDigits(char *ptr, int32_t value) {
		if (value < 0 || value > 99) {
			throw InternalException("FormatTwoDigits called with out-of-range value %d", value);
		}
		if (value < 10) {
			ptr[0] = '0';
			ptr[1] = UnsafeNumericCast<char>('0' + value);
		} else {
			auto index = UnsafeNumericCast<unsigned>(value * 2);
			ptr[0] = duckdb_fmt::internal::data::digits[index];
			ptr[1] = duckdb_fmt::internal::data::digits[index + 1];
		}
	}

	static void Format(char *data, idx_t length, int32_t hour, int32_t minute, int32_t second, int32_t unused,
	                   char micro_buffer[]) {
		// first write hour, month and day
		FormatTwoDigits(data, hour);
		data[2] = ':';
		FormatTwoDigits(data + 3, minute);
		data[5] = ':';
		FormatTwoDigits(data + 6, second);
		if (length > 8) {
			// write the micro seconds at the end
			data[8] = '.';
			memcpy(data + 9, micro_buffer, length - 9);
		}
	}

	static void Format(char *data, idx_t length, int32_t time[], char micro_buffer[]) {
		Format(data, length, time[0], time[1], time[2], time[3], micro_buffer);
	}
};

struct IntervalToStringCast {
	static void FormatSignedNumber(int64_t value, char buffer[], idx_t &length) {
		int sign = -(value < 0);
		auto unsigned_value = NumericCast<uint64_t>((value ^ sign) - sign);
		length += NumericCast<idx_t>(NumericHelper::UnsignedLength<uint64_t>(unsigned_value) - sign);
		auto endptr = buffer + length;
		endptr = NumericHelper::FormatUnsigned<uint64_t>(unsigned_value, endptr);
		if (sign) {
			*--endptr = '-';
		}
	}

	static void FormatTwoDigits(int64_t value, char buffer[], idx_t &length) {
		TimeToStringCast::FormatTwoDigits(buffer + length, UnsafeNumericCast<int32_t>(value));
		length += 2;
	}

	static void FormatIntervalValue(int32_t value, char buffer[], idx_t &length, const char *name, idx_t name_len) {
		if (value == 0) {
			return;
		}
		if (length != 0) {
			// space if there is already something in the buffer
			buffer[length++] = ' ';
		}
		FormatSignedNumber(value, buffer, length);
		// append the name together with a potential "s" (for plurals)
		memcpy(buffer + length, name, name_len);
		length += name_len;
		if (value != 1 && value != -1) {
			buffer[length++] = 's';
		}
	}

	//! Formats an interval to a buffer, the buffer should be >=70 characters
	//! years: 17 characters (max value: "-2147483647 years")
	//! months: 9 (max value: "12 months")
	//! days: 16 characters (max value: "-2147483647 days")
	//! time: 24 characters (max value: -2562047788:00:00.123456)
	//! spaces between all characters (+3 characters)
	//! Total: 70 characters
	//! Returns the length of the interval
	static idx_t Format(interval_t interval, char buffer[]) {
		idx_t length = 0;
		if (interval.months != 0) {
			int32_t years = interval.months / 12;
			int32_t months = interval.months - years * 12;
			// format the years and months
			FormatIntervalValue(years, buffer, length, " year", 5);
			FormatIntervalValue(months, buffer, length, " month", 6);
		}
		if (interval.days != 0) {
			// format the days
			FormatIntervalValue(interval.days, buffer, length, " day", 4);
		}
		if (interval.micros != 0) {
			if (length != 0) {
				// space if there is already something in the buffer
				buffer[length++] = ' ';
			}
			int64_t micros = interval.micros;
			if (micros < 0) {
				// negative time: append negative sign
				buffer[length++] = '-';
			} else {
				micros = -micros;
			}
			int64_t hour = -(micros / Interval::MICROS_PER_HOUR);
			micros += hour * Interval::MICROS_PER_HOUR;
			int64_t min = -(micros / Interval::MICROS_PER_MINUTE);
			micros += min * Interval::MICROS_PER_MINUTE;
			int64_t sec = -(micros / Interval::MICROS_PER_SEC);
			micros += sec * Interval::MICROS_PER_SEC;
			micros = -micros;

			if (hour < 10) {
				buffer[length++] = '0';
			}
			FormatSignedNumber(hour, buffer, length);
			buffer[length++] = ':';
			FormatTwoDigits(min, buffer, length);
			buffer[length++] = ':';
			FormatTwoDigits(sec, buffer, length);
			if (micros != 0) {
				buffer[length++] = '.';
				auto trailing_zeros = TimeToStringCast::FormatMicros(NumericCast<int32_t>(micros), buffer + length);
				length += NumericCast<idx_t>(6 - trailing_zeros);
			}
		} else if (length == 0) {
			// empty interval: default to 00:00:00
			memcpy(buffer, "00:00:00", 8); // NOLINT
			return 8;
		}
		return length;
	}
};

} // namespace duckdb
