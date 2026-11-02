#include "duckdb/storage/statistics/variant_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/type_visitor.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include "duckdb/common/types/variant_visitor.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"

namespace duckdb {

static void AssertVariant(const BaseStatistics &stats) {
	if (DUCKDB_UNLIKELY(stats.GetStatsType() != StatisticsType::VARIANT_STATS)) {
		throw InternalException(
		    "Calling a VariantStats method on BaseStatistics that are not of type VARIANT, but of type %s",
		    EnumUtil::ToString(stats.GetStatsType()));
	}
}

void VariantStats::Construct(BaseStatistics &stats) {
	stats.child_stats = unsafe_unique_array<BaseStatistics>(new BaseStatistics[2]);
	GetDataUnsafe(stats).shredding_state = VariantStatsShreddingState::UNINITIALIZED;
	CreateUnshreddedStats(stats);
}

BaseStatistics VariantStats::CreateUnknown(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	//! Unknown - we have no clue what's in this
	GetDataUnsafe(result).shredding_state = VariantStatsShreddingState::INCONSISTENT;
	result.child_stats[0].Copy(BaseStatistics::CreateUnknown(VariantShredding::GetUnshreddedType()));
	return result;
}

BaseStatistics VariantStats::CreateEmpty(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	GetDataUnsafe(result).shredding_state = VariantStatsShreddingState::UNINITIALIZED;
	result.child_stats[0].Copy(BaseStatistics::CreateEmpty(VariantShredding::GetUnshreddedType()));
	return result;
}

//===--------------------------------------------------------------------===//
// Unshredded Stats
//===--------------------------------------------------------------------===//

void VariantStats::CreateUnshreddedStats(BaseStatistics &stats) {
	BaseStatistics::Construct(stats.child_stats[0], VariantShredding::GetUnshreddedType());
}

const BaseStatistics &VariantStats::GetUnshreddedStats(const BaseStatistics &stats) {
	AssertVariant(stats);
	return stats.child_stats[0];
}

BaseStatistics &VariantStats::GetUnshreddedStats(BaseStatistics &stats) {
	AssertVariant(stats);
	return stats.child_stats[0];
}

const BaseStatistics &VariantStats::GetTypedStats(const BaseStatistics &stats) {
	if (stats.GetType().id() != LogicalTypeId::STRUCT) {
		// primitive stats - return the stats directly
		return stats;
	}
	// STRUCT(typed_value, (untyped_value)) - return the typed_value stats
	return StructStats::GetChildStats(stats, TYPED_VALUE_INDEX);
}

optional_ptr<const BaseStatistics> VariantStats::GetUntypedStats(const BaseStatistics &stats) {
	if (stats.GetType().id() != LogicalTypeId::STRUCT) {
		// primitive stats - no untyped stats
		return nullptr;
	}
	// STRUCT(typed_value, (untyped_value)) - check if we have untyped stats
	if (StructType::GetChildCount(stats.GetType()) == 1) {
		// fully shredded and no untyped stats
		return nullptr;
	}
	return StructStats::GetChildStats(stats, UNTYPED_VALUE_INDEX);
}

void VariantStats::SetUnshreddedStats(BaseStatistics &stats, const BaseStatistics &new_stats) {
	AssertVariant(stats);
	stats.child_stats[0].Copy(new_stats);
}

void VariantStats::SetUnshreddedStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats) {
	AssertVariant(stats);
	if (!new_stats) {
		CreateUnshreddedStats(stats);
	} else {
		SetUnshreddedStats(stats, *new_stats);
	}
}

void VariantStats::MarkAsNotShredded(BaseStatistics &stats) {
	D_ASSERT(!IsShredded(stats));
	auto &data = GetDataUnsafe(stats);
	//! All Variant stats start off as UNINITIALIZED, to support merging
	//! This method marks the stats as being unshredded, so they produce INCONSISTENT when merged with SHREDDED stats
	data.shredding_state = VariantStatsShreddingState::NOT_SHREDDED;
}

//===--------------------------------------------------------------------===//
// Shredded Stats
//===--------------------------------------------------------------------===//

