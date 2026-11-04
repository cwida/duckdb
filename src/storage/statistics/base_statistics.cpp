#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

BaseStatistics::BaseStatistics() : type(LogicalType::INVALID), has_null(false), has_no_null(false), distinct_count(0) {
	memset(&stats_union, 0, sizeof(stats_union));
}

BaseStatistics::BaseStatistics(LogicalType type) {
	Construct(*this, std::move(type));
}

void BaseStatistics::Construct(BaseStatistics &stats, LogicalType type) {
	stats.has_null = false;
	stats.has_no_null = false;
	stats.distinct_count = 0;
	memset(&stats.stats_union, 0, sizeof(stats.stats_union));
	stats.type = std::move(type);
	switch (GetStatsType(stats.type)) {
	case StatisticsType::LIST_STATS:
		ListStats::Construct(stats);
		break;
	case StatisticsType::STRUCT_STATS:
		StructStats::Construct(stats);
		break;
	case StatisticsType::ARRAY_STATS:
		ArrayStats::Construct(stats);
		break;
	case StatisticsType::VARIANT_STATS:
		VariantStats::Construct(stats);
		break;
	default:
		break;
	}
}

BaseStatistics::~BaseStatistics() {
}

BaseStatistics::BaseStatistics(BaseStatistics &&other) noexcept {
	std::swap(type, other.type);
	has_null = other.has_null;
	has_no_null = other.has_no_null;
	distinct_count = other.distinct_count;
	stats_union = other.stats_union;
	std::swap(extra_data, other.extra_data);
	std::swap(child_stats, other.child_stats);
}

BaseStatistics &BaseStatistics::operator=(BaseStatistics &&other) noexcept {
	std::swap(type, other.type);
	has_null = other.has_null;
	has_no_null = other.has_no_null;
	distinct_count = other.distinct_count;
	stats_union = other.stats_union;
	std::swap(extra_data, other.extra_data);
	std::swap(child_stats, other.child_stats);
	return *this;
}

StatisticsType BaseStatistics::GetStatsType(const LogicalType &type) {
	if (type.id() == LogicalTypeId::SQLNULL) {
		return StatisticsType::BASE_STATS;
	}
	if (type.id() == LogicalTypeId::GEOMETRY || type.id() == LogicalTypeId::GEOGRAPHY) {
		return StatisticsType::GEOMETRY_STATS;
	}
	if (type.id() == LogicalTypeId::VARIANT) {
		return StatisticsType::VARIANT_STATS;
	}
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
	case PhysicalType::INTERVAL:
		return StatisticsType::NUMERIC_STATS;
	case PhysicalType::VARCHAR:
		return StatisticsType::STRING_STATS;
	case PhysicalType::STRUCT:
		return StatisticsType::STRUCT_STATS;
	case PhysicalType::LIST:
		return StatisticsType::LIST_STATS;
	case PhysicalType::ARRAY:
		return StatisticsType::ARRAY_STATS;
	case PhysicalType::BIT:
	default:
		return StatisticsType::BASE_STATS;
	}
}

StatisticsType BaseStatistics::GetStatsType() const {
	return GetStatsType(GetType());
}

void BaseStatistics::InitializeUnknown() {
	has_null = true;
	has_no_null = true;
}

void BaseStatistics::InitializeEmpty() {
	has_null = false;
	has_no_null = false;
}

bool BaseStatistics::CanHaveNull() const {
	return has_null;
}

bool BaseStatistics::CanHaveNoNull() const {
	return has_no_null;
}

bool BaseStatistics::IsConstant() const {
	if (type.id() == LogicalTypeId::VALIDITY) {
		// validity mask
		if (CanHaveNull() && !CanHaveNoNull()) {
			return true;
		}
		if (!CanHaveNull() && CanHaveNoNull()) {
			return true;
		}
		return false;
	}
	switch (GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		return ConstantFun::TypeIsSupported(type.InternalType()) && NumericStats::IsConstant(*this);
	default:
		break;
	}
	return false;
}

