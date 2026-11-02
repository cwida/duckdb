#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/logical_type_info/enum_type_info.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Extension Type Info
//===--------------------------------------------------------------------===//

bool ExtensionTypeInfo::Equals(optional_ptr<ExtensionTypeInfo> lhs, optional_ptr<ExtensionTypeInfo> rhs) {
	// Either both are null, or both are the same, so they are equal
	if (lhs.get() == rhs.get()) {
		return true;
	}
	// If one is null, then we cant compare them
	if (lhs == nullptr || rhs == nullptr) {
		return true;
	}

	// Both are not null, so we can compare them
	D_ASSERT(lhs != nullptr && rhs != nullptr);

	// Compare modifiers
	const auto &lhs_mods = lhs->modifiers;
	const auto &rhs_mods = rhs->modifiers;
	const auto common_mods = MinValue(lhs_mods.size(), rhs_mods.size());
	for (idx_t i = 0; i < common_mods; i++) {
		// If the types are not strictly equal, they are not equal
		auto &lhs_val = lhs_mods[i].value;
		auto &rhs_val = rhs_mods[i].value;

		if (lhs_val.type() != rhs_val.type()) {
			return false;
		}

		// If both are null, its fine
		if (lhs_val.IsNull() && rhs_val.IsNull()) {
			continue;
		}

		// If one is null, the other must be null too
		if (lhs_val.IsNull() != rhs_val.IsNull()) {
			return false;
		}

		if (lhs_val != rhs_val) {
			return false;
		}
	}

	// Properties are optional, so only compare those present in both
	const auto &lhs_props = lhs->properties;
	const auto &rhs_props = rhs->properties;

	for (const auto &kv : lhs_props) {
		auto it = rhs_props.find(kv.first);
		if (it == rhs_props.end()) {
			// Continue
			continue;
		}
		if (kv.second != it->second) {
			// Mismatch!
			return false;
		}
	}

	// All ok!
	return true;
}

//===--------------------------------------------------------------------===//
// Extra Type Info
//===--------------------------------------------------------------------===//
LogicalTypeInfo::LogicalTypeInfo(LogicalTypeInfoType type) noexcept : type(type) {
}
LogicalTypeInfo::LogicalTypeInfo(LogicalTypeInfoType type, string alias) : type(type), alias(std::move(alias)) {
}
LogicalTypeInfo::~LogicalTypeInfo() {
}

LogicalTypeInfo::LogicalTypeInfo(const LogicalTypeInfo &other) : type(other.type), alias(other.alias) {
	if (other.extension_info) {
		extension_info = make_uniq<ExtensionTypeInfo>(*other.extension_info);
	}
}

unique_ptr<LogicalTypeInfo> LogicalTypeInfo::Copy() const {
	return unique_ptr<LogicalTypeInfo>(new LogicalTypeInfo(*this));
}

void LogicalTypeInfo::CopyBaseInfo(LogicalTypeInfo &target) const {
	target.alias = alias;
	if (extension_info) {
		target.extension_info = make_uniq<ExtensionTypeInfo>(*extension_info);
	}
}

//! Infos that carry no parameters that affect equality - collations do not affect equality
static bool HasNoEqualityParameters(LogicalTypeInfoType type) {
	return type == LogicalTypeInfoType::INVALID_TYPE_INFO || type == LogicalTypeInfoType::STRING_TYPE_INFO ||
	       type == LogicalTypeInfoType::GENERIC_TYPE_INFO;
}

bool LogicalTypeInfo::Equals(const LogicalTypeInfo &other) const {
	if (alias != other.alias) {
		return false;
	}
	if (!ExtensionTypeInfo::Equals(extension_info, other.extension_info)) {
		return false;
	}
	if (HasNoEqualityParameters(type) && HasNoEqualityParameters(other.type)) {
		return true;
	}
	if (type != other.type) {
		return false;
	}
	return EqualsInternal(&other);
}

bool LogicalTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	// Do nothing
	return true;
}

//===--------------------------------------------------------------------===//
// Decimal Type Info
//===--------------------------------------------------------------------===//
DecimalTypeInfo::DecimalTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::DECIMAL_TYPE_INFO) {
}