static void AssertShreddedStats(const BaseStatistics &stats) {
	auto &stats_type = stats.GetType();
	if (stats_type.id() != LogicalTypeId::STRUCT) {
		// primitive stats - this is fine
		return;
	}
	auto &struct_children = StructType::GetChildTypes(stats_type);
	if (struct_children.size() > 2 || struct_children[VariantStats::TYPED_VALUE_INDEX].first != "typed_value") {
		throw InternalException("Shredded stats need to consist of 1 or 2 children, 'typed_value' and optionally "
		                        "'untyped_value_index', not: %s",
		                        stats.GetType().ToString());
	}

	if (struct_children.size() == 2) {
		auto &untyped_entry = struct_children[VariantStats::UNTYPED_VALUE_INDEX];
		if (untyped_entry.first != "untyped_value_index") {
			throw InternalException("Untyped value index entry should be called \"untyped_value_index\"");
		}
		if (untyped_entry.second.id() != LogicalTypeId::UINTEGER) {
			throw InternalException("Shredded stats 'untyped_value_index' should be of type UINTEGER, not %s",
			                        EnumUtil::ToString(untyped_entry.second.id()));
		}
	}
}

optional_ptr<const BaseStatistics> VariantShreddedStats::FindChildStats(const BaseStatistics &stats,
                                                                        const VariantPathComponent &component) {
	const_reference<BaseStatistics> typed_value_stats_ref(stats);
	const_reference<LogicalType> typed_value_type_ref(stats.GetType());
	if (typed_value_type_ref.get().IsNested()) {
		// "typed_value" / "untyped_value"
		AssertShreddedStats(stats);

		typed_value_stats_ref = StructStats::GetChildStats(stats, VariantStats::TYPED_VALUE_INDEX);
		typed_value_type_ref = typed_value_stats_ref.get().GetType();
	}
	auto &typed_value_stats = typed_value_stats_ref.get();
	auto &typed_value_type = typed_value_type_ref.get();

	switch (component.lookup_mode) {
	case VariantChildLookupMode::BY_INDEX: {
		if (typed_value_type.id() != LogicalTypeId::LIST) {
			return nullptr;
		}
		auto &child_stats = ListStats::GetChildStats(typed_value_stats);
		return child_stats;
	}
	case VariantChildLookupMode::BY_KEY: {
		if (typed_value_type.id() != LogicalTypeId::STRUCT) {
			return nullptr;
		}
		auto &object_fields = StructType::GetChildTypes(typed_value_type);
		for (idx_t i = 0; i < object_fields.size(); i++) {
			auto &object_field = object_fields[i];
			if (object_field.first.GetIdentifierName() == component.key) {
				return StructStats::GetChildStats(typed_value_stats, i);
			}
		}
		return nullptr;
	}
	default:
		throw InternalException("VariantChildLookupMode::%s not implemented for FindShreddedStats",
		                        EnumUtil::ToString(component.lookup_mode));
	}
}

bool VariantShreddedStats::IsFullyShredded(const BaseStatistics &stats) {
	auto &stats_type = stats.GetType();
	if (!stats_type.IsNested()) {
		// if this is a primitive type this is fully nested
		return true;
	}
	AssertShreddedStats(stats);
	if (StructType::GetChildCount(stats_type) == 1) {
		// we don't have untyped values - this must be fully shredded
		return true;
	}

	auto &typed_value_stats = StructStats::GetChildStats(stats, VariantStats::TYPED_VALUE_INDEX);
	auto &untyped_value_index_stats = StructStats::GetChildStats(stats, VariantStats::UNTYPED_VALUE_INDEX);

	if (!typed_value_stats.CanHaveNull()) {
		//! Fully shredded, no nulls
		return true;
	}
	if (!untyped_value_index_stats.CanHaveNoNull()) {
		//! In the event that the untyped_value_index is entirely NULL, all values are NULL Variant values
		//! But that doesn't mean we can't do pushdown into this field, so it is shredded (only when the extract path
		//! ends at the parent we can't do pushdown)
		D_ASSERT(untyped_value_index_stats.CanHaveNull());
		return true;
	}
	if (!NumericStats::HasMin(untyped_value_index_stats) || !NumericStats::HasMax(untyped_value_index_stats)) {
		//! Has no min/max values, essentially double-checking the CanHaveNoNull from above
		return false;
	}
	auto min_value = NumericStats::GetMinUnsafe<uint32_t>(untyped_value_index_stats);
	auto max_value = NumericStats::GetMaxUnsafe<uint32_t>(untyped_value_index_stats);
	if (min_value != max_value) {
		//! Not a constant
		return false;
	}
	//! 0 is reserved for missing values (field absent from parent object)
	return min_value == 0;
}