void BaseStatistics::Merge(const BaseStatistics &other, StatsMergeType merge_type) {
	has_null = has_null || other.has_null;
	has_no_null = has_no_null || other.has_no_null;
	switch (GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		NumericStats::Merge(*this, other);
		break;
	case StatisticsType::STRING_STATS:
		StringStats::Merge(*this, other, merge_type);
		break;
	case StatisticsType::LIST_STATS:
		ListStats::Merge(*this, other, merge_type);
		break;
	case StatisticsType::STRUCT_STATS:
		StructStats::Merge(*this, other, merge_type);
		break;
	case StatisticsType::ARRAY_STATS:
		ArrayStats::Merge(*this, other, merge_type);
		break;
	case StatisticsType::GEOMETRY_STATS:
		GeometryStats::Merge(*this, other);
		break;
	case StatisticsType::VARIANT_STATS:
		VariantStats::Merge(*this, other);
		break;
	default:
		break;
	}
}

void BaseStatistics::ResetAdditiveStatistics() {
	switch (GetStatsType()) {
	case StatisticsType::STRING_STATS:
		stats_union.string_data.has_total_string_length = false;
		break;
	case StatisticsType::LIST_STATS:
	case StatisticsType::ARRAY_STATS:
		child_stats[0].ResetAdditiveStatistics();
		break;
	case StatisticsType::STRUCT_STATS:
		for (idx_t child_idx = 0; child_idx < StructType::GetChildCount(type); child_idx++) {
			child_stats[child_idx].ResetAdditiveStatistics();
		}
		break;
	case StatisticsType::VARIANT_STATS:
		child_stats[0].ResetAdditiveStatistics();
		if (child_stats[1].GetType().id() != LogicalTypeId::INVALID) {
			child_stats[1].ResetAdditiveStatistics();
		}
		break;
	default:
		break;
	}
}

idx_t BaseStatistics::GetDistinctCount() {
	return distinct_count;
}

BaseStatistics BaseStatistics::CreateUnknownType(LogicalType type) {
	switch (GetStatsType(type)) {
	case StatisticsType::NUMERIC_STATS:
		return NumericStats::CreateUnknown(std::move(type));
	case StatisticsType::STRING_STATS:
		return StringStats::CreateUnknown(std::move(type));
	case StatisticsType::LIST_STATS:
		return ListStats::CreateUnknown(std::move(type));
	case StatisticsType::STRUCT_STATS:
		return StructStats::CreateUnknown(std::move(type));
	case StatisticsType::ARRAY_STATS:
		return ArrayStats::CreateUnknown(std::move(type));
	case StatisticsType::GEOMETRY_STATS:
		return GeometryStats::CreateUnknown(std::move(type));
	case StatisticsType::VARIANT_STATS:
		return VariantStats::CreateUnknown(std::move(type));
	default:
		return BaseStatistics(std::move(type));
	}
}

BaseStatistics BaseStatistics::CreateEmptyType(LogicalType type) {
	switch (GetStatsType(type)) {
	case StatisticsType::NUMERIC_STATS:
		return NumericStats::CreateEmpty(std::move(type));
	case StatisticsType::STRING_STATS:
		return StringStats::CreateEmpty(std::move(type));
	case StatisticsType::LIST_STATS:
		return ListStats::CreateEmpty(std::move(type));
	case StatisticsType::STRUCT_STATS:
		return StructStats::CreateEmpty(std::move(type));
	case StatisticsType::ARRAY_STATS:
		return ArrayStats::CreateEmpty(std::move(type));
	case StatisticsType::GEOMETRY_STATS:
		return GeometryStats::CreateEmpty(std::move(type));
	case StatisticsType::VARIANT_STATS:
		return VariantStats::CreateEmpty(std::move(type));
	default:
		return BaseStatistics(std::move(type));
	}
}

BaseStatistics BaseStatistics::CreateUnknown(LogicalType type) {
	auto result = CreateUnknownType(std::move(type));
	result.InitializeUnknown();
	return result;
}

BaseStatistics BaseStatistics::CreateEmpty(LogicalType type) {
	if (type.InternalType() == PhysicalType::BIT) {
		// FIXME: this special case should not be necessary
		// but currently InitializeEmpty sets StatsInfo::CAN_HAVE_VALID_VALUES
		BaseStatistics result(std::move(type));
		result.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		result.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
		return result;
	}
	auto result = CreateEmptyType(std::move(type));
	result.InitializeEmpty();
	return result;
}

