#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace duckdb {

namespace {

struct DatePartEntry {
	const char *name;
	DatePartSpecifier value;
};

static constexpr uint8_t DATE_PART_ASSO_VALUES[256] = {
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 8,   101, 1,   5,   0,   101, 101, 30,  5,   27,  27,  22,  21,
    25,  26,  11,  12,  16,  0,   45,  39,  101, 64,  101, 2,   101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101,
    101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101, 101};

static constexpr DatePartEntry DATE_PART_ENTRIES[101] = {{nullptr, DatePartSpecifier::INVALID},
                                                         {"s", DatePartSpecifier::SECOND},
                                                         {"c", DatePartSpecifier::CENTURY},
                                                         {"y", DatePartSpecifier::YEAR},
                                                         {"sec", DatePartSpecifier::SECOND},
                                                         {"secs", DatePartSpecifier::SECOND},
                                                         {"d", DatePartSpecifier::DAY},
                                                         {"second", DatePartSpecifier::SECOND},
                                                         {"seconds", DatePartSpecifier::SECOND},
                                                         {"dec", DatePartSpecifier::DECADE},
                                                         {"decs", DatePartSpecifier::DECADE},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"decade", DatePartSpecifier::DECADE},
                                                         {"decades", DatePartSpecifier::DECADE},
                                                         {"year", DatePartSpecifier::YEAR},
                                                         {"years", DatePartSpecifier::YEAR},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"day", DatePartSpecifier::DAY},
                                                         {"days", DatePartSpecifier::DAY},
                                                         {"yr", DatePartSpecifier::YEAR},
                                                         {"yrs", DatePartSpecifier::YEAR},
                                                         {"m", DatePartSpecifier::MINUTE},
                                                         {"ms", DatePartSpecifier::MILLISECONDS},
                                                         {"dayofweek", DatePartSpecifier::DOW},
                                                         {"msec", DatePartSpecifier::MILLISECONDS},
                                                         {"msecs", DatePartSpecifier::MILLISECONDS},
                                                         {"era", DatePartSpecifier::ERA},
                                                         {"msecond", DatePartSpecifier::MILLISECONDS},
                                                         {"mseconds", DatePartSpecifier::MILLISECONDS},
                                                         {"cent", DatePartSpecifier::CENTURY},
                                                         {"h", DatePartSpecifier::HOUR},
                                                         {"dayofyear", DatePartSpecifier::DOY},
                                                         {"century", DatePartSpecifier::CENTURY},
                                                         {"jd", DatePartSpecifier::JULIAN_DAY},
                                                         {"centuries", DatePartSpecifier::CENTURY},
                                                         {"doy", DatePartSpecifier::DOY},
                                                         {"isodow", DatePartSpecifier::ISODOW},
                                                         {"isoyear", DatePartSpecifier::ISOYEAR},
                                                         {"microsecond", DatePartSpecifier::MICROSECONDS},
                                                         {"microseconds", DatePartSpecifier::MICROSECONDS},
                                                         {"us", DatePartSpecifier::MICROSECONDS},
                                                         {"epoch", DatePartSpecifier::EPOCH},
                                                         {"usec", DatePartSpecifier::MICROSECONDS},
                                                         {"usecs", DatePartSpecifier::MICROSECONDS},
                                                         {"yearweek", DatePartSpecifier::YEARWEEK},
                                                         {"usecond", DatePartSpecifier::MICROSECONDS},
                                                         {"useconds", DatePartSpecifier::MICROSECONDS},
                                                         {"hr", DatePartSpecifier::HOUR},
                                                         {"hrs", DatePartSpecifier::HOUR},
                                                         {"dayofmonth", DatePartSpecifier::DAY},
                                                         {"mil", DatePartSpecifier::MILLENNIUM},
                                                         {"mils", DatePartSpecifier::MILLENNIUM},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"min", DatePartSpecifier::MINUTE},
                                                         {"mins", DatePartSpecifier::MINUTE},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"minute", DatePartSpecifier::MINUTE},
                                                         {"minutes", DatePartSpecifier::MINUTE},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"millisecond", DatePartSpecifier::MILLISECONDS},
                                                         {"milliseconds", DatePartSpecifier::MILLISECONDS},
                                                         {"millennia", DatePartSpecifier::MILLENNIUM},
                                                         {"millennium", DatePartSpecifier::MILLENNIUM},
                                                         {"millenniums", DatePartSpecifier::MILLENNIUM},
                                                         {"w", DatePartSpecifier::WEEK},
                                                         {"quarter", DatePartSpecifier::QUARTER},
                                                         {"quarters", DatePartSpecifier::QUARTER},
                                                         {"week", DatePartSpecifier::WEEK},
                                                         {"weeks", DatePartSpecifier::WEEK},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"weekday", DatePartSpecifier::DOW},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"weekofyear", DatePartSpecifier::WEEK},
                                                         {"mon", DatePartSpecifier::MONTH},
                                                         {"mons", DatePartSpecifier::MONTH},
                                                         {"month", DatePartSpecifier::MONTH},
                                                         {"months", DatePartSpecifier::MONTH},
                                                         {"timezone", DatePartSpecifier::TIMEZONE},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"timezone_hour", DatePartSpecifier::TIMEZONE_HOUR},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"timezone_minute", DatePartSpecifier::TIMEZONE_MINUTE},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"julian", DatePartSpecifier::JULIAN_DAY},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"millenium", DatePartSpecifier::MILLENNIUM},
                                                         {nullptr, DatePartSpecifier::INVALID},
                                                         {"dow", DatePartSpecifier::DOW},
                                                         {"hour", DatePartSpecifier::HOUR},
                                                         {"hours", DatePartSpecifier::HOUR}};