LogicalType ToStructuredType(const LogicalType &shredding) {
	if (shredding.id() != LogicalTypeId::STRUCT) {
		// not a struct - this is a primitive type
		return shredding;
	}
	D_ASSERT(shredding.id() == LogicalTypeId::STRUCT);
	auto &child_types = StructType::GetChildTypes(shredding);
	D_ASSERT(child_types.size() <= 2);

	auto &typed_value = child_types[VariantStats::TYPED_VALUE_INDEX].second;

	if (typed_value.id() == LogicalTypeId::STRUCT) {
		auto &struct_children = StructType::GetChildTypes(typed_value);
		child_list_t<LogicalType> structured_children;
		vector<idx_t> indices(struct_children.size());
		for (idx_t i = 0; i < indices.size(); i++) {
			indices[i] = i;
		}
		std::sort(indices.begin(), indices.end(), [&](const idx_t &lhs, const idx_t &rhs) {
			auto &a = struct_children[lhs].first;
			auto &b = struct_children[rhs].first;
			return a < b;
		});
		for (auto &index : indices) {
			auto &child = struct_children[index];
			structured_children.emplace_back(child.first, ToStructuredType(child.second));
		}
		return LogicalType::STRUCT(structured_children);
	} else if (typed_value.id() == LogicalTypeId::LIST) {
		auto &child_type = ListType::GetChildType(typed_value);
		return LogicalType::LIST(ToStructuredType(child_type));
	} else {
		return typed_value;
	}
}

LogicalType VariantStats::GetShreddedStructuredType(const BaseStatistics &stats) {
	D_ASSERT(IsShredded(stats));
	return ToStructuredType(GetShreddedStats(stats).GetType());
}

LogicalType VariantStats::GetShreddingType(const LogicalType &structured_type) {
	return TypeVisitor::VisitReplace(structured_type, [](const LogicalType &type) {
		return LogicalType::STRUCT({{"typed_value", type}, {"untyped_value_index", LogicalType::UINTEGER}});
	});
}

void VariantStats::CreateShreddedStats(BaseStatistics &stats, const LogicalType &shredded_type) {
	BaseStatistics::Construct(stats.child_stats[1], shredded_type);
	auto &data = GetDataUnsafe(stats);
	data.shredding_state = VariantStatsShreddingState::SHREDDED;
}

bool VariantStats::IsShredded(const BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	return data.shredding_state == VariantStatsShreddingState::SHREDDED;
}

bool VariantStats::IsShredded(const BaseStatistics &stats, const ColumnIndex &index) {
	if (!IsShredded(stats)) {
		return false;
	}

	reference<const BaseStatistics> shredded_stats(VariantStats::GetShreddedStats(stats));
	reference<const ColumnIndex> path_iter(index);
	while (path_iter.get().HasChildren()) {
		auto &current_stats = shredded_stats.get();
		if (!VariantShreddedStats::IsFullyShredded(current_stats)) {
			return false;
		}
		auto &current = path_iter.get();
		auto &child = current.GetChildIndexes()[0];
		VariantPathComponent path_component(child.GetFieldName());
		auto child_stats = VariantShreddedStats::FindChildStats(shredded_stats.get(), path_component);
		if (!child_stats) {
			return false;
		}
		path_iter = child;
		shredded_stats = *child_stats;
	}
	if (!VariantShreddedStats::IsFullyShredded(shredded_stats.get())) {
		return false;
	}
	auto &typed_value_stats = StructStats::GetChildStats(shredded_stats.get(), VariantStats::TYPED_VALUE_INDEX);
	auto &typed_value_type = typed_value_stats.GetType();

	return !typed_value_type.IsNested();
}

BaseStatistics VariantStats::CreateShredded(const LogicalType &shredded_type) {
	BaseStatistics result(LogicalType::VARIANT());
	result.InitializeEmpty();

	CreateShreddedStats(result, shredded_type);
	result.child_stats[0].Copy(BaseStatistics::CreateEmpty(VariantShredding::GetUnshreddedType()));
	result.child_stats[1].Copy(BaseStatistics::CreateEmpty(shredded_type));
	return result;
}

const BaseStatistics &VariantStats::GetShreddedStats(const BaseStatistics &stats) {
	AssertVariant(stats);
	D_ASSERT(IsShredded(stats));
	return stats.child_stats[1];
}

BaseStatistics &VariantStats::GetShreddedStats(BaseStatistics &stats) {
	AssertVariant(stats);
	D_ASSERT(IsShredded(stats));
	return stats.child_stats[1];
}