DecimalTypeInfo::DecimalTypeInfo(uint8_t width_p, uint8_t scale_p)
    : LogicalTypeInfo(LogicalTypeInfoType::DECIMAL_TYPE_INFO), width(width_p), scale(scale_p) {
	D_ASSERT(width_p >= scale_p);
}

bool DecimalTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<DecimalTypeInfo>();
	return width == other.width && scale == other.scale;
}

unique_ptr<LogicalTypeInfo> DecimalTypeInfo::Copy() const {
	return make_uniq<DecimalTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// String Type Info
//===--------------------------------------------------------------------===//
StringTypeInfo::StringTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::STRING_TYPE_INFO) {
}

StringTypeInfo::StringTypeInfo(string collation_p)
    : LogicalTypeInfo(LogicalTypeInfoType::STRING_TYPE_INFO), collation(std::move(collation_p)) {
}

bool StringTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	// collation info has no impact on equality
	return true;
}

unique_ptr<LogicalTypeInfo> StringTypeInfo::Copy() const {
	return make_uniq<StringTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// List Type Info
//===--------------------------------------------------------------------===//
ListTypeInfo::ListTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::LIST_TYPE_INFO) {
}

ListTypeInfo::ListTypeInfo(LogicalType child_type_p)
    : LogicalTypeInfo(LogicalTypeInfoType::LIST_TYPE_INFO), child_type(std::move(child_type_p)) {
}

bool ListTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<ListTypeInfo>();
	return child_type == other.child_type;
}

unique_ptr<LogicalTypeInfo> ListTypeInfo::Copy() const {
	return make_uniq<ListTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Struct Type Info
//===--------------------------------------------------------------------===//
StructTypeInfo::StructTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::STRUCT_TYPE_INFO) {
}

StructTypeInfo::StructTypeInfo(child_list_t<LogicalType> child_types_p)
    : LogicalTypeInfo(LogicalTypeInfoType::STRUCT_TYPE_INFO), child_types(std::move(child_types_p)) {
}

bool StructTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<StructTypeInfo>();
	return child_types == other.child_types;
}

unique_ptr<LogicalTypeInfo> StructTypeInfo::Copy() const {
	return make_uniq<StructTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// User Type Info
//===--------------------------------------------------------------------===//
void UnboundTypeInfo::Serialize(Serializer &serializer) const {
	LogicalTypeInfo::Serialize(serializer);

	if (serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
		serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(204, "expr", expr);
		return;
	}

	// Try to write this as an old "USER" type, if possible
	if (expr->GetExpressionType() != ExpressionType::TYPE) {
		throw SerializationException(
		    "Cannot serialize non-type type expression when targeting database storage version '%s'",
		    serializer.GetOptions().storage_compatibility.duckdb_version);
	}

	auto &type_expr = expr->Cast<TypeExpression>();
	serializer.WritePropertyWithDefault<string>(200, "name", type_expr.GetTypeName().GetIdentifierName());
	serializer.WritePropertyWithDefault<string>(201, "catalog", type_expr.GetCatalog().GetIdentifierName());
	serializer.WritePropertyWithDefault<string>(202, "schema", type_expr.GetSchema().GetIdentifierName());

	// Try to write the user type mods too
	vector<Value> user_type_mods;
	for (auto &param : type_expr.GetChildren()) {
		if (param->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			throw SerializationException(
			    "Cannot serialize non-constant type parameter when targeting serialization version %s",
			    serializer.GetOptions().storage_compatibility.duckdb_version);
		}

		auto &const_expr = param->Cast<ConstantExpression>();
		user_type_mods.push_back(const_expr.GetLiteral().ToValue());
	}

	serializer.WritePropertyWithDefault<vector<Value>>(203, "user_type_modifiers", user_type_mods);
}

unique_ptr<LogicalTypeInfo> UnboundTypeInfo::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<UnboundTypeInfo>(new UnboundTypeInfo());

	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(204, "expr", result->expr);

	if (!result->expr) {
		// This is a legacy "USER" type
		string name;
		deserializer.ReadPropertyWithDefault<string>(200, "name", name);
		string catalog;
		deserializer.ReadPropertyWithDefault<string>(201, "catalog", catalog);
		string schema;
		deserializer.ReadPropertyWithDefault<string>(202, "schema", schema);

		vector<unique_ptr<ParsedExpression>> user_type_mods;
		auto mods = deserializer.ReadPropertyWithDefault<vector<Value>>(203, "user_type_modifiers");
		for (auto &mod : mods) {
			user_type_mods.push_back(ConstantExpression::FromValue(mod));
		}

		result->expr = make_uniq<TypeExpression>(
		    QualifiedName(Identifier(catalog), Identifier(schema), Identifier(name)), std::move(user_type_mods));
	}

	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Legacy Aggregate State Type Info
//===--------------------------------------------------------------------===//
LegacyAggregateStateTypeInfo::LegacyAggregateStateTypeInfo()
    : LogicalTypeInfo(LogicalTypeInfoType::LEGACY_AGGREGATE_STATE_TYPE_INFO) {
	throw InternalException("LegacyAggregateStateTypeInfo should no longer be getting constructed");
}

bool LegacyAggregateStateTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	throw InternalException("LegacyAggregateStateTypeInfo should no longer be getting constructed");
}

