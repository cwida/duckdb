#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/common/types/value.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/storage/statistics/stats_writer.hpp"

namespace duckdb {

bool StringStatsData::HasMinStringLength() const {
	if (!has_min_string_length) {
		return false;
	}
	return min_string_length != NumericLimits<uint32_t>::Maximum();
}

BaseStatistics StringStats::CreateUnknown(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	auto &string_data = GetDataUnsafe(result);
	string_data.max_string_length = 0;
	string_data.has_max_string_length = false;
	string_data.min_string_length = 0;
	string_data.has_min_string_length = false;
	string_data.total_string_length = 0;
	string_data.has_total_string_length = false;
	string_data.has_unicode = true;
	string_data.min_type = StringStatsType::NO_STATS;
	string_data.max_type = StringStatsType::NO_STATS;
	return result;
}

BaseStatistics StringStats::CreateEmpty(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	auto &string_data = GetDataUnsafe(result);
	string_data.max_string_length = 0;
	string_data.has_max_string_length = true;
	string_data.min_string_length = NumericLimits<uint32_t>::Maximum();
	string_data.has_min_string_length = true;
	string_data.total_string_length = 0;
	string_data.has_total_string_length = true;
	string_data.has_unicode = false;
	string_data.min_type = StringStatsType::EMPTY_STATS;
	string_data.max_type = StringStatsType::EMPTY_STATS;
	return result;
}

StringStatsData &StringStats::GetDataUnsafe(BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::STRING_STATS);
	return stats.stats_union.string_data;
}

const StringStatsData &StringStats::GetDataUnsafe(const BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::STRING_STATS);
	return stats.stats_union.string_data;
}

bool StatsIsSet(StringStatsType type) {
	return type == StringStatsType::TRUNCATED_STATS || type == StringStatsType::EXACT_STATS;
}

bool StringStats::HasMinMax(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return false;
	}
	auto &string_data = GetDataUnsafe(stats);
	return StatsIsSet(string_data.min_type) && StatsIsSet(string_data.max_type);
}

bool StringStats::HasMin(const BaseStatistics &stats) {
	return StatsIsSet(GetMinType(stats));
}

bool StringStats::HasMax(const BaseStatistics &stats) {
	return StatsIsSet(GetMaxType(stats));
}

StringStatsType StringStats::GetMinType(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return StringStatsType::NO_STATS;
	}
	auto &string_data = GetDataUnsafe(stats);
	return string_data.min_type;
}

StringStatsType StringStats::GetMaxType(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return StringStatsType::NO_STATS;
	}
	auto &string_data = GetDataUnsafe(stats);
	return string_data.max_type;
}

bool StringStats::HasMaxStringLength(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return false;
	}
	return GetDataUnsafe(stats).has_max_string_length;
}

uint32_t StringStats::MaxStringLength(const BaseStatistics &stats) {
	if (!HasMaxStringLength(stats)) {
		throw InternalException("MaxStringLength called on statistics that does not have a max string length");
	}
	return GetDataUnsafe(stats).max_string_length;
}

optional_idx StringStats::MinStringLength(const BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	if (!data.HasMinStringLength()) {
		return optional_idx();
	}
	return data.min_string_length;
}

optional_idx StringStats::TotalStringLength(const BaseStatistics &stats) {
	auto &string_stats = GetDataUnsafe(stats);
	return string_stats.has_total_string_length ? string_stats.total_string_length : optional_idx();
}

bool StringStats::CanContainUnicode(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return true;
	}
	return GetDataUnsafe(stats).has_unicode;
}

string StringStats::Min(const BaseStatistics &stats) {
	auto &string_data = GetDataUnsafe(stats);
	if (!StatsIsSet(string_data.min_type)) {
		throw InternalException("StringStats::Min called but no string stats were found - call StringStats::HasMin or "
		                        "StringStats::HasMinMax first");
	}
	return string_data.min.GetString();
}

string StringStats::Max(const BaseStatistics &stats) {
	auto &string_data = GetDataUnsafe(stats);
	if (!StatsIsSet(string_data.max_type)) {
		throw InternalException("StringStats::Max called but no string stats were found - call StringStats::HasMax or "
		                        "StringStats::HasMinMax first");
	}
	return string_data.max.GetString();
}