void VariantStats::SetShreddedStats(BaseStatistics &stats, const BaseStatistics &new_stats) {
	auto &data = GetDataUnsafe(stats);
	if (!IsShredded(stats)) {
		BaseStatistics::Construct(stats.child_stats[1], new_stats.GetType());
		D_ASSERT(data.shredding_state != VariantStatsShreddingState::INCONSISTENT);
		data.shredding_state = VariantStatsShreddingState::SHREDDED;
	}
	stats.child_stats[1].Copy(new_stats);
}

void VariantStats::SetShreddedStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats) {
	AssertVariant(stats);
	D_ASSERT(new_stats);
	SetShreddedStats(stats, *new_stats);
}

//===--------------------------------------------------------------------===//
// (De)Serialization
//===--------------------------------------------------------------------===//

void VariantStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &data = GetDataUnsafe(stats);
	auto &unshredded_stats = VariantStats::GetUnshreddedStats(stats);

	serializer.WriteProperty(200, "shredding_state", data.shredding_state);

	serializer.WriteProperty(225, "unshredded_stats", unshredded_stats);
	if (IsShredded(stats)) {
		auto &shredded_stats = VariantStats::GetShreddedStats(stats);
		serializer.WriteProperty(230, "shredded_type", shredded_stats.type);
		serializer.WriteProperty(235, "shredded_stats", shredded_stats);
	}
}

void VariantStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	D_ASSERT(base.GetType().InternalType() == PhysicalType::STRUCT);
	D_ASSERT(base.GetType().id() == LogicalTypeId::VARIANT);
	auto &data = GetDataUnsafe(base);

	auto unshredded_type = VariantShredding::GetUnshreddedType();
	data.shredding_state = deserializer.ReadProperty<VariantStatsShreddingState>(200, "shredding_state");

	{
		//! Read the 'unshredded_stats' child
		deserializer.Set<const LogicalType &>(unshredded_type);
		auto stat = deserializer.ReadProperty<BaseStatistics>(225, "unshredded_stats");
		base.child_stats[0].Copy(stat);
		deserializer.Unset<LogicalType>();
	}

	if (!IsShredded(base)) {
		return;
	}
	//! Read the type of the 'shredded_stats'
	auto shredded_type = deserializer.ReadProperty<LogicalType>(230, "shredded_type");

	{
		//! Finally read the 'shredded_stats' themselves
		deserializer.Set<const LogicalType &>(shredded_type);
		auto stat = deserializer.ReadProperty<BaseStatistics>(235, "shredded_stats");
		if (base.child_stats[1].type.id() == LogicalTypeId::INVALID) {
			base.child_stats[1] = BaseStatistics::CreateUnknown(shredded_type);
		}
		base.child_stats[1].Copy(stat);
		deserializer.Unset<LogicalType>();
	}
}

static Value GetShreddedStatsStruct(const BaseStatistics &stats, bool fully_shredded) {
	if (VariantShreddedStats::IsFullyShredded(stats) != fully_shredded) {
		return Value();
	}

	auto &typed_value = VariantStats::GetTypedStats(stats);
	auto type_id = typed_value.GetType().id();
	if (type_id == LogicalTypeId::LIST) {
		// list
		auto &child_stats = ListStats::GetChildStats(typed_value);
		child_list_t<Value> result;
		auto result_stats = GetShreddedStatsStruct(child_stats, fully_shredded);
		if (result_stats.IsNull()) {
			return Value();
		}
		result.emplace_back("child_stats", std::move(result_stats));
		return Value::STRUCT(std::move(result));
	}
	if (type_id == LogicalTypeId::STRUCT) {
		// struct
		child_list_t<Value> result;
		auto &fields = StructType::GetChildTypes(typed_value.GetType());
		vector<idx_t> indices(fields.size());
		for (idx_t i = 0; i < indices.size(); i++) {
			indices[i] = i;
		}
		std::sort(indices.begin(), indices.end(), [&](const idx_t &lhs, const idx_t &rhs) {
			auto &a = fields[lhs].first;
			auto &b = fields[rhs].first;
			return std::lexicographical_compare(a.GetIdentifierName().begin(), a.GetIdentifierName().end(),
			                                    b.GetIdentifierName().begin(), b.GetIdentifierName().end());
		});
		for (idx_t i = 0; i < indices.size(); i++) {
			auto &child_stats = StructStats::GetChildStats(typed_value, indices[i]);
			auto &field = fields[indices[i]];
			auto child_stats_entry = GetShreddedStatsStruct(child_stats, fully_shredded);
			if (child_stats_entry.IsNull()) {
				continue;
			}
			result.emplace_back(field.first, std::move(child_stats_entry));
		}
		if (result.empty()) {
			return Value();
		}
		return Value::STRUCT(std::move(result));
	}
	child_list_t<Value> result;
	result.emplace_back("type", Value(typed_value.GetType().ToString()));
	result.emplace_back("stats", typed_value.ToStruct());
	return Value::STRUCT(std::move(result));
}

