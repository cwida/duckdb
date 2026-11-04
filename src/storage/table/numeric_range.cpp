#include "duckdb/storage/table/numeric_range.hpp"
#include "duckdb/storage/statistics/numeric_range_stats.hpp"

#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/numeric_blob.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Values
//===--------------------------------------------------------------------===//
//! Exact values of any representation compare through their canonical encoding, which orders like the values; the
//! endpoints keep their own representations, which need not have one in common
static int CompareExact(const Value &left, const Value &right) {
	auto left_blob = NumericBlob::Encode(left.ToString());
	auto right_blob = NumericBlob::Encode(right.ToString());
	auto common = MinValue(left_blob.size(), right_blob.size());
	auto result = memcmp(left_blob.data(), right_blob.data(), common);
	if (result != 0) {
		return result < 0 ? -1 : 1;
	}
	if (left_blob.size() == right_blob.size()) {
		return 0;
	}
	return left_blob.size() < right_blob.size() ? -1 : 1;
}

void NumericRange::Merge(const NumericRange &other) {
	scale = MaxValue(scale, other.scale);
	if (!other.has_values) {
		return;
	}
	if (!has_values) {
		min = other.min;
		max = other.max;
		has_values = true;
		return;
	}
	if (CompareExact(other.min, min) < 0) {
		min = other.min;
	}
	if (CompareExact(other.max, max) > 0) {
		max = other.max;
	}
}

void NumericRange::Update(const Value &value) {
	if (value.IsNull()) {
		return;
	}
	if (!NumericType::IsExact(value.type())) {
		throw InternalException("NumericRange::Update called with a value of type %s", value.type().ToString());
	}
	NumericRange other;
	other.has_values = true;
	other.scale = value.type().id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(value.type()) : 0;
	other.min = value;
	other.max = value;
	Merge(other);
}

//===--------------------------------------------------------------------===//
// Vectors
//===--------------------------------------------------------------------===//
template <class T>
static bool FindMinMax(UnifiedVectorFormat &vdata, idx_t count, T &min, T &max) {
	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	bool found = false;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		if (!found) {
			min = data[idx];
			max = data[idx];
			found = true;
			continue;
		}
		if (data[idx] < min) {
			min = data[idx];
		}
		if (data[idx] > max) {
			max = data[idx];
		}
	}
	return found;
}

//! A value of an exact representation from its physical value
template <class T>
static Value ExactValue(const LogicalType &type, T value) {
	if (type.id() == LogicalTypeId::DECIMAL) {
		return Value::DECIMAL(value, DecimalType::GetWidth(type), DecimalType::GetScale(type));
	}
	return Value::Numeric(type, value);
}
template <>
Value ExactValue(const LogicalType &type, int8_t value) {
	if (type.id() == LogicalTypeId::DECIMAL) {
		return Value::DECIMAL(int16_t(value), DecimalType::GetWidth(type), DecimalType::GetScale(type));
	}
	return Value::Numeric(type, int64_t(value));
}

//! The fractional digits the values of a decimal vector need: trailing zeros do not count
template <class T>
static uint32_t NeededScale(UnifiedVectorFormat &vdata, idx_t count, uint32_t scale) {
	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	uint32_t needed = 0;
	for (idx_t i = 0; i < count && needed < scale; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		auto value = data[idx];
		uint32_t trailing_zeros = 0;
		while (trailing_zeros < scale && value % 10 == 0) {
			value /= 10;
			trailing_zeros++;
		}
		needed = MaxValue<uint32_t>(needed, scale - trailing_zeros);
	}
	return needed;
}

//! The fractional digits the values of a wide decimal vector need, from their decimal text
static uint32_t NeededScaleWide(UnifiedVectorFormat &vdata, idx_t count, const LogicalType &type) {
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	auto width = DecimalType::GetWidth(type);
	auto scale = DecimalType::GetScale(type);
	uint32_t needed = 0;
	for (idx_t i = 0; i < count && needed < scale; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		auto text = Value::WIDEDECIMAL(data[idx].GetString(), width, scale).ToString();
		auto point = text.find('.');
		if (point == string::npos) {
			continue;
		}
		idx_t end = text.size();
		while (end > point + 1 && text[end - 1] == '0') {
			end--;
		}
		needed = MaxValue<uint32_t>(needed, NumericCast<uint32_t>(end - point - 1));
	}
	return needed;
}