Value StringStats::TryGetValidMin(const BaseStatistics &stats) {
	auto min = Min(stats);
	if (GetMinType(stats) == StringStatsType::EXACT_STATS) {
		return Value(std::move(min));
	}
	string result;
	if (!Utf8Proc::ValidLowerBound(min, result)) {
		return Value();
	}
	return Value(std::move(result));
}

Value StringStats::TryGetValidMax(const BaseStatistics &stats) {
	auto max = Max(stats);
	if (GetMaxType(stats) == StringStatsType::EXACT_STATS) {
		return Value(std::move(max));
	}
	string result;
	if (!Utf8Proc::ValidUpperBound(max, result)) {
		return Value();
	}
	return Value(std::move(result));
}

void StringStats::ResetMaxStringLength(BaseStatistics &stats) {
	GetDataUnsafe(stats).has_max_string_length = false;
}

void StringStats::SetMaxStringLength(BaseStatistics &stats, uint32_t length) {
	auto &data = GetDataUnsafe(stats);
	data.has_max_string_length = true;
	data.max_string_length = length;
}

void StringStats::SetContainsUnicode(BaseStatistics &stats) {
	GetDataUnsafe(stats).has_unicode = true;
}

static void LegacyConstructValue(const_data_ptr_t data, idx_t size, data_t target[]) {
	constexpr auto max_length = StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE;
	idx_t value_size = size > max_length ? max_length : size;
	memcpy(target, data, value_size);
	for (idx_t i = value_size; i < max_length; i++) {
		target[i] = '\0';
	}
}

void LegacyConstructMinMax(string_t input, StringStatsType type, data_t result[], bool is_min) {
	auto input_data = const_data_ptr_cast(input.GetData());
	if (type == StringStatsType::EMPTY_STATS) {
		// for empty stats we serialize min as the maximum value, and max as the minimum value
		data_t empty_byte = is_min ? 0xFF : 0x00;
		memset(result, empty_byte, StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE);
	} else if (type == StringStatsType::NO_STATS) {
		// for no stats we serialize min as 0x00..., and max as 0xFF...
		data_t no_stats_byte = is_min ? 0x00 : 0xFF;
		memset(result, no_stats_byte, StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE);
	} else {
		LegacyConstructValue(input_data, input.GetSize(), result);
	}
}

string_t LegacyReadMinMax(const data_t result[]) {
	// truncate any trailing 0-bytes - there's no reason to keep them around
	uint32_t len = StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE;
	for (; len > 0; len--) {
		if (result[len - 1] != '\0') {
			break;
		}
	}
	return string_t(const_char_ptr_cast(result), len);
}

static bool LegacyAllCharsEqualTo(const data_t data[], data_t comp) {
	for (idx_t i = 0; i < StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE; i++) {
		if (data[i] != comp) {
			return false;
		}
	}
	return true;
}

StringStatsType LegacyGetMinMaxType(const data_t min[], const data_t max[], bool has_max_length, idx_t max_length) {
	if (min[0] > max[0]) {
		// if min > max then we have empty stats
		return StringStatsType::EMPTY_STATS;
	}
	if (LegacyAllCharsEqualTo(min, 0x00) && LegacyAllCharsEqualTo(max, 0xFF)) {
		// if min is 0x00... and max is 0xFF... then we have no min/max (nothing can ever be pruned)
		return StringStatsType::NO_STATS;
	}
	if (has_max_length && max_length <= StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE) {
		// when:
		// (1) max length is known, and
		// (2) max length is <= 8, and
		// (3) min/max are equivalent to the max length
		// We know the min/max are exact because they have not been truncated
		// if min/max are not equal to the max length - we can't distinguish between e.g. `hello\0` and `hello`
		if (max_length == 0 || (min[max_length - 1] != '\0' && max[max_length - 1] != '\0')) {
			return StringStatsType::EXACT_STATS;
		}
	}
	return StringStatsType::TRUNCATED_STATS;
}