static uint8_t DatePartHashChar(char c) {
	return DATE_PART_ASSO_VALUES[StringUtil::ASCII_TO_LOWER_MAP[static_cast<uint8_t>(c)]];
}

static bool DatePartEquals(string_t input, const char *expected) {
	const auto input_data = input.GetData();
	const auto input_size = input.GetSize();
	for (idx_t i = 0; i < input_size; i++) {
		if (expected[i] == '\0') {
			return false;
		}
		if (StringUtil::ASCII_TO_LOWER_MAP[static_cast<uint8_t>(input_data[i])] !=
		    StringUtil::ASCII_TO_LOWER_MAP[static_cast<uint8_t>(expected[i])]) {
			return false;
		}
	}
	return expected[input_size] == '\0';
}

} // namespace

bool TryGetDatePartSpecifier(const string &specifier, DatePartSpecifier &result) {
	return TryGetDatePartSpecifier(string_t(specifier), result);
}

bool TryGetDatePartSpecifier(string_t specifier, DatePartSpecifier &result) {
	const auto specifier_data = specifier.GetData();
	const auto specifier_size = specifier.GetSize();
	if (specifier_size == 0 || specifier_size > 15) {
		return false;
	}

	uint32_t hash = static_cast<uint32_t>(specifier_size);
	if (specifier_size >= 8) {
		hash += DatePartHashChar(specifier_data[7]);
	}
	if (specifier_size >= 3) {
		hash += DatePartHashChar(specifier_data[2]);
	}
	if (specifier_size >= 2) {
		hash += DatePartHashChar(specifier_data[1]);
	}
	hash += DatePartHashChar(specifier_data[0]);
	if (hash > 100) {
		return false;
	}

	const auto &entry = DATE_PART_ENTRIES[hash];
	if (entry.name == nullptr || !DatePartEquals(specifier, entry.name)) {
		return false;
	}
	result = entry.value;
	return true;
}

DatePartSpecifier GetDatePartSpecifier(const string &specifier) {
	DatePartSpecifier result;
	if (!TryGetDatePartSpecifier(specifier, result)) {
		throw ConversionException("extract specifier \"%s\" not recognized", specifier);
	}
	return result;
}

DatePartSpecifier GetDatePartSpecifier(string_t specifier) {
	DatePartSpecifier result;
	if (!TryGetDatePartSpecifier(specifier, result)) {
		throw ConversionException("extract specifier \"%s\" not recognized", specifier.GetString());
	}
	return result;
}

} // namespace duckdb