template <class T>
static bool UpdateTyped(UnifiedVectorFormat &vdata, idx_t count, const LogicalType &type, uint32_t type_scale,
                        Value &min, Value &max, uint32_t &needed_scale) {
	T lo, hi;
	if (!FindMinMax<T>(vdata, count, lo, hi)) {
		return false;
	}
	min = ExactValue<T>(type, lo);
	max = ExactValue<T>(type, hi);
	if (type_scale > 0) {
		needed_scale = NeededScale<T>(vdata, count, type_scale);
	}
	return true;
}

void NumericRange::UpdateCanonical(const Vector &canonical, idx_t count) {
	D_ASSERT(canonical.GetType().id() == LogicalTypeId::NUMERIC);
	UnifiedVectorFormat vdata;
	canonical.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	// the canonical encoding orders like the values
	string_t lo, hi;
	if (!FindMinMax<string_t>(vdata, count, lo, hi)) {
		return;
	}
	uint32_t needed_scale = 0;
	for (idx_t i = 0; i < count && needed_scale < Decimal::MAX_WIDTH_DECIMAL; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		idx_t integer_digits, fractional_digits;
		NumericBlob::Digits(data[idx], integer_digits, fractional_digits);
		needed_scale = MaxValue(needed_scale, NumericCast<uint32_t>(fractional_digits));
	}
	NumericRange other;
	other.has_values = true;
	other.scale = needed_scale;
	if (!NumericType::TryParseLiteral(NumericBlob::ToText(lo), other.min) ||
	    !NumericType::TryParseLiteral(NumericBlob::ToText(hi), other.max)) {
		throw InternalException("NumericRange::UpdateCanonical - canonical value does not parse");
	}
	Merge(other);
}

void NumericRange::Update(const Vector &vector, idx_t count) {
	auto &type = vector.GetType();
	if (type.id() == LogicalTypeId::NUMERIC) {
		UpdateCanonical(vector, count);
		return;
	}
	D_ASSERT(NumericType::IsExact(type));
	UnifiedVectorFormat vdata;
	vector.ToUnifiedFormat(vdata);
	uint32_t type_scale = type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : 0;
	uint32_t needed_scale = type_scale;
	Value vector_min, vector_max;
	bool found;
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		found = UpdateTyped<int8_t>(vdata, count, type, type_scale, vector_min, vector_max, needed_scale);
		break;
	case PhysicalType::INT16:
		found = UpdateTyped<int16_t>(vdata, count, type, type_scale, vector_min, vector_max, needed_scale);
		break;
	case PhysicalType::INT32:
		found = UpdateTyped<int32_t>(vdata, count, type, type_scale, vector_min, vector_max, needed_scale);
		break;
	case PhysicalType::INT64:
		found = UpdateTyped<int64_t>(vdata, count, type, type_scale, vector_min, vector_max, needed_scale);
		break;
	case PhysicalType::INT128:
		found = UpdateTyped<hugeint_t>(vdata, count, type, type_scale, vector_min, vector_max, needed_scale);
		break;
	case PhysicalType::VARCHAR: {
		// wide decimals: BIGNUM blobs, ordered like their values
		string_t lo, hi;
		found = FindMinMax<string_t>(vdata, count, lo, hi);
		if (found) {
			vector_min = Value::WIDEDECIMAL(lo.GetString(), DecimalType::GetWidth(type), type_scale);
			vector_max = Value::WIDEDECIMAL(hi.GetString(), DecimalType::GetWidth(type), type_scale);
			if (type_scale > 0) {
				needed_scale = NeededScaleWide(vdata, count, type);
			}
		}
		break;
	}
	default:
		// NUMERIC data is stored in signed integers and decimals
		throw InternalException("NUMERIC data in an unexpected representation %s", type.ToString());
	}
	if (!found) {
		return;
	}
	if (needed_scale < type_scale) {
		// whole numbers, or fewer fractional digits than the type: record them that way
		auto reduced = LogicalType::DECIMAL(DecimalType::GetWidth(type), needed_scale);
		vector_min = vector_min.DefaultCastAs(reduced);
		vector_max = vector_max.DefaultCastAs(reduced);
	}
	NumericRange other;
	other.has_values = true;
	other.scale = needed_scale;
	other.min = std::move(vector_min);
	other.max = std::move(vector_max);
	Merge(other);
}

