#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

struct CCustomType {
	unique_ptr<LogicalType> base_type;
	string name;
};

} // namespace duckdb

static bool AssertLogicalTypeId(duckdb_logical_type type, duckdb::LogicalTypeId type_id) {
	if (!type) {
		return false;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (logical_type.id() != type_id) {
		return false;
	}
	return true;
}

static bool AssertInternalType(duckdb_logical_type type, duckdb::PhysicalType physical_type) {
	if (!type) {
		return false;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (logical_type.InternalType() != physical_type) {
		return false;
	}
	return true;
}

duckdb_logical_type duckdb_create_logical_type(duckdb_type type) {
	if (type == DUCKDB_TYPE_DECIMAL || type == DUCKDB_TYPE_ENUM || type == DUCKDB_TYPE_LIST ||
	    type == DUCKDB_TYPE_STRUCT || type == DUCKDB_TYPE_MAP || type == DUCKDB_TYPE_ARRAY ||
	    type == DUCKDB_TYPE_UNION) {
		type = DUCKDB_TYPE_INVALID;
	}

	if (type == DUCKDB_TYPE_VARIANT) {
		// a VARIANT carries the types of its children
		return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(duckdb::LogicalType::VARIANT()));
	}
	auto type_id = duckdb::LogicalTypeIdFromC(type);
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(type_id));
}

duckdb_logical_type duckdb_create_list_type(duckdb_logical_type type) {
	if (!type) {
		return nullptr;
	}
	try {
		duckdb::LogicalType *logical_type =
		    new duckdb::LogicalType(duckdb::LogicalType::LIST(*reinterpret_cast<duckdb::LogicalType *>(type)));
		return reinterpret_cast<duckdb_logical_type>(logical_type);
	} catch (...) {
		return nullptr;
	}
}

duckdb_logical_type duckdb_create_array_type(duckdb_logical_type type, idx_t array_size) {
	if (!type) {
		return nullptr;
	}
	if (array_size >= duckdb::ArrayType::MAX_ARRAY_SIZE) {
		return nullptr;
	}
	try {
		duckdb::LogicalType *ltype = new duckdb::LogicalType(
		    duckdb::LogicalType::ARRAY(*reinterpret_cast<duckdb::LogicalType *>(type), array_size));
		return reinterpret_cast<duckdb_logical_type>(ltype);
	} catch (...) {
		return nullptr;
	}
}

duckdb_logical_type duckdb_create_union_type(duckdb_logical_type *member_types_p, const char **member_names,
                                             idx_t member_count) {
	if (!member_types_p || !member_names) {
		return nullptr;
	}
	duckdb::LogicalType **member_types = reinterpret_cast<duckdb::LogicalType **>(member_types_p);
	try {
		duckdb::child_list_t<duckdb::LogicalType> members;

		for (idx_t i = 0; i < member_count; i++) {
			members.emplace_back(make_pair(member_names[i], *member_types[i]));
		}
		duckdb::LogicalType *mtype = new duckdb::LogicalType(duckdb::LogicalType::UNION(members));
		return reinterpret_cast<duckdb_logical_type>(mtype);
	} catch (...) {
		return nullptr;
	}
}

duckdb_logical_type duckdb_create_struct_type(duckdb_logical_type *member_types_p, const char **member_names,
                                              idx_t member_count) {
	if (!member_types_p || !member_names) {
		return nullptr;
	}
	duckdb::LogicalType **member_types = (duckdb::LogicalType **)member_types_p;
	for (idx_t i = 0; i < member_count; i++) {
		if (!member_names[i] || !member_types[i]) {
			return nullptr;
		}
	}

	try {
		duckdb::child_list_t<duckdb::LogicalType> members;

		for (idx_t i = 0; i < member_count; i++) {
			members.emplace_back(make_pair(member_names[i], *member_types[i]));
		}
		// an unnamed (empty member names) struct is a TUPLE - construct it as such so it is treated canonically
		auto struct_type = duckdb::LogicalType::STRUCT(members);
		if (duckdb::StructType::IsUnnamed(struct_type)) {
			struct_type = duckdb::LogicalType::TUPLE(std::move(members));
		}
		duckdb::LogicalType *mtype = new duckdb::LogicalType(std::move(struct_type));
		return reinterpret_cast<duckdb_logical_type>(mtype);
	} catch (...) {
		return nullptr;
	}
}