void BaseStatistics::Copy(const BaseStatistics &other) {
	D_ASSERT(GetType() == other.GetType());
	CopyBase(other);
	auto stats_type = GetStatsType();

	stats_union = other.stats_union;
	switch (stats_type) {
	case StatisticsType::LIST_STATS:
		ListStats::Copy(*this, other);
		break;
	case StatisticsType::STRUCT_STATS:
		StructStats::Copy(*this, other);
		break;
	case StatisticsType::ARRAY_STATS:
		ArrayStats::Copy(*this, other);
		break;
	case StatisticsType::VARIANT_STATS:
		VariantStats::Copy(*this, other);
		break;
	case StatisticsType::STRING_STATS:
		StringStats::Copy(*this, other);
		break;
	default:
		break;
	}
}

unique_ptr<BaseStatistics> BaseStatistics::PushdownExtract(const StorageIndex &index) const {
	auto stats_type = GetStatsType();
	switch (stats_type) {
	case StatisticsType::STRUCT_STATS:
		return StructStats::PushdownExtract(*this, index);
	case StatisticsType::VARIANT_STATS:
		return VariantStats::PushdownExtract(*this, index);
	default:
		throw InternalException("PushdownExtract not supported for StatisticsType::%s", EnumUtil::ToString(stats_type));
	}
}

BaseStatistics BaseStatistics::Copy() const {
	BaseStatistics result(type);
	result.Copy(*this);
	return result;
}

unique_ptr<BaseStatistics> BaseStatistics::ToUnique() const {
	auto result = unique_ptr<BaseStatistics>(new BaseStatistics(type));
	result->Copy(*this);
	return result;
}

void BaseStatistics::CopyBase(const BaseStatistics &other) {
	has_null = other.has_null;
	has_no_null = other.has_no_null;
	distinct_count = other.distinct_count;
}

void BaseStatistics::Set(StatsInfo info) {
	switch (info) {
	case StatsInfo::CAN_HAVE_NULL_VALUES:
		SetHasNull();
		break;
	case StatsInfo::CANNOT_HAVE_NULL_VALUES:
		has_null = false;
		break;
	case StatsInfo::CAN_HAVE_VALID_VALUES:
		SetHasNoNull();
		break;
	case StatsInfo::CANNOT_HAVE_VALID_VALUES:
		has_no_null = false;
		break;
	case StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES:
		SetHasNull();
		SetHasNoNull();
		break;
	default:
		throw InternalException("Unrecognized StatsInfo for BaseStatistics::Set");
	}
}

void BaseStatistics::SetHasNull() {
	has_null = true;
	if (type.id() == LogicalTypeId::VARIANT) {
		VariantStats::GetUnshreddedStats(*this).SetHasNull();
		return;
	}
	if (type.InternalType() == PhysicalType::STRUCT) {
		for (idx_t c = 0; c < StructType::GetChildCount(type); c++) {
			StructStats::GetChildStats(*this, c).SetHasNull();
		}
	}
}

void BaseStatistics::SetHasNoNull() {
	has_no_null = true;
	if (type.id() == LogicalTypeId::VARIANT) {
		VariantStats::GetUnshreddedStats(*this).SetHasNoNull();
		return;
	}
	if (type.InternalType() == PhysicalType::STRUCT) {
		for (idx_t c = 0; c < StructType::GetChildCount(type); c++) {
			StructStats::GetChildStats(*this, c).SetHasNoNull();
		}
	}
}

void BaseStatistics::CombineValidity(const BaseStatistics &left, const BaseStatistics &right) {
	has_null = left.has_null || right.has_null;
	has_no_null = left.has_no_null || right.has_no_null;
}

void BaseStatistics::CopyValidity(const BaseStatistics &stats) {
	has_null = stats.has_null;
	has_no_null = stats.has_no_null;
}

void BaseStatistics::SetDistinctCount(idx_t count) {
	this->distinct_count = count;
}