struct StringStatsField {
	explicit StringStatsField(uint32_t data_p = 0U) : data(data_p) {
	}

public:
	static constexpr uint32_t NO_UNICODE_FLAG = 1U << 0U;
	static constexpr uint32_t MIN_EXACT_OR_EMPTY_FLAG = 1U << 1U;
	static constexpr uint32_t MAX_EXACT_OR_EMPTY_FLAG = 1U << 2U;
	static constexpr uint32_t LENGTH_MASK = 0xFU;
	static constexpr uint32_t MIN_LENGTH_POSITION = 3U;
	static constexpr uint32_t MAX_LENGTH_POSITION = 7U;
	static constexpr uint32_t MIN_LENGTH_MASK = LENGTH_MASK << MIN_LENGTH_POSITION;
	static constexpr uint32_t MAX_LENGTH_MASK = LENGTH_MASK << MAX_LENGTH_POSITION;
	static constexpr uint32_t MIN_STRING_LENGTH_POSITION = 11U;
	static constexpr uint32_t MIN_STRING_LENGTH_BIT_SIZE = 21U;
	static constexpr uint32_t MIN_STRING_LENGTH_MASK = ((1U << MIN_STRING_LENGTH_BIT_SIZE) - 1)
	                                                   << MIN_STRING_LENGTH_POSITION;

	void SetHasUnicode(bool has_unicode) {
		// default is has_unicode = true
		if (!has_unicode) {
			data |= NO_UNICODE_FLAG;
		} else {
			data &= ~NO_UNICODE_FLAG;
		}
	}
	bool GetHasUnicode() const {
		return !(data & NO_UNICODE_FLAG);
	}
	void SetMinType(StringStatsType min_type) {
		// we use one bit to encode these 4 values
		// there are 4 possibilities
		// min_max missing - bit set to 1 - EMPTY_STATS
		// min_max missing - bit set to 0 - NO_STATS
		// min_max present - bit set to 1 - EXACT_STATS
		// min_max present - bit set to 0 - TRUNCATED_STATS
		if (min_type == StringStatsType::EXACT_STATS || min_type == StringStatsType::EMPTY_STATS) {
			data |= MIN_EXACT_OR_EMPTY_FLAG;
		} else {
			data &= ~MIN_EXACT_OR_EMPTY_FLAG;
		}
	}
	StringStatsType GetMinType(bool has_min) const {
		if (data & MIN_EXACT_OR_EMPTY_FLAG) {
			return has_min ? StringStatsType::EXACT_STATS : StringStatsType::EMPTY_STATS;
		} else {
			return has_min ? StringStatsType::TRUNCATED_STATS : StringStatsType::NO_STATS;
		}
	}
	void SetMaxType(StringStatsType max_type) {
		if (max_type == StringStatsType::EXACT_STATS || max_type == StringStatsType::EMPTY_STATS) {
			data |= MAX_EXACT_OR_EMPTY_FLAG;
		} else {
			data &= ~MAX_EXACT_OR_EMPTY_FLAG;
		}
	}
	StringStatsType GetMaxType(bool has_max) const {
		if (data & MAX_EXACT_OR_EMPTY_FLAG) {
			return has_max ? StringStatsType::EXACT_STATS : StringStatsType::EMPTY_STATS;
		} else {
			return has_max ? StringStatsType::TRUNCATED_STATS : StringStatsType::NO_STATS;
		}
	}
	void SetMinLength(idx_t min_length) {
		if (min_length > 12) {
			throw InternalException("StringStatsField::SetMinLength should be at most 12");
		}
		data =
		    (data & ~MIN_LENGTH_MASK) | ((static_cast<uint32_t>(min_length) << MIN_LENGTH_POSITION) & MIN_LENGTH_MASK);
	}
	uint32_t GetMinLength() const {
		auto result = (data >> MIN_LENGTH_POSITION) & LENGTH_MASK;
		if (result > 12) {
			throw SerializationException("Invalid string stats - min length cannot be more than 12");
		}
		return result;
	}
	void SetMaxLength(idx_t max_length) {
		if (max_length > 12) {
			throw InternalException("StringStatsField::SetMaxLength should be at most 12");
		}
		data =
		    (data & ~MAX_LENGTH_MASK) | ((static_cast<uint32_t>(max_length) << MAX_LENGTH_POSITION) & MAX_LENGTH_MASK);
	}
	uint32_t GetMaxLength() const {
		auto result = (data >> MAX_LENGTH_POSITION) & LENGTH_MASK;
		if (result > 12) {
			throw SerializationException("Invalid string stats - min length cannot be more than 12");
		}
		return result;
	}

	uint32_t GetData() const {
		return data;
	}