child_list_t<Value> VariantStats::ToStruct(const BaseStatistics &stats) {
	child_list_t<Value> result;
	bool is_shredded = IsShredded(stats);
	auto &data = GetDataUnsafe(stats);
	result.emplace_back("shredding_state", Value(EnumUtil::ToString(data.shredding_state)));
	if (is_shredded) {
		auto fully_shredded_stats = GetShreddedStatsStruct(stats.child_stats[1], true);
		if (!fully_shredded_stats.IsNull()) {
			result.emplace_back("fully_shredded", std::move(fully_shredded_stats));
		}
		auto partially_shredded_stats = GetShreddedStatsStruct(stats.child_stats[1], false);
		if (!partially_shredded_stats.IsNull()) {
			result.emplace_back("partially_shredded", std::move(partially_shredded_stats));
		}
	}
	return result;
}

static BaseStatistics WrapTypedValue(const BaseStatistics &typed_value,
                                     optional_ptr<BaseStatistics> untyped_value_index) {
	if (!untyped_value_index && !typed_value.GetType().IsNested()) {
		// no untyped value and not a nested type - directly emit the typed stats
		return typed_value.Copy();
	}
	child_list_t<LogicalType> stats_type;
	stats_type.emplace_back(make_pair("typed_value", typed_value.GetType()));
	if (untyped_value_index) {
		stats_type.emplace_back(make_pair("untyped_value_index", untyped_value_index->GetType()));
	}
	BaseStatistics shredded = BaseStatistics::CreateEmpty(LogicalType::STRUCT(std::move(stats_type)));

	StructStats::GetChildStats(shredded, VariantStats::TYPED_VALUE_INDEX).Copy(typed_value);
	if (untyped_value_index) {
		StructStats::GetChildStats(shredded, VariantStats::UNTYPED_VALUE_INDEX).Copy(*untyped_value_index);
	}
	return shredded;
}

//! Recursively build the shredding representation stats for a value of `type` with statistics `input`.
//! Returns nullptr when the type can not be represented as a single consistent shredding.
static unique_ptr<BaseStatistics> TryBuildShreddingStats(const LogicalType &type, const BaseStatistics &input) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		auto &fields = StructType::GetChildTypes(type);
		if (fields.empty()) {
			// an empty object has no shredded fields to push down
			return nullptr;
		}
		child_list_t<LogicalType> typed_children;
		vector<unique_ptr<BaseStatistics>> child_stats;
		child_stats.reserve(fields.size());
		for (idx_t i = 0; i < fields.size(); i++) {
			auto child_result = TryBuildShreddingStats(fields[i].second, StructStats::GetChildStats(input, i));
			if (!child_result) {
				return nullptr;
			}
			typed_children.emplace_back(fields[i].first, child_result->GetType());
			child_stats.push_back(std::move(child_result));
		}
		auto typed_value = BaseStatistics::CreateEmpty(LogicalType::STRUCT(std::move(typed_children)));
		for (idx_t i = 0; i < child_stats.size(); i++) {
			StructStats::SetChildStats(typed_value, i, *child_stats[i]);
		}
		typed_value.CopyValidity(input);
		return WrapTypedValue(typed_value, nullptr).ToUnique();
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		const bool is_list = type.id() == LogicalTypeId::LIST;
		auto &child_type = is_list ? ListType::GetChildType(type) : ArrayType::GetChildType(type);
		auto &input_child = is_list ? ListStats::GetChildStats(input) : ArrayStats::GetChildStats(input);
		auto child_result = TryBuildShreddingStats(child_type, input_child);
		if (!child_result) {
			return nullptr;
		}
		// a variant stores both LISTs and fixed-size ARRAYs as a (variable length) array
		auto typed_value = BaseStatistics::CreateEmpty(LogicalType::LIST(child_result->GetType()));
		ListStats::SetChildStats(typed_value, std::move(child_result));
		typed_value.CopyValidity(input);
		return WrapTypedValue(typed_value, nullptr).ToUnique();
	}
	default:
		if (type.IsNested() || type.id() == LogicalTypeId::ENUM || type.IsJSONType()) {
			// MAP / UNION / ENUM etc. are not stored in their source representation in the variant
			return nullptr;
		}
		return input.ToUnique();
	}
}