void BaseStatistics::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "has_null", has_null);
	serializer.WriteProperty(101, "has_no_null", has_no_null);
	serializer.WriteProperty(102, "distinct_count", distinct_count);
	serializer.WriteObject(103, "type_stats", [&](Serializer &serializer) {
		switch (GetStatsType()) {
		case StatisticsType::NUMERIC_STATS:
			NumericStats::Serialize(*this, serializer);
			break;
		case StatisticsType::STRING_STATS:
			StringStats::Serialize(*this, serializer);
			break;
		case StatisticsType::LIST_STATS:
			ListStats::Serialize(*this, serializer);
			break;
		case StatisticsType::STRUCT_STATS:
			StructStats::Serialize(*this, serializer);
			break;
		case StatisticsType::ARRAY_STATS:
			ArrayStats::Serialize(*this, serializer);
			break;
		case StatisticsType::GEOMETRY_STATS:
			GeometryStats::Serialize(*this, serializer);
			break;
		case StatisticsType::VARIANT_STATS:
			VariantStats::Serialize(*this, serializer);
			break;
		default:
			break;
		}
	});
}

BaseStatistics BaseStatistics::Deserialize(Deserializer &deserializer) {
	auto has_null = deserializer.ReadProperty<bool>(100, "has_null");
	auto has_no_null = deserializer.ReadProperty<bool>(101, "has_no_null");
	auto distinct_count = deserializer.ReadProperty<idx_t>(102, "distinct_count");

	// Get the logical type from the deserializer context.
	auto &type = deserializer.Get<const LogicalType &>();
	auto stats_type = GetStatsType(type);

	BaseStatistics stats(type);

	stats.has_null = has_null;
	stats.has_no_null = has_no_null;
	stats.distinct_count = distinct_count;

	deserializer.ReadObject(103, "type_stats", [&](Deserializer &obj) {
		switch (stats_type) {
		case StatisticsType::NUMERIC_STATS:
			NumericStats::Deserialize(obj, stats);
			break;
		case StatisticsType::STRING_STATS:
			StringStats::Deserialize(obj, stats);
			break;
		case StatisticsType::LIST_STATS:
			ListStats::Deserialize(obj, stats);
			break;
		case StatisticsType::STRUCT_STATS:
			StructStats::Deserialize(obj, stats);
			break;
		case StatisticsType::ARRAY_STATS:
			ArrayStats::Deserialize(obj, stats);
			break;
		case StatisticsType::GEOMETRY_STATS:
			GeometryStats::Deserialize(obj, stats);
			break;
		case StatisticsType::VARIANT_STATS:
			VariantStats::Deserialize(obj, stats);
			break;
		default:
			break;
		}
	});

	return stats;
}

Value BaseStatistics::ToStruct() const {
	child_list_t<Value> children;
	switch (GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		children = NumericStats::ToStruct(*this);
		break;
	case StatisticsType::STRING_STATS:
		children = StringStats::ToStruct(*this);
		break;
	case StatisticsType::LIST_STATS:
		children = ListStats::ToStruct(*this);
		break;
	case StatisticsType::STRUCT_STATS:
		children = StructStats::ToStruct(*this);
		break;
	case StatisticsType::ARRAY_STATS:
		children = ArrayStats::ToStruct(*this);
		break;
	case StatisticsType::GEOMETRY_STATS:
		children = GeometryStats::ToStruct(*this);
		break;
	case StatisticsType::VARIANT_STATS:
		children = VariantStats::ToStruct(*this);
		break;
	default:
		break;
	}
	children.emplace_back("has_null", Value::BOOLEAN(has_null));
	children.emplace_back("has_no_null", Value::BOOLEAN(has_no_null));
	if (distinct_count > 0) {
		children.emplace_back("approx_unique", Value::UBIGINT(distinct_count));
	}
	return Value::STRUCT(std::move(children));
}

string BaseStatistics::ToString() const {
	return ToStruct().ToString();
}