	void SetMinStringLength(uint32_t min_str_length) {
		if (min_str_length >= StringStatsData::MAXIMUM_MIN_STRING_LENGTH) {
			// exceeds max length - set to max value to indicate "we know the min string length, it is >= the max)
			min_str_length = StringStatsData::MAXIMUM_MIN_STRING_LENGTH;
		} else {
			// increment by 1 (0 is reserved for not set)
			min_str_length++;
		}
		data = (data & ~MIN_STRING_LENGTH_MASK) |
		       ((min_str_length << MIN_STRING_LENGTH_POSITION) & MIN_STRING_LENGTH_MASK);
	}
	bool HasMinStringLength() const {
		return GetMinStringLengthInternal() > 0;
	}
	uint32_t GetMinStringLength() const {
		auto min_str_len = GetMinStringLengthInternal();
		if (min_str_len == StringStatsData::MAXIMUM_MIN_STRING_LENGTH) {
			// we know the min string length is >= MAXIMUM_MIN_STRING_LENGTH
			// return UINT_MAX to indicate this
			return NumericLimits<uint32_t>::Maximum();
		}
		if (min_str_len == 0) {
			throw InternalException("GetMinStringLength() called but min string length is not set");
		}
		return min_str_len - 1;
	}

private:
	uint32_t GetMinStringLengthInternal() const {
		return (data & MIN_STRING_LENGTH_MASK) >> MIN_STRING_LENGTH_POSITION;
	}

private:
	uint32_t data;
};

void TruncateStatsIfRequired(idx_t &len, StringStatsType &type) {
	if (len > StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE) {
		len = StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE;
		type = StringStatsType::TRUNCATED_STATS;
	}
}

void StringStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &string_data = GetDataUnsafe(stats);
	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		// targeting old storage: use legacy serialize
		data_t min_data[StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE];
		data_t max_data[StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE];
		LegacyConstructMinMax(string_data.min, string_data.min_type, min_data, true);
		LegacyConstructMinMax(string_data.max, string_data.max_type, max_data, false);
		serializer.WriteProperty(200, "min", min_data, StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE);
		serializer.WriteProperty(201, "max", max_data, StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE);
		serializer.WriteProperty(202, "has_unicode", string_data.has_unicode);
		serializer.WriteProperty(203, "has_max_string_length", string_data.has_max_string_length);
		serializer.WriteProperty(204, "max_string_length", string_data.max_string_length);
	} else {
		// new serialize - we try to pack elements into a single integer to save on fields
		StringStatsField field;
		field.SetHasUnicode(string_data.has_unicode);
		if (string_data.has_max_string_length) {
			// use the same field as before for max string length
			serializer.WriteProperty(204, "max_string_length", string_data.max_string_length);
		}
		auto min_type = string_data.min_type;
		auto max_type = string_data.max_type;
		if (StatsIsSet(min_type) && StatsIsSet(max_type)) {
			// write the min/max together as a single blob field, if they are set
			constexpr uint32_t min_max_size = StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE * 2;
			data_t min_max_data[min_max_size] = {};
			auto min_size = string_data.min.GetSize();
			auto max_size = string_data.max.GetSize();
			TruncateStatsIfRequired(min_size, min_type);
			TruncateStatsIfRequired(max_size, max_type);
			field.SetMinLength(min_size);
			field.SetMaxLength(max_size);

			memcpy(min_max_data, string_data.min.GetData(), min_size);
			memcpy(min_max_data + StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE, string_data.max.GetData(), max_size);

			serializer.WriteProperty(205, "min_max", min_max_data, min_max_size);
		}
		field.SetMinType(min_type);
		field.SetMaxType(max_type);
		if (string_data.has_min_string_length) {
			field.SetMinStringLength(string_data.min_string_length);
		}
		// write the stats as a single packed field
		serializer.WriteProperty(206, "packed_field", field.GetData());
		serializer.WritePropertyWithDefault(207, "total_string_length",
		                                    string_data.has_total_string_length ? string_data.total_string_length
		                                                                        : optional_idx());
	}
}

void StringStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	auto &string_data = GetDataUnsafe(base);
	// try to deserialize legacy stats first
	data_t legacy_min_data[StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE];
	data_t legacy_max_data[StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE];
	auto has_min =
	    deserializer.ReadOptionalProperty(200, "min", legacy_min_data, StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE);
	auto has_max =
	    deserializer.ReadOptionalProperty(201, "max", legacy_max_data, StringStatsData::LEGACY_MAX_STRING_MINMAX_SIZE);
	if (has_min && has_max) {
		// legacy stats
		deserializer.ReadProperty(202, "has_unicode", string_data.has_unicode);
		deserializer.ReadProperty(203, "has_max_string_length", string_data.has_max_string_length);
		deserializer.ReadProperty(204, "max_string_length", string_data.max_string_length);
		string_data.min = AssignString(base, LegacyReadMinMax(legacy_min_data), true);
		string_data.max = AssignString(base, LegacyReadMinMax(legacy_max_data), false);
		string_data.min_type = LegacyGetMinMaxType(legacy_min_data, legacy_max_data, string_data.has_max_string_length,
		                                           string_data.max_string_length);
		string_data.max_type = string_data.min_type;
		string_data.has_total_string_length = false;
		string_data.has_min_string_length = false;
	} else {
		auto max_string_length =
		    deserializer.ReadPropertyWithExplicitDefault(204, "max_string_length", NumericLimits<uint32_t>::Maximum());
		if (max_string_length == NumericLimits<uint32_t>::Maximum()) {
			string_data.has_max_string_length = false;
		} else {
			string_data.has_max_string_length = true;
			string_data.max_string_length = max_string_length;
		}
		constexpr uint32_t min_max_size = StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE * 2;
		data_t min_max_data[min_max_size];
		auto has_min_max = deserializer.ReadOptionalProperty(205, "min_max", min_max_data, min_max_size);
		auto packed_field = deserializer.ReadProperty<uint32_t>(206, "packed_field");
		StringStatsField field(packed_field);
		string_data.min_type = field.GetMinType(has_min_max);
		string_data.max_type = field.GetMaxType(has_min_max);
		if (has_min_max) {
			auto min = string_t(const_char_ptr_cast(min_max_data), field.GetMinLength());
			auto max = string_t(const_char_ptr_cast(min_max_data + StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE),
			                    field.GetMaxLength());
			string_data.min = AssignString(base, min, true);
			string_data.max = AssignString(base, max, false);
		}
		string_data.has_unicode = field.GetHasUnicode();
		if (field.HasMinStringLength()) {
			string_data.has_min_string_length = true;
			string_data.min_string_length = field.GetMinStringLength();
		} else {
			string_data.has_min_string_length = false;
		}
		auto total_string_length = deserializer.ReadPropertyWithDefault<optional_idx>(207, "total_string_length");
		if (total_string_length.IsValid()) {
			string_data.has_total_string_length = true;
			string_data.total_string_length = total_string_length.GetIndex();
		} else {
			string_data.has_total_string_length = false;
		}
	}
}

void StringStats::FromConstant(BaseStatistics &stats, string_t value) {
	static constexpr const idx_t CONSTANT_STATS_BOUND = 100000;

	// use the string stats writer for setting stats
	StatsWriter<string_t> writer(stats.GetType());
	writer.Update(value);
	writer.Merge(stats);

	// just directly assign the min/max, since the stats writer truncates
	// ... except if it is REALLY long
	if (value.GetSize() <= CONSTANT_STATS_BOUND) {
		SetMin(stats, value, StringStatsType::EXACT_STATS);
		SetMax(stats, value, StringStatsType::EXACT_STATS);
	}

	// constants don't have a "total string length"
	auto &string_stats = GetDataUnsafe(stats);
	string_stats.has_total_string_length = false;
}

void StringStats::MergeInConstant(BaseStatistics &stats, string_t input) {
	auto constant_stats = CreateEmpty(stats.GetType());
	FromConstant(constant_stats, input);
	Merge(stats, constant_stats);
}

void StringStats::Update(BaseStatistics &stats, const string_t &value) {
	MergeInConstant(stats, value);
}

struct StringData {
	unique_ptr<data_t[]> data;
	idx_t capacity = 0;

	string_t AssignString(const string_t &value) {
		if (value.GetSize() > capacity) {
			auto next_capacity = NextPowerOfTwo(value.GetSize());
			data = make_uniq_array<data_t>(next_capacity);
			capacity = next_capacity;
		}
		memcpy(data.get(), value.GetData(), value.GetSize());
		return string_t(const_char_ptr_cast(data.get()), static_cast<uint32_t>(value.GetSize()));
	}
};

struct StringStatsExtraData : public ExtraStatsData {
	StringData string_data[2];
};