duckdb_logical_type duckdb_create_enum_type(const char **member_names, idx_t member_count) {
	if (!member_names) {
		return nullptr;
	}
	duckdb::Vector enum_vector(duckdb::LogicalType::VARCHAR, member_count);
	auto enum_vector_ptr = duckdb::FlatVector::GetDataMutable<duckdb::string_t>(enum_vector);

	for (idx_t i = 0; i < member_count; i++) {
		if (!member_names[i]) {
			return nullptr;
		}
		enum_vector_ptr[i] = duckdb::StringVector::AddStringOrBlob(enum_vector, member_names[i]);
	}

	try {
		duckdb::LogicalType *mtype = new duckdb::LogicalType(duckdb::LogicalType::ENUM(enum_vector, member_count));
		return reinterpret_cast<duckdb_logical_type>(mtype);
	} catch (...) {
		return nullptr;
	}
}

duckdb_logical_type duckdb_create_map_type(duckdb_logical_type key_type, duckdb_logical_type value_type) {
	if (!key_type || !value_type) {
		return nullptr;
	}
	try {
		duckdb::LogicalType *mtype = new duckdb::LogicalType(duckdb::LogicalType::MAP(
		    *reinterpret_cast<duckdb::LogicalType *>(key_type), *reinterpret_cast<duckdb::LogicalType *>(value_type)));
		return reinterpret_cast<duckdb_logical_type>(mtype);
	} catch (...) {
		return nullptr;
	}
}

duckdb_logical_type duckdb_create_decimal_type(uint8_t width, uint8_t scale) {
	if (!duckdb::Decimal::IsValidWidthScale(width, scale)) {
		return nullptr;
	}
	try {
		return reinterpret_cast<duckdb_logical_type>(
		    new duckdb::LogicalType(duckdb::LogicalType::DECIMAL(width, scale)));
	} catch (...) {
		return nullptr;
	}
}

duckdb_type duckdb_get_type_id(duckdb_logical_type type) {
	if (!type) {
		return DUCKDB_TYPE_INVALID;
	}
	auto logical_type = reinterpret_cast<duckdb::LogicalType *>(type);
	return duckdb::LogicalTypeIdToC(logical_type->id());
}

void duckdb_destroy_logical_type(duckdb_logical_type *type) {
	if (type && *type) {
		auto logical_type = reinterpret_cast<duckdb::LogicalType *>(*type);
		delete logical_type;
		*type = nullptr;
	}
}

uint8_t duckdb_decimal_width(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::DECIMAL)) {
		return 0;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return duckdb::DecimalType::GetWidth(logical_type);
}

uint8_t duckdb_decimal_scale(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::DECIMAL)) {
		return 0;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return duckdb::DecimalType::GetScale(logical_type);
}

duckdb_type duckdb_decimal_internal_type(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::DECIMAL)) {
		return DUCKDB_TYPE_INVALID;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	switch (logical_type.InternalType()) {
	case duckdb::PhysicalType::INT16:
		return DUCKDB_TYPE_SMALLINT;
	case duckdb::PhysicalType::INT32:
		return DUCKDB_TYPE_INTEGER;
	case duckdb::PhysicalType::INT64:
		return DUCKDB_TYPE_BIGINT;
	case duckdb::PhysicalType::INT128:
		return DUCKDB_TYPE_HUGEINT;
	default:
		return DUCKDB_TYPE_INVALID;
	}
}

duckdb_type duckdb_enum_internal_type(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::ENUM)) {
		return DUCKDB_TYPE_INVALID;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	switch (logical_type.InternalType()) {
	case duckdb::PhysicalType::UINT8:
		return DUCKDB_TYPE_UTINYINT;
	case duckdb::PhysicalType::UINT16:
		return DUCKDB_TYPE_USMALLINT;
	case duckdb::PhysicalType::UINT32:
		return DUCKDB_TYPE_UINTEGER;
	default:
		return DUCKDB_TYPE_INVALID;
	}
}

uint32_t duckdb_enum_dictionary_size(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::ENUM)) {
		return 0;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return duckdb::NumericCast<uint32_t>(duckdb::EnumType::GetSize(logical_type));
}

char *duckdb_enum_dictionary_value(duckdb_logical_type type, idx_t index) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::ENUM)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	auto &vector = duckdb::EnumType::GetValuesInsertOrder(logical_type);
	auto value = vector.GetValue(index);
	return strdup(duckdb::StringValue::Get(value).c_str());
}

duckdb_logical_type duckdb_list_type_child_type(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::LIST) &&
	    !AssertLogicalTypeId(type, duckdb::LogicalTypeId::MAP)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (logical_type.id() != duckdb::LogicalTypeId::LIST && logical_type.id() != duckdb::LogicalTypeId::MAP) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(duckdb::ListType::GetChildType(logical_type)));
}