unique_ptr<LogicalTypeInfo> LegacyAggregateStateTypeInfo::LegacyDeserialize() {
	return make_uniq<LogicalTypeInfo>(LogicalTypeInfoType::GENERIC_TYPE_INFO);
}

//===--------------------------------------------------------------------===//
// Enum Type Info
//===--------------------------------------------------------------------===//
PhysicalType EnumTypeInfo::DictType(idx_t size) {
	if (size <= NumericLimits<uint8_t>::Maximum()) {
		return PhysicalType::UINT8;
	} else if (size <= NumericLimits<uint16_t>::Maximum()) {
		return PhysicalType::UINT16;
	} else if (size <= NumericLimits<uint32_t>::Maximum()) {
		return PhysicalType::UINT32;
	} else {
		throw InternalException("Enum size must be lower than " + std::to_string(NumericLimits<uint32_t>::Maximum()));
	}
}

void EnumTypeInfo::ThrowDuplicateValue(const string_t &value) {
	throw InvalidInputException("Attempted to create ENUM type with duplicate value %s", value.GetString());
}

void EnumTypeInfo::ThrowCorruptedIndex(idx_t index, idx_t size) {
	throw SerializationException("Corrupted enum: string value index %llu exceeds enum size %u", index,
	                             UnsafeNumericCast<uint32_t>(size));
}

void EnumTypeInfo::ThrowCorruptedCount(idx_t size, idx_t read_count) {
	throw SerializationException("Corrupted enum: expected %u values, but found %llu",
	                             UnsafeNumericCast<uint32_t>(size), read_count);
}

EnumTypeInfo::EnumTypeInfo(const Vector &values_insert_order_p, idx_t dict_size_p)
    : LogicalTypeInfo(LogicalTypeInfoType::ENUM_TYPE_INFO), values_insert_order(Vector::Ref(values_insert_order_p)),
      dict_type(EnumDictType::VECTOR_DICT), dict_size(dict_size_p) {
}

const EnumDictType &EnumTypeInfo::GetEnumDictType() const {
	return dict_type;
}

const Vector &EnumTypeInfo::GetValuesInsertOrder() const {
	return values_insert_order;
}

const idx_t &EnumTypeInfo::GetDictSize() const {
	return dict_size;
}

unique_ptr<LogicalTypeInfo> EnumTypeInfo::CreateTypeInfo(const Vector &ordered_data, idx_t size) {
	auto enum_internal_type = EnumTypeInfo::DictType(size);
	switch (enum_internal_type) {
	case PhysicalType::UINT8:
		return make_uniq<EnumTypeInfoTemplated<uint8_t>>(ordered_data, size);
	case PhysicalType::UINT16:
		return make_uniq<EnumTypeInfoTemplated<uint16_t>>(ordered_data, size);
	case PhysicalType::UINT32:
		return make_uniq<EnumTypeInfoTemplated<uint32_t>>(ordered_data, size);
	default:
		throw InternalException("Invalid Physical Type for ENUMs");
	}
}