bool NumericRange::TryUpdate(const BaseStatistics &typed_stats) {
	auto &type = typed_stats.GetType();
	if (!NumericType::IsExact(type)) {
		return false;
	}
	switch (typed_stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		if (!NumericStats::HasMinMax(typed_stats)) {
			return !typed_stats.CanHaveNoNull();
		}
		Update(NumericStats::Min(typed_stats));
		Update(NumericStats::Max(typed_stats));
		return true;
	case StatisticsType::STRING_STATS:
		// wide decimals: the statistics are exact only for short blobs
		if (StringStats::GetMinType(typed_stats) != StringStatsType::EXACT_STATS ||
		    StringStats::GetMaxType(typed_stats) != StringStatsType::EXACT_STATS) {
			return false;
		}
		Update(Value::WIDEDECIMAL(StringStats::Min(typed_stats), DecimalType::GetWidth(type),
		                          DecimalType::GetScale(type)));
		Update(Value::WIDEDECIMAL(StringStats::Max(typed_stats), DecimalType::GetWidth(type),
		                          DecimalType::GetScale(type)));
		return true;
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// Types and statistics
//===--------------------------------------------------------------------===//
//! The unscaled value of an exact Value held in a fixed-size integer
static bool TryGetUnscaledValue(const Value &value, hugeint_t &result) {
	switch (value.type().InternalType()) {
	case PhysicalType::INT8:
		result = value.GetValueUnsafe<int8_t>();
		return true;
	case PhysicalType::INT16:
		result = value.GetValueUnsafe<int16_t>();
		return true;
	case PhysicalType::INT32:
		result = value.GetValueUnsafe<int32_t>();
		return true;
	case PhysicalType::INT64:
		result = value.GetValueUnsafe<int64_t>();
		return true;
	case PhysicalType::INT128:
		result = value.GetValueUnsafe<hugeint_t>();
		return true;
	default:
		return false;
	}
}

//! The unscaled value of an endpoint at `target_scale`, rescaled from the endpoint's own representation: the endpoints
//! need not share a scale, so each is rescaled from its own type rather than the range's combined scale
static bool TryGetUnscaledAt(const Value &value, uint32_t target_scale, hugeint_t &result) {
	uint32_t own_scale = value.type().id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(value.type()) : 0;
	if (target_scale < own_scale || target_scale - own_scale >= Hugeint::CACHED_POWERS_OF_TEN) {
		return false;
	}
	if (!TryGetUnscaledValue(value, result)) {
		return false;
	}
	if (target_scale > own_scale) {
		auto &factor = Hugeint::POWERS_OF_TEN[target_scale - own_scale];
		if (!Hugeint::TryMultiply(result, factor, result)) {
			return false;
		}
	}
	return true;
}

bool NumericRange::TryGetUnscaled(const LogicalType &representation, ShreddedNumericRange &result) const {
	result.valid = false;
	if (!has_values || !NumericType::IsExact(representation)) {
		return false;
	}
	uint32_t target_scale = representation.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(representation) : 0;
	hugeint_t unscaled_min, unscaled_max;
	if (!TryGetUnscaledAt(min, target_scale, unscaled_min) || !TryGetUnscaledAt(max, target_scale, unscaled_max)) {
		return false;
	}
	result.valid = true;
	result.scale = target_scale;
	result.min = unscaled_min;
	result.max = unscaled_max;
	return true;
}

LogicalType NumericRange::ResolveType() const {
	if (!has_values) {
		return NumericType::DefaultType();
	}
	LogicalType type;
	if (!NumericType::TryForRange(min, max, scale, type)) {
		// no representation holds every value: they are stored as canonical values
		return NumericType::CanonicalStorageType();
	}
	return type;
}

//! Whether the value survives a round trip through the type, i.e. the type can hold it exactly
static bool TryExactCast(const Value &value, const LogicalType &type, Value &result) {
	auto cast = value.DefaultTryCastAs(type, nullptr, true);
	if (!cast) {
		return false;
	}
	auto back = cast->DefaultTryCastAs(value.type(), nullptr, true);
	if (!back || !ValueOperations::Equals(*back, value)) {
		return false;
	}
	result = *cast;
	return true;
}

BaseStatistics NumericRange::ToTyped(const LogicalType &type) const {
	if (!NumericType::IsExact(type)) {
		// canonical storage: the string statistics of the blobs are not exact beyond their prefix
		return BaseStatistics::CreateUnknown(type);
	}
	Value typed_min, typed_max;
	if (has_values && (!TryExactCast(min, type, typed_min) || !TryExactCast(max, type, typed_max))) {
		return BaseStatistics::CreateUnknown(type);
	}
	if (BaseStatistics::GetStatsType(type) == StatisticsType::STRING_STATS) {
		// wide decimals keep their BIGNUM blobs as string statistics
		auto result = StringStats::CreateEmpty(type);
		if (has_values) {
			StringStats::FromConstant(result, string_t(StringValue::Get(typed_min)));
			StringStats::MergeInConstant(result, string_t(StringValue::Get(typed_max)));
		}
		return result;
	}
	auto result = NumericStats::CreateEmpty(type);
	if (has_values) {
		NumericStats::SetMin(result, typed_min);
		NumericStats::SetMax(result, typed_max);
	}
	return result;
}

BaseStatistics NumericRange::ToCanonical() const {
	auto result = BaseStatistics::CreateEmpty(LogicalType::NUMERIC);
	if (has_values) {
		auto min_blob = NumericBlob::Encode(min.ToString());
		auto max_blob = NumericBlob::Encode(max.ToString());
		StringStats::FromConstant(result, string_t(min_blob));
		StringStats::MergeInConstant(result, string_t(max_blob));
		// the endpoints bound the values, not the lengths of their blobs: zero is one byte, a value between the
		// endpoints can carry more significant digits than either
		StringStats::ResetMinStringLength(result);
		StringStats::ResetMaxStringLength(result);
	}
	NumericRangeStats::Set(result, *this);
	return result;
}

//===--------------------------------------------------------------------===//
// Serialization
//===--------------------------------------------------------------------===//
void NumericRange::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<bool>(100, "has_values", has_values, false);
	serializer.WritePropertyWithDefault<uint8_t>(101, "scale", NumericCast<uint8_t>(scale), 0);
	serializer.WritePropertyWithDefault<string>(102, "min", has_values ? min.ToString() : string(), string());
	serializer.WritePropertyWithDefault<string>(103, "max", has_values ? max.ToString() : string(), string());
}

NumericRange NumericRange::Deserialize(Deserializer &deserializer) {
	NumericRange result;
	auto has_values = deserializer.ReadPropertyWithExplicitDefault<bool>(100, "has_values", false);
	auto scale = deserializer.ReadPropertyWithExplicitDefault<uint8_t>(101, "scale", 0);
	auto min_text = deserializer.ReadPropertyWithExplicitDefault<string>(102, "min", string());
	auto max_text = deserializer.ReadPropertyWithExplicitDefault<string>(103, "max", string());
	if (has_values) {
		Value min_value, max_value;
		if (!NumericType::TryParseLiteral(min_text, min_value) || !NumericType::TryParseLiteral(max_text, max_value)) {
			throw SerializationException("Invalid NUMERIC range '%s' .. '%s'", min_text, max_text);
		}
		result.Update(min_value);
		result.Update(max_value);
		result.scale = MaxValue<uint32_t>(result.scale, scale);
	} else {
		result.scale = scale;
	}
	return result;
}

} // namespace duckdb