duckdb_logical_type duckdb_array_type_child_type(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::ARRAY)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (logical_type.id() != duckdb::LogicalTypeId::ARRAY) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(
	    new duckdb::LogicalType(duckdb::ArrayType::GetChildType(logical_type)));
}

idx_t duckdb_array_type_array_size(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::ARRAY)) {
		return 0;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (logical_type.id() != duckdb::LogicalTypeId::ARRAY) {
		return 0;
	}
	return duckdb::ArrayType::GetSize(logical_type);
}

duckdb_logical_type duckdb_map_type_key_type(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::MAP)) {
		return nullptr;
	}
	auto &mtype = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (mtype.id() != duckdb::LogicalTypeId::MAP) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(duckdb::MapType::KeyType(mtype)));
}

duckdb_logical_type duckdb_map_type_value_type(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::MAP)) {
		return nullptr;
	}
	auto &mtype = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (mtype.id() != duckdb::LogicalTypeId::MAP) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(new duckdb::LogicalType(duckdb::MapType::ValueType(mtype)));
}

idx_t duckdb_struct_type_child_count(duckdb_logical_type type) {
	if (!AssertInternalType(type, duckdb::PhysicalType::STRUCT)) {
		return 0;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return duckdb::StructType::GetChildCount(logical_type);
}

idx_t duckdb_union_type_member_count(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::UNION)) {
		return 0;
	}
	idx_t member_count = duckdb_struct_type_child_count(type);
	if (member_count != 0) {
		member_count--;
	}
	return member_count;
}

char *duckdb_union_type_member_name(duckdb_logical_type type, idx_t index) {
	if (!AssertInternalType(type, duckdb::PhysicalType::STRUCT)) {
		return nullptr;
	}
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::UNION)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return strdup(duckdb::UnionType::GetMemberName(logical_type, index).c_str());
}

duckdb_logical_type duckdb_union_type_member_type(duckdb_logical_type type, idx_t index) {
	if (!AssertInternalType(type, duckdb::PhysicalType::STRUCT)) {
		return nullptr;
	}
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::UNION)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return reinterpret_cast<duckdb_logical_type>(
	    new duckdb::LogicalType(duckdb::UnionType::GetMemberType(logical_type, index)));
}

char *duckdb_struct_type_child_name(duckdb_logical_type type, idx_t index) {
	if (!AssertInternalType(type, duckdb::PhysicalType::STRUCT)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return strdup(duckdb::StructType::GetChildName(logical_type, index).c_str());
}

char *duckdb_logical_type_get_alias(duckdb_logical_type type) {
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	return logical_type.HasAlias() ? strdup(logical_type.GetAlias().c_str()) : nullptr;
}

void duckdb_logical_type_set_alias(duckdb_logical_type type, const char *alias) {
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	logical_type = logical_type.WithAlias(alias);
}

duckdb_logical_type duckdb_struct_type_child_type(duckdb_logical_type type, idx_t index) {
	if (!AssertInternalType(type, duckdb::PhysicalType::STRUCT)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (logical_type.InternalType() != duckdb::PhysicalType::STRUCT) {
		return nullptr;
	}
	return reinterpret_cast<duckdb_logical_type>(
	    new duckdb::LogicalType(duckdb::StructType::GetChildType(logical_type, index)));
}

duckdb_state duckdb_register_logical_type(duckdb_connection connection, duckdb_logical_type type,
                                          duckdb_create_type_info info) {
	if (!connection || !type) {
		return DuckDBError;
	}

	// Unused for now
	(void)info;

	const auto &base_type = *reinterpret_cast<duckdb::LogicalType *>(type);
	if (!base_type.HasAlias()) {
		return DuckDBError;
	}

	if (duckdb::TypeVisitor::Contains(base_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(base_type, duckdb::LogicalTypeId::ANY)) {
		return DuckDBError;
	}

	try {
		const auto con = reinterpret_cast<duckdb::Connection *>(connection);
		con->context->RunFunctionInTransaction([&]() {
			auto &catalog = duckdb::Catalog::GetSystemCatalog(*con->context);
			duckdb::CreateTypeInfo info(base_type.GetAlias(), base_type);
			info.temporary = true;
			info.internal = true;
			catalog.CreateType(*con->context, info);
		});
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

char *duckdb_geometry_type_get_crs(duckdb_logical_type type) {
	if (!AssertLogicalTypeId(type, duckdb::LogicalTypeId::GEOMETRY)) {
		return nullptr;
	}
	auto &logical_type = *(reinterpret_cast<duckdb::LogicalType *>(type));
	if (!duckdb::GeoType::HasCRS(logical_type)) {
		return nullptr;
	}
	return strdup(duckdb::GeoType::GetCRS(logical_type).GetDefinition().c_str());
}