unique_ptr<BaseStatistics> VariantStats::StatisticsPropagateToVariant(const LogicalType &source_type,
                                                                      const BaseStatistics &child_stats) {
	if (source_type.id() == LogicalTypeId::VARIANT) {
		return nullptr;
	}
	auto shredding = TryBuildShreddingStats(source_type, child_stats);
	if (!shredding) {
		return nullptr;
	}
	auto result = VariantStats::CreateShredded(shredding->GetType());
	VariantStats::SetShreddedStats(result, *shredding);
	// the cast preserves NULLs exactly, so the top-level variant validity matches the input validity
	result.CopyBase(child_stats);
	return result.ToUnique();
}

unique_ptr<BaseStatistics> VariantStats::WrapExtractedFieldAsVariant(const BaseStatistics &base_variant,
                                                                     const BaseStatistics &extracted_field) {
	D_ASSERT(base_variant.type.id() == LogicalTypeId::VARIANT);
	AssertShreddedStats(extracted_field);

	BaseStatistics copy = BaseStatistics::CreateUnknown(base_variant.GetType());
	copy.Copy(base_variant);
	copy.child_stats[1] = BaseStatistics::CreateUnknown(extracted_field.GetType());
	copy.child_stats[1].Copy(extracted_field);
	// The extraction can produce NULL when the field's typed value is NULL, even if the input
	// variant is not - descend through the shredding STRUCTs to the typed value.
	const BaseStatistics *effective = &extracted_field;
	bool can_have_null = false;
	while (true) {
		can_have_null |= effective->CanHaveNull();
		if (effective->GetType().id() != LogicalTypeId::STRUCT) {
			break;
		}
		effective = &VariantStats::GetTypedStats(*effective);
	}
	if (can_have_null) {
		copy.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	return copy.ToUnique();
}

bool VariantStats::MergeShredding(const BaseStatistics &stats, const BaseStatistics &other, BaseStatistics &new_stats) {
	//! shredded_type:
	//! <shredding>
	//! STRUCT(typed_value <shredding>)
	//! STRUCT(typed_value <shredding>, untyped_value_index UINTEGER)

	//! shredding, 1 of:
	//! - <primitive type>
	//! - <shredded_type>
	//! - <shredded_type>[]

	auto &stats_typed_value = GetTypedStats(stats);
	auto &other_typed_value = GetTypedStats(other);

	auto &stats_typed_value_type = stats_typed_value.GetType();
	auto &other_typed_value_type = other_typed_value.GetType();

	auto untyped_value_stats = GetUntypedStats(stats);
	auto other_untyped_stats = GetUntypedStats(other);

	// handle untyped value stats
	optional_ptr<BaseStatistics> new_untyped_value_stats;
	BaseStatistics owned_untyped_value_stats;

	if (untyped_value_stats && other_untyped_stats) {
		// both entries have untyped value stats - merge them
		owned_untyped_value_stats = untyped_value_stats->Copy();
		owned_untyped_value_stats.Merge(*other_untyped_stats);
		new_untyped_value_stats = owned_untyped_value_stats;
	} else if (untyped_value_stats) {
		// only LHS has untyped value stats
		owned_untyped_value_stats = untyped_value_stats->Copy();
		new_untyped_value_stats = owned_untyped_value_stats;
	} else if (other_untyped_stats) {
		// only RHS has untyped value stats
		owned_untyped_value_stats = other_untyped_stats->Copy();
		new_untyped_value_stats = owned_untyped_value_stats;
	}

	if (stats_typed_value_type.id() == LogicalTypeId::STRUCT) {
		if (stats_typed_value_type.id() != other_typed_value_type.id()) {
			//! other is not an OBJECT, can't merge
			return false;
		}
		auto &stats_object_children = StructType::GetChildTypes(stats_typed_value_type);
		auto &other_object_children = StructType::GetChildTypes(other_typed_value_type);

		//! Map field name to index, for 'other'
		unordered_map<string, idx_t> key_to_index;
		for (idx_t i = 0; i < other_object_children.size(); i++) {
			auto &other_object_child = other_object_children[i];
			key_to_index.emplace(other_object_child.first.GetIdentifierName(), i);
		}

		//! Attempt to merge all overlapping fields, only keep the fields that were able to be merged
		child_list_t<LogicalType> new_children;
		vector<BaseStatistics> new_child_stats;

		for (idx_t i = 0; i < stats_object_children.size(); i++) {
			auto &stats_object_child = stats_object_children[i];
			auto other_it = key_to_index.find(stats_object_child.first.GetIdentifierName());
			if (other_it == key_to_index.end()) {
				continue;
			}
			auto &other_object_child = other_object_children[other_it->second];
			if (other_object_child.second.id() != stats_object_child.second.id()) {
				//! TODO: perhaps we can keep the field but demote the type to unshredded somehow?
				//! Or even use MaxLogicalType and merge the stats into that ?
				continue;
			}

			auto &stats_child = StructStats::GetChildStats(stats_typed_value, i);
			auto &other_child = StructStats::GetChildStats(other_typed_value, other_it->second);
			BaseStatistics new_child;
			if (!MergeShredding(stats_child, other_child, new_child)) {
				continue;
			}
			new_children.emplace_back(stats_object_child.first, new_child.GetType());
			new_child_stats.emplace_back(std::move(new_child));
		}
		if (new_children.empty()) {
			//! No fields remaining, demote to unshredded
			return false;
		}

		//! Create new stats out of the remaining fields
		auto new_object_type = LogicalType::STRUCT(std::move(new_children));
		auto new_typed_value = BaseStatistics::CreateEmpty(new_object_type);
		for (idx_t i = 0; i < new_child_stats.size(); i++) {
			StructStats::SetChildStats(new_typed_value, i, new_child_stats[i]);
		}
		new_typed_value.CombineValidity(stats_typed_value, other_typed_value);
		new_stats = WrapTypedValue(new_typed_value, new_untyped_value_stats);
		return true;
	} else if (stats_typed_value_type.id() == LogicalTypeId::LIST) {
		if (stats_typed_value_type.id() != other_typed_value_type.id()) {
			//! other is not an ARRAY, can't merge
			return false;
		}
		auto &stats_child = ListStats::GetChildStats(stats_typed_value);
		auto &other_child = ListStats::GetChildStats(other_typed_value);

		//! TODO: perhaps we can keep the LIST part of the stats, and only demote the child to unshredded?
		BaseStatistics new_child_stats;
		if (!MergeShredding(stats_child, other_child, new_child_stats)) {
			return false;
		}
		auto new_typed_value = BaseStatistics::CreateEmpty(LogicalType::LIST(new_child_stats.type));
		new_typed_value.CombineValidity(stats_typed_value, other_typed_value);
		ListStats::SetChildStats(new_typed_value, new_child_stats.ToUnique());
		new_stats = WrapTypedValue(new_typed_value, new_untyped_value_stats);
		return true;
	} else {
		D_ASSERT(!stats_typed_value_type.IsNested());
		if (stats_typed_value_type != other_typed_value_type) {
			//! other is not the same type (e.g. a DECIMAL of a different width or scale), can't merge
			return false;
		}
		auto new_typed_stats = stats_typed_value.Copy();
		new_typed_stats.Merge(other_typed_value);
		new_stats = WrapTypedValue(new_typed_stats, new_untyped_value_stats);
		return true;
	}
}

void VariantStats::Merge(BaseStatistics &stats, const BaseStatistics &other) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}

	stats.child_stats[0].Merge(other.child_stats[0]);
	auto &data = GetDataUnsafe(stats);
	auto &other_data = GetDataUnsafe(other);

	const auto other_shredding_state = other_data.shredding_state;
	const auto shredding_state = data.shredding_state;

	if (other_shredding_state == VariantStatsShreddingState::UNINITIALIZED) {
		//! No need to merge
		return;
	}

	switch (shredding_state) {
	case VariantStatsShreddingState::INCONSISTENT: {
		//! INCONSISTENT + ANY -> INCONSISTENT
		return;
	}
	case VariantStatsShreddingState::UNINITIALIZED: {
		switch (other_shredding_state) {
		case VariantStatsShreddingState::SHREDDED:
			stats.child_stats[1] = BaseStatistics::CreateUnknown(other.child_stats[1].GetType());
			stats.child_stats[1].Copy(other.child_stats[1]);
			break;
		default:
			break;
		}
		//! UNINITIALIZED + ANY -> ANY
		data.shredding_state = other_shredding_state;
		break;
	}
	case VariantStatsShreddingState::NOT_SHREDDED: {
		if (other_shredding_state == VariantStatsShreddingState::NOT_SHREDDED) {
			return;
		}
		//! NOT_SHREDDED + !NOT_SHREDDED -> INCONSISTENT
		data.shredding_state = VariantStatsShreddingState::INCONSISTENT;
		stats.child_stats[1].type = LogicalType::INVALID;
		break;
	}
	case VariantStatsShreddingState::SHREDDED: {
		switch (other_shredding_state) {
		case VariantStatsShreddingState::SHREDDED: {
			BaseStatistics merged_shredding_stats;
			if (!MergeShredding(stats.child_stats[1], other.child_stats[1], merged_shredding_stats)) {
				//! SHREDDED(T1) + SHREDDED(T2) -> INCONSISTENT
				data.shredding_state = VariantStatsShreddingState::INCONSISTENT;
				stats.child_stats[1].type = LogicalType::INVALID;
			} else {
				//! SHREDDED(T1) + SHREDDED(T1) -> SHREDDED
				stats.child_stats[1] = BaseStatistics::CreateUnknown(merged_shredding_stats.GetType());
				stats.child_stats[1].Copy(merged_shredding_stats);
			}
			break;
		}
		default:
			//! SHREDDED + !SHREDDED -> INCONSISTENT
			data.shredding_state = VariantStatsShreddingState::INCONSISTENT;
			stats.child_stats[1].type = LogicalType::INVALID;
			break;
		}
		break;
	}
	}
}

void VariantStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	auto &other_data = VariantStats::GetDataUnsafe(other);
	auto &data = VariantStats::GetDataUnsafe(stats);
	(void)other_data;
	(void)data;

	//! This is ensured by the CopyBase method of BaseStatistics
	D_ASSERT(data.shredding_state == other_data.shredding_state);
	stats.child_stats[0].Copy(other.child_stats[0]);
	if (IsShredded(other)) {
		stats.child_stats[1] = BaseStatistics::CreateUnknown(other.child_stats[1].GetType());
		stats.child_stats[1].Copy(other.child_stats[1]);
	} else {
		stats.child_stats[1].type = LogicalType::INVALID;
	}
}

void VariantStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	// TODO: Verify stats
}

const VariantStatsData &VariantStats::GetDataUnsafe(const BaseStatistics &stats) {
	AssertVariant(stats);
	return stats.stats_union.variant_data;
}

VariantStatsData &VariantStats::GetDataUnsafe(BaseStatistics &stats) {
	AssertVariant(stats);
	return stats.stats_union.variant_data;
}

static bool CanUseShreddedStats(optional_ptr<const BaseStatistics> shredded_stats) {
	return shredded_stats && VariantShreddedStats::IsFullyShredded(*shredded_stats);
}

unique_ptr<BaseStatistics> VariantStats::PushdownExtract(const BaseStatistics &stats, const StorageIndex &index) {
	if (!VariantStats::IsShredded(stats)) {
		//! Not shredded at all, no stats available
		return nullptr;
	}

	optional_ptr<const BaseStatistics> res(VariantStats::GetShreddedStats(stats));
	if (!CanUseShreddedStats(res)) {
		//! Not fully shredded, can't say anything meaningful about the stats
		return nullptr;
	}

	reference<const StorageIndex> index_iter(index);
	while (true) {
		auto &current = index_iter.get();
		D_ASSERT(!current.HasPrimaryIndex());
		auto &field_name = current.GetFieldName();
		VariantPathComponent path(field_name);
		res = VariantShreddedStats::FindChildStats(*res, path);
		if (!CanUseShreddedStats(res)) {
			//! Not fully shredded, can't say anything meaningful about the stats
			return nullptr;
		}
		if (!index_iter.get().HasChildren()) {
			break;
		}
		index_iter = index_iter.get().GetChildIndex(0);
	}
	auto &shredded_child_stats = *res;

	auto &typed_value_stats = GetTypedStats(shredded_child_stats);
	auto &last_index = index_iter.get();
	auto &child_type = typed_value_stats.type;
	if (!last_index.HasType() || last_index.GetType().id() == LogicalTypeId::VARIANT) {
		//! Return the variant stats, not the 'typed_value' (non-variant) stats, since there's no cast pushed down
		return WrapExtractedFieldAsVariant(stats, shredded_child_stats);
	}
	if (!VariantShreddedStats::IsFullyShredded(shredded_child_stats)) {
		//! Not all data is shredded, so there are values in the column that are not of the shredded type
		return nullptr;
	}

	auto &cast_type = last_index.GetType();
	if (child_type != cast_type) {
		//! FIXME: support try_cast
		return CastStatistics::TryPropagate(typed_value_stats, child_type, cast_type);
	}
	auto result = typed_value_stats.ToUnique();
	return result;
}

} // namespace duckdb