void BaseStatistics::Verify(const Vector &vector, const SelectionVector &sel, idx_t count,
                            const bool ignore_has_null) const {
	D_ASSERT(vector.GetType() == this->type);
	switch (GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		NumericStats::Verify(*this, vector, sel, count);
		break;
	case StatisticsType::STRING_STATS:
		StringStats::Verify(*this, vector, sel, count);
		break;
	case StatisticsType::LIST_STATS:
		ListStats::Verify(*this, vector, sel, count);
		break;
	case StatisticsType::STRUCT_STATS:
		StructStats::Verify(*this, vector, sel, count);
		break;
	case StatisticsType::ARRAY_STATS:
		ArrayStats::Verify(*this, vector, sel, count);
		break;
	case StatisticsType::GEOMETRY_STATS:
		GeometryStats::Verify(*this, vector, sel, count);
		break;
	case StatisticsType::VARIANT_STATS:
		VariantStats::Verify(*this, vector, sel, count);
		break;
	default:
		break;
	}
	if (has_null && has_no_null) {
		// nothing to verify
		return;
	}
	auto validity_entries = vector.Validity();
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		bool row_is_valid = validity_entries.IsValid(idx);
		if (row_is_valid && !has_no_null) {
			throw InternalException(
			    "Statistics mismatch: vector labeled as having only NULL values, but vector contains valid values: %s",
			    vector.ToString());
		}
		if (!row_is_valid && !has_null && !ignore_has_null) {
			throw InternalException(
			    "Statistics mismatch: vector labeled as not having NULL values, but vector contains null values: %s",
			    vector.ToString());
		}
	}
}

void BaseStatistics::Verify(const Vector &vector, idx_t count) const {
	auto sel = FlatVector::IncrementalSelectionVector();
	Verify(vector, *sel, count, false);
}

BaseStatistics BaseStatistics::FromConstantType(const Value &input) {
	switch (GetStatsType(input.type())) {
	case StatisticsType::NUMERIC_STATS: {
		auto result = NumericStats::CreateEmpty(input.type());
		NumericStats::SetMin(result, input);
		NumericStats::SetMax(result, input);
		return result;
	}
	case StatisticsType::STRING_STATS: {
		auto result = StringStats::CreateEmpty(input.type());
		if (!input.IsNull()) {
			auto &string_value = StringValue::Get(input);
			StringStats::FromConstant(result, string_t(string_value));
		}
		return result;
	}
	case StatisticsType::LIST_STATS: {
		auto result = ListStats::CreateEmpty(input.type());
		auto &child_stats = ListStats::GetChildStats(result);
		if (!input.IsNull()) {
			auto &list_children = ListValue::GetChildren(input);
			for (auto &child_element : list_children) {
				child_stats.Merge(FromConstant(child_element));
			}
		}
		return result;
	}
	case StatisticsType::STRUCT_STATS: {
		auto result = StructStats::CreateEmpty(input.type());
		auto &child_types = StructType::GetChildTypes(input.type());
		if (input.IsNull()) {
			for (idx_t i = 0; i < child_types.size(); i++) {
				StructStats::SetChildStats(result, i, FromConstant(Value(child_types[i].second)));
			}
		} else {
			auto &struct_children = StructValue::GetChildren(input);
			for (idx_t i = 0; i < child_types.size(); i++) {
				StructStats::SetChildStats(result, i, FromConstant(struct_children[i]));
			}
		}
		return result;
	}
	case StatisticsType::ARRAY_STATS: {
		auto result = ArrayStats::CreateEmpty(input.type());
		auto &child_stats = ArrayStats::GetChildStats(result);
		if (!input.IsNull()) {
			auto &list_children = ArrayValue::GetChildren(input);
			for (auto &child_element : list_children) {
				child_stats.Merge(FromConstant(child_element));
			}
		}
		return result;
	}
	case StatisticsType::GEOMETRY_STATS: {
		auto result = GeometryStats::CreateEmpty(input.type());
		if (!input.IsNull()) {
			auto &string_value = StringValue::Get(input);
			GeometryStats::Update(result, string_t(string_value));
		}
		return result;
	}
	case StatisticsType::VARIANT_STATS: {
		auto result = VariantStats::CreateEmpty(input.type());
		auto unshredded_type = VariantShredding::GetUnshreddedType();
		if (input.IsNull()) {
			VariantStats::SetUnshreddedStats(result, FromConstant(Value(unshredded_type)));
		} else {
			VariantStats::SetUnshreddedStats(
			    result, FromConstant(Value::STRUCT(unshredded_type, StructValue::GetChildren(input))));
		}
		return result;
	}
	default:
		return BaseStatistics(input.type());
	}
}

BaseStatistics BaseStatistics::FromConstant(const Value &input) {
	auto result = FromConstantType(input);
	result.SetDistinctCount(1);
	if (input.IsNull()) {
		result.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		result.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	} else {
		result.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		result.Set(StatsInfo::CAN_HAVE_VALID_VALUES);
	}
	return result;
}

} // namespace duckdb