LogicalType EnumTypeInfo::CreateType(const Vector &ordered_data, idx_t size) {
	return LogicalType(LogicalTypeId::ENUM, CreateTypeInfo(ordered_data, size));
}

template <class T>
int64_t TemplatedGetPos(const string_map_t<T> &map, const string_t &key) {
	auto it = map.find(key);
	if (it == map.end()) {
		return -1;
	}
	return it->second;
}

int64_t EnumType::GetPos(const LogicalType &type, const string_t &key) {
	auto &info = type.GetTypeInfo();
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		return TemplatedGetPos(info.Cast<EnumTypeInfoTemplated<uint8_t>>().GetValues(), key);
	case PhysicalType::UINT16:
		return TemplatedGetPos(info.Cast<EnumTypeInfoTemplated<uint16_t>>().GetValues(), key);
	case PhysicalType::UINT32:
		return TemplatedGetPos(info.Cast<EnumTypeInfoTemplated<uint32_t>>().GetValues(), key);
	default:
		throw InternalException("ENUM can only have unsigned integers (except UINT64) as physical types");
	}
}

string_t EnumType::GetString(const LogicalType &type, idx_t pos) {
	D_ASSERT(pos < EnumType::GetSize(type));
	return FlatVector::GetData<string_t>(EnumType::GetValuesInsertOrder(type))[pos];
}

unique_ptr<LogicalTypeInfo> EnumTypeInfo::Deserialize(Deserializer &deserializer) {
	auto values_count = deserializer.ReadProperty<idx_t>(200, "values_count");
	if (values_count > NumericLimits<uint32_t>::Maximum()) {
		throw SerializationException("Corrupted enum: enum size %llu exceeds maximum %llu", values_count,
		                             static_cast<idx_t>(NumericLimits<uint32_t>::Maximum()));
	}
	auto enum_internal_type = EnumTypeInfo::DictType(values_count);
	switch (enum_internal_type) {
	case PhysicalType::UINT8:
		return EnumTypeInfoTemplated<uint8_t>::Deserialize(deserializer, NumericCast<uint32_t>(values_count));
	case PhysicalType::UINT16:
		return EnumTypeInfoTemplated<uint16_t>::Deserialize(deserializer, NumericCast<uint32_t>(values_count));
	case PhysicalType::UINT32:
		return EnumTypeInfoTemplated<uint32_t>::Deserialize(deserializer, NumericCast<uint32_t>(values_count));
	default:
		throw InternalException("Invalid Physical Type for ENUMs");
	}
}

// Equalities are only used in enums with different catalog entries
bool EnumTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<EnumTypeInfo>();
	if (dict_type != other.dict_type) {
		return false;
	}
	D_ASSERT(dict_type == EnumDictType::VECTOR_DICT);
	// We must check if both enums have the same size
	if (other.dict_size != dict_size) {
		return false;
	}
	auto other_vector_ptr = FlatVector::GetData<string_t>(other.values_insert_order);
	auto this_vector_ptr = FlatVector::GetData<string_t>(values_insert_order);

	// Now we must check if all strings are the same
	for (idx_t i = 0; i < dict_size; i++) {
		if (!Equals::Operation(other_vector_ptr[i], this_vector_ptr[i])) {
			return false;
		}
	}
	return true;
}

void EnumTypeInfo::Serialize(Serializer &serializer) const {
	LogicalTypeInfo::Serialize(serializer);

	// Enums are special in that we serialize their values as a list instead of dumping the whole vector
	auto strings = FlatVector::GetData<string_t>(values_insert_order);
	serializer.WriteProperty(200, "values_count", dict_size);
	serializer.WriteList(201, "values", dict_size,
	                     [&](Serializer::List &list, idx_t i) { list.WriteElement(strings[i]); });
}

unique_ptr<LogicalTypeInfo> EnumTypeInfo::Copy() const {
	// create a templated copy so that the value lookup map is rebuilt - the dictionary itself is shared
	auto result = CreateTypeInfo(values_insert_order, dict_size);
	CopyBaseInfo(*result);
	return result;
}