string_t StringStats::AssignString(BaseStatistics &stats, const string_t &input, bool is_min) {
	if (input.IsInlined()) {
		return input;
	}
	if (!stats.extra_data) {
		stats.extra_data = make_uniq<StringStatsExtraData>();
	}
	auto &extra_data = stats.extra_data->Cast<StringStatsExtraData>();
	auto data_idx = is_min ? 0ULL : 1ULL;
	return extra_data.string_data[data_idx].AssignString(input);
}

void StringStats::SetMin(BaseStatistics &stats, const string_t &value, StringStatsType type) {
	auto &stats_data = GetDataUnsafe(stats);
	stats_data.min = AssignString(stats, value, true);
	stats_data.min_type = type;
}

void StringStats::SetMax(BaseStatistics &stats, const string_t &value, StringStatsType type) {
	auto &stats_data = GetDataUnsafe(stats);
	stats_data.max = AssignString(stats, value, false);
	stats_data.max_type = type;
}

void StringStats::SetMin(BaseStatistics &stats, const string_t &value) {
	SetMin(stats, value, StringStatsType::TRUNCATED_STATS);
}

void StringStats::SetMax(BaseStatistics &stats, const string_t &value) {
	SetMax(stats, value, StringStatsType::TRUNCATED_STATS);
}

void StringStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	auto &string_data = GetDataUnsafe(stats);
	auto &other_data = GetDataUnsafe(other);
	if (StatsIsSet(other_data.min_type)) {
		string_data.min = AssignString(stats, other_data.min, true);
	}
	if (StatsIsSet(other_data.max_type)) {
		string_data.max = AssignString(stats, other_data.max, false);
	}
}

void StringStats::MergeStats(BaseStatistics &stats, string_t &target, StringStatsType &target_type,
                             const string_t &source, StringStatsType source_type, bool is_min) {
	if (target_type == StringStatsType::NO_STATS || source_type == StringStatsType::NO_STATS) {
		// no min/max available - result is no min/max
		target_type = StringStatsType::NO_STATS;
		return;
	}
	if (source_type == StringStatsType::EMPTY_STATS) {
		// source is empty - nothing to update
		return;
	}
	if (target_type == StringStatsType::EMPTY_STATS) {
		// we don't have min/max - copy them from the target
		target = AssignString(stats, source, is_min);
		target_type = source_type;
		return;
	}
	// both min/max stats are there - compare
	auto comparison = Comparator::Operation(source, target);
	if (comparison == 0) {
		if (target_type == StringStatsType::TRUNCATED_STATS || source_type == StringStatsType::TRUNCATED_STATS) {
			target_type = StringStatsType::TRUNCATED_STATS;
		}
		return;
	}
	bool new_is_more_extreme = is_min ? comparison < 0 : comparison > 0;
	// for min: a prefix is always <= its extensions, so the shorter value is already a safe lower bound
	// for max: a truncated prefix represents unknown larger extensions and must be kept as the upper bound
	if (!is_min && memcmp(source.GetData(), target.GetData(), MinValue(source.GetSize(), target.GetSize())) == 0) {
		if (comparison < 0) {
			// source is a prefix of target: source wins if it is truncated
			new_is_more_extreme = source_type == StringStatsType::TRUNCATED_STATS;
		} else {
			// target is a prefix of source: target wins if it is truncated
			new_is_more_extreme = target_type != StringStatsType::TRUNCATED_STATS;
		}
	}
	if (!new_is_more_extreme) {
		return;
	}
	// assign the new value
	target = AssignString(stats, source, is_min);
	target_type = source_type;
}

void StringStats::Merge(BaseStatistics &stats, const StringStatsData &other_data, StatsMergeType merge_type) {
	auto &string_data = GetDataUnsafe(stats);

	// merge min/max
	MergeStats(stats, string_data.min, string_data.min_type, other_data.min, other_data.min_type, true);
	MergeStats(stats, string_data.max, string_data.max_type, other_data.max, other_data.max_type, false);
	string_data.has_unicode = string_data.has_unicode || other_data.has_unicode;
	string_data.has_max_string_length = string_data.has_max_string_length && other_data.has_max_string_length;
	string_data.max_string_length = MaxValue<uint32_t>(string_data.max_string_length, other_data.max_string_length);
	string_data.has_min_string_length = string_data.has_min_string_length && other_data.has_min_string_length;
	string_data.min_string_length = MinValue<uint32_t>(string_data.min_string_length, other_data.min_string_length);
	if (merge_type == StatsMergeType::EXPAND_BOUNDS) {
		// when expanding bounds we cannot sum total_string_length — the other side may overlap with our data
		string_data.has_total_string_length = false;
	} else {
		string_data.has_total_string_length = string_data.has_total_string_length && other_data.has_total_string_length;
		if (string_data.has_total_string_length) {
			string_data.total_string_length += other_data.total_string_length;
		}
	}
}

