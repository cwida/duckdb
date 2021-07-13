#include "duckdb/main/config.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

namespace duckdb {

static ConfigurationOption internal_options[] = {
    {ConfigurationOptionType::ACCESS_MODE, "access_mode",
     "Access mode of the database ([AUTOMATIC], READ_ONLY or READ_WRITE)", LogicalTypeId::VARCHAR},
    {ConfigurationOptionType::DEFAULT_ORDER_TYPE, "default_order",
     "The order type used when none is specified ([ASC] or DESC)", LogicalTypeId::VARCHAR},
    {ConfigurationOptionType::DEFAULT_NULL_ORDER, "default_null_order",
     "Null ordering used when none is specified ([NULLS_FIRST] or NULLS_LAST)", LogicalTypeId::VARCHAR},
    {ConfigurationOptionType::ENABLE_EXTERNAL_ACCESS, "enable_external_access",
     "Allow the database to access external state (through e.g. COPY TO/FROM, CSV readers, pandas replacement scans, "
     "etc)",
     LogicalTypeId::BOOLEAN},
    {ConfigurationOptionType::ENABLE_OBJECT_CACHE, "enable_object_cache",
     "Whether or not object cache is used to cache e.g. Parquet metadata", LogicalTypeId::BOOLEAN},
    {ConfigurationOptionType::MAXIMUM_MEMORY, "max_memory", "The maximum memory of the system (e.g. 1GB)",
     LogicalTypeId::VARCHAR},
    {ConfigurationOptionType::THREADS, "threads", "The number of total threads used by the system",
     LogicalTypeId::BIGINT},
    {ConfigurationOptionType::INVALID, nullptr, nullptr, LogicalTypeId::INVALID}};

vector<ConfigurationOption> DBConfig::GetOptions() {
	vector<ConfigurationOption> options;
	for (idx_t index = 0; internal_options[index].name; index++) {
		options.push_back(internal_options[index]);
	}
	return options;
}

ConfigurationOption *DBConfig::GetOptionByName(const string &name) {
	for (idx_t index = 0; internal_options[index].name; index++) {
		if (internal_options[index].name == name) {
			return internal_options + index;
		}
	}
	return nullptr;
}

void DBConfig::SetOption(const ConfigurationOption &option, const Value &value) {
	switch (option.type) {
	case ConfigurationOptionType::ACCESS_MODE: {
		auto parameter = StringUtil::Lower(value.ToString());
		if (parameter == "automatic") {
			access_mode = AccessMode::AUTOMATIC;
		} else if (parameter == "read_only") {
			access_mode = AccessMode::READ_ONLY;
		} else if (parameter == "read_write") {
			access_mode = AccessMode::READ_WRITE;
		} else {
			throw InvalidInputException(
			    "Unrecognized parameter for option ACCESS_MODE \"%s\". Expected READ_ONLY or READ_WRITE.", parameter);
		}
		break;
	}
	case ConfigurationOptionType::DEFAULT_ORDER_TYPE: {
		auto parameter = StringUtil::Lower(value.ToString());
		if (parameter == "asc") {
			default_order_type = OrderType::ASCENDING;
		} else if (parameter == "desc") {
			default_order_type = OrderType::DESCENDING;
		} else {
			throw InvalidInputException("Unrecognized parameter for option DEFAULT_ORDER \"%s\". Expected ASC or DESC.",
			                            parameter);
		}
		break;
	}
	case ConfigurationOptionType::DEFAULT_NULL_ORDER: {
		auto parameter = StringUtil::Lower(value.ToString());
		if (parameter == "nulls_first") {
			default_null_order = OrderByNullType::NULLS_FIRST;
		} else if (parameter == "nulls_last") {
			default_null_order = OrderByNullType::NULLS_LAST;
		} else {
			throw InvalidInputException(
			    "Unrecognized parameter for option NULL_ORDER \"%s\". Expected NULLS_FIRST or NULLS_LAST.", parameter);
		}
		break;
	}
	case ConfigurationOptionType::ENABLE_EXTERNAL_ACCESS: {
		enable_external_access = value.CastAs(LogicalType::BOOLEAN).GetValueUnsafe<int8_t>();
		break;
	}
	case ConfigurationOptionType::ENABLE_OBJECT_CACHE: {
		object_cache_enable = value.CastAs(LogicalType::BOOLEAN).GetValueUnsafe<int8_t>();
		break;
	}
	case ConfigurationOptionType::MAXIMUM_MEMORY: {
		maximum_memory = ParseMemoryLimit(value.ToString());
		break;
	}
	case ConfigurationOptionType::THREADS: {
		maximum_threads = value.GetValue<int64_t>();
		break;
	}
	default: // LCOV_EXCL_START
		break;
	} // LCOV_EXCL_STOP
}

idx_t DBConfig::ParseMemoryLimit(const string &arg) {
	if (arg[0] == '-' || arg == "null" || arg == "none") {
		return INVALID_INDEX;
	}
	// split based on the number/non-number
	idx_t idx = 0;
	while (StringUtil::CharacterIsSpace(arg[idx])) {
		idx++;
	}
	idx_t num_start = idx;
	while ((arg[idx] >= '0' && arg[idx] <= '9') || arg[idx] == '.' || arg[idx] == 'e' || arg[idx] == 'E' ||
	       arg[idx] == '-') {
		idx++;
	}
	if (idx == num_start) {
		throw ParserException("Memory limit must have a number (e.g. PRAGMA memory_limit=1GB");
	}
	string number = arg.substr(num_start, idx - num_start);

	// try to parse the number
	double limit = Cast::Operation<string_t, double>(string_t(number));

	// now parse the memory limit unit (e.g. bytes, gb, etc)
	while (StringUtil::CharacterIsSpace(arg[idx])) {
		idx++;
	}
	idx_t start = idx;
	while (idx < arg.size() && !StringUtil::CharacterIsSpace(arg[idx])) {
		idx++;
	}
	if (limit < 0) {
		// limit < 0, set limit to infinite
		return (idx_t)-1;
	}
	string unit = StringUtil::Lower(arg.substr(start, idx - start));
	idx_t multiplier;
	if (unit == "byte" || unit == "bytes" || unit == "b") {
		multiplier = 1;
	} else if (unit == "kilobyte" || unit == "kilobytes" || unit == "kb" || unit == "k") {
		multiplier = 1000LL;
	} else if (unit == "megabyte" || unit == "megabytes" || unit == "mb" || unit == "m") {
		multiplier = 1000LL * 1000LL;
	} else if (unit == "gigabyte" || unit == "gigabytes" || unit == "gb" || unit == "g") {
		multiplier = 1000LL * 1000LL * 1000LL;
	} else if (unit == "terabyte" || unit == "terabytes" || unit == "tb" || unit == "t") {
		multiplier = 1000LL * 1000LL * 1000LL * 1000LL;
	} else {
		throw ParserException("Unknown unit for memory_limit: %s (expected: b, mb, gb or tb)", unit);
	}
	return (idx_t)multiplier * limit;
}

} // namespace duckdb