//===--------------------------------------------------------------------===//
// ArrayTypeInfo
//===--------------------------------------------------------------------===//

ArrayTypeInfo::ArrayTypeInfo(LogicalType child_type_p, uint32_t size_p)
    : LogicalTypeInfo(LogicalTypeInfoType::ARRAY_TYPE_INFO), child_type(std::move(child_type_p)), size(size_p) {
}

bool ArrayTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<ArrayTypeInfo>();
	return child_type == other.child_type && size == other.size;
}

unique_ptr<LogicalTypeInfo> ArrayTypeInfo::Copy() const {
	return make_uniq<ArrayTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Any Type Info
//===--------------------------------------------------------------------===//
AnyTypeInfo::AnyTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::ANY_TYPE_INFO) {
}

AnyTypeInfo::AnyTypeInfo(LogicalType target_type_p, idx_t cast_score_p)
    : LogicalTypeInfo(LogicalTypeInfoType::ANY_TYPE_INFO), target_type(std::move(target_type_p)),
      cast_score(cast_score_p) {
}

bool AnyTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<AnyTypeInfo>();
	return target_type == other.target_type && cast_score == other.cast_score;
}

unique_ptr<LogicalTypeInfo> AnyTypeInfo::Copy() const {
	return make_uniq<AnyTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Integer Literal Type Info
//===--------------------------------------------------------------------===//
IntegerLiteralTypeInfo::IntegerLiteralTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::INTEGER_LITERAL_TYPE_INFO) {
}

IntegerLiteralTypeInfo::IntegerLiteralTypeInfo(Value constant_value_p)
    : LogicalTypeInfo(LogicalTypeInfoType::INTEGER_LITERAL_TYPE_INFO), constant_value(std::move(constant_value_p)) {
	if (constant_value.IsNull()) {
		throw InternalException("Integer literal cannot be NULL");
	}
}

bool IntegerLiteralTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<IntegerLiteralTypeInfo>();
	return constant_value == other.constant_value;
}

unique_ptr<LogicalTypeInfo> IntegerLiteralTypeInfo::Copy() const {
	return make_uniq<IntegerLiteralTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Template Type Info
//===--------------------------------------------------------------------===//
TemplateTypeInfo::TemplateTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::TEMPLATE_TYPE_INFO) {
}

TemplateTypeInfo::TemplateTypeInfo(string name_p)
    : LogicalTypeInfo(LogicalTypeInfoType::TEMPLATE_TYPE_INFO), name(std::move(name_p)) {
}

bool TemplateTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<TemplateTypeInfo>();
	return name == other.name;
}

unique_ptr<LogicalTypeInfo> TemplateTypeInfo::Copy() const {
	return make_uniq<TemplateTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Geo Type Info
//===--------------------------------------------------------------------===//
GeoTypeInfo::GeoTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::GEO_TYPE_INFO) {
}

bool GeoTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	// No additional info to compare
	const auto &other = other_p->Cast<GeoTypeInfo>();
	return other.crs.Equals(crs);
}

unique_ptr<LogicalTypeInfo> GeoTypeInfo::Copy() const {
	return make_uniq<GeoTypeInfo>(*this);
}

//===--------------------------------------------------------------------===//
// Unbound Type Info
//===--------------------------------------------------------------------===//
UnboundTypeInfo::UnboundTypeInfo() : LogicalTypeInfo(LogicalTypeInfoType::UNBOUND_TYPE_INFO) {
}

UnboundTypeInfo::UnboundTypeInfo(unique_ptr<ParsedExpression> expr_p)
    : LogicalTypeInfo(LogicalTypeInfoType::UNBOUND_TYPE_INFO), expr(std::move(expr_p)) {
}

bool UnboundTypeInfo::EqualsInternal(const LogicalTypeInfo *other_p) const {
	auto &other = other_p->Cast<UnboundTypeInfo>();
	if (!expr->Equals(*other.expr)) {
		return false;
	}
	return true;
}

unique_ptr<LogicalTypeInfo> UnboundTypeInfo::Copy() const {
	auto result = make_uniq<UnboundTypeInfo>(expr->Copy());
	CopyBaseInfo(*result);
	return std::move(result);
}

} // namespace duckdb