void StringStats::Merge(BaseStatistics &stats, const BaseStatistics &other, StatsMergeType merge_type) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}
	if (other.GetType().id() == LogicalTypeId::SQLNULL) {
		return;
	}
	auto &other_data = GetDataUnsafe(other);
	Merge(stats, other_data, merge_type);
}

string_t ReadWriterStats(const data_t data[], idx_t size, StringStatsType &type) {
	if (size <= StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE) {
		// exact length
		type = StringStatsType::EXACT_STATS;
		return string_t(const_char_ptr_cast(data), static_cast<uint32_t>(size));
	}
	// truncated
	type = StringStatsType::TRUNCATED_STATS;
	return string_t(const_char_ptr_cast(data), StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
}

void StringStats::Merge(BaseStatistics &stats, const StatsWriter<string_t> &stats_writer) {
	if (!stats_writer.AnyValid()) {
		return;
	}
	// construct string stats data from the writer
	StringStatsData other_data;
	other_data.min = ReadWriterStats(stats_writer.min, stats_writer.min_size, other_data.min_type);
	other_data.max = ReadWriterStats(stats_writer.max, stats_writer.max_size, other_data.max_type);
	other_data.has_unicode = stats_writer.has_unicode;
	other_data.has_max_string_length = true;
	other_data.max_string_length = stats_writer.max_string_length;
	other_data.has_min_string_length = true;
	other_data.min_string_length = stats_writer.min_string_length;
	other_data.has_total_string_length = true;
	other_data.total_string_length = stats_writer.total_string_length;
	Merge(stats, other_data);
}

FilterPropagateResult StringStats::CheckZonemap(const BaseStatistics &stats, ExpressionType comparison_type,
                                                array_ptr<const Value> constants) {
	auto &string_data = GetDataUnsafe(stats);
	D_ASSERT(stats.CanHaveNoNull());
	for (auto &constant_value : constants) {
		D_ASSERT(constant_value.type() == stats.GetType());
		D_ASSERT(!constant_value.IsNull());
		auto &constant = StringValue::Get(constant_value);
		FilterPropagateResult prune_result;
		if (HasMinMax(stats)) {
			// Special handle cases where constant is equal to the truncated min bound, but we're able to prune a few
			// row groups. For example, a truncated "bbbbbbbbbbbb" with minimum length 13 has the lower bound
			// "bbbbbbbbbbbb\0" -- the min bound is obviously larger than the constant.
			auto min = string_data.min;
			auto min_type = string_data.min_type;
			string min_bound;
			auto min_string_length = MinStringLength(stats);
			if (min_type == StringStatsType::TRUNCATED_STATS && constant.size() == min.GetSize() &&
			    min_string_length.IsValid() && min_string_length.GetIndex() > constant.size() &&
			    CompareStringStats(constant, min, min_type) == 0) {
				min_bound = min.GetString() + '\0';
				min = string_t(min_bound);
				min_type = StringStatsType::EXACT_STATS;
			}
			prune_result =
			    CheckZonemap(min, min_type, string_data.max, string_data.max_type, comparison_type, constant);
		} else {
			prune_result = FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
	}
	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

int8_t StringStats::CompareStringStats(string_t input, string_t stats, StringStatsType type) {
	if (type == StringStatsType::TRUNCATED_STATS && input.GetSize() > stats.GetSize()) {
		// if the stats are truncated we can only compare at most the bytes as are present in the stats
		return Comparator::Operation(string_t(input.GetData(), static_cast<uint32_t>(stats.GetSize())), stats);
	}
	return Comparator::Operation(input, stats);
}

FilterPropagateResult StringStats::CheckZonemap(string_t min, StringStatsType min_type, string_t max,
                                                StringStatsType max_type, ExpressionType comparison_type,
                                                string_t constant) {
	auto min_comp = CompareStringStats(constant, min, min_type);
	auto max_comp = CompareStringStats(constant, max, max_type);
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		if (min_comp == 0 && max_comp == 0 && min_type == StringStatsType::EXACT_STATS &&
		    max_type == StringStatsType::EXACT_STATS) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (min_comp >= 0 && max_comp <= 0) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
		if (min_comp < 0 || max_comp > 0) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		if (min_comp == 0 && max_comp == 0 && min_type == StringStatsType::EXACT_STATS &&
		    max_type == StringStatsType::EXACT_STATS) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (max_comp < 0 || (max_comp == 0 && max_type != StringStatsType::EXACT_STATS)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (max_comp <= 0) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHAN:
		if (min_comp > 0 || (min_comp == 0 && min_type != StringStatsType::EXACT_STATS)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (min_comp >= 0) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	default:
		throw InternalException("Expression type not implemented for string statistics zone map");
	}
}

child_list_t<Value> StringStats::ToStruct(const BaseStatistics &stats) {
	child_list_t<Value> result;
	auto &string_data = GetDataUnsafe(stats);
	if (HasMinMax(stats)) {
		result.emplace_back("min", Blob::ToString(string_data.min));
		result.emplace_back("max", Blob::ToString(string_data.max));
	}
	result.emplace_back("has_unicode", Value::BOOLEAN(string_data.has_unicode));
	if (string_data.HasMinStringLength()) {
		result.emplace_back("min_string_length", Value::UBIGINT(string_data.min_string_length));
	}
	if (HasMaxStringLength(stats)) {
		result.emplace_back("max_string_length", Value::UBIGINT(string_data.max_string_length));
	}
	if (string_data.has_total_string_length) {
		result.emplace_back("total_string_length", Value::UBIGINT(string_data.total_string_length));
	}
	return result;
}

void StringStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &string_data = GetDataUnsafe(stats);

	auto entries = vector.Values<string_t>();
	idx_t str_len_in_vector = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto entry = entries[idx];
		if (!entry.IsValid()) {
			continue;
		}
		auto value = entry.GetValue();
		auto data = value.GetData();
		auto len = value.GetSize();
		// LCOV_EXCL_START
		if (string_data.has_max_string_length && len > string_data.max_string_length) {
			throw InternalException(
			    "Statistics mismatch: string value exceeds maximum string length.\nStatistics: %s\nVector: %s",
			    stats.ToString(), vector.ToString());
		}
		if (string_data.HasMinStringLength() && len < string_data.min_string_length) {
			throw InternalException(
			    "Statistics mismatch: string value exceeds minimum string length.\nStatistics: %s\nVector: %s",
			    stats.ToString(), vector.ToString());
		}
		if (stats.GetType().id() == LogicalTypeId::VARCHAR && !string_data.has_unicode) {
			auto unicode = Utf8Proc::Analyze(data, len);
			if (unicode == UnicodeType::UTF8) {
				throw InternalException("Statistics mismatch: string value contains unicode, but statistics says it "
				                        "shouldn't.\nStatistics: %s\nVector: %s",
				                        stats.ToString(), vector.ToString());
			} else if (unicode == UnicodeType::INVALID) {
				throw InternalException("Invalid unicode detected in vector: %s", vector.ToString());
			}
		}
		if (StatsIsSet(string_data.min_type)) {
			auto min_cmp = CompareStringStats(value, string_data.min, string_data.min_type);
			if (min_cmp < 0) {
				throw InternalException("Statistics mismatch: value is smaller than min.\nStatistics: %s\nVector: %s",
				                        stats.ToString(), vector.ToString());
			}
		}
		if (StatsIsSet(string_data.max_type)) {
			auto max_cmp = CompareStringStats(value, string_data.max, string_data.max_type);
			if (max_cmp > 0) {
				throw InternalException("Statistics mismatch: value is bigger than max.\nStatistics: %s\nVector: %s",
				                        stats.ToString(), vector.ToString());
			}
		}
		str_len_in_vector += len;
		// LCOV_EXCL_STOP
	}
	// this is not exhaustive but it's better than nothing
	if (string_data.has_total_string_length && str_len_in_vector > string_data.total_string_length) {
		throw InternalException(
		    "Statistics mismatch: string length in vector exceeds total string length reported in stats",
		    stats.ToString(), vector.ToString());
	}
}

} // namespace duckdb
