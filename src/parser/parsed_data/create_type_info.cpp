#include "duckdb/parser/parsed_data/create_type_info.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

CreateTypeInfo::CreateTypeInfo() : CreateInfo(CatalogType::TYPE_ENTRY) {
}
CreateTypeInfo::CreateTypeInfo(string name_p, LogicalType type_p, bind_logical_type_function_t bind_function_p)
    : CreateInfo(CatalogType::TYPE_ENTRY), type(std::move(type_p)) {
	SetTypeName(Identifier(std::move(name_p)));
	if (bind_function_p) {
		constructors.AddFunction(TypeConstructor::Unchecked(GetTypeName(), bind_function_p));
	}
}
CreateTypeInfo::CreateTypeInfo(string name_p, LogicalType type_p, TypeConstructorSet constructors_p)
    : CreateInfo(CatalogType::TYPE_ENTRY), type(std::move(type_p)), constructors(std::move(constructors_p)) {
	SetTypeName(Identifier(std::move(name_p)));
}

unique_ptr<CreateInfo> CreateTypeInfo::Copy() const {
	auto result = make_uniq<CreateTypeInfo>();
	CopyProperties(*result);
	result->SetTypeName(GetTypeName());
	result->type = type;
	if (query) {
		result->query = query->Copy();
	}
	result->constructors = constructors;
	return std::move(result);
}

string CreateTypeInfo::ToString() const {
	string result = GetCreatePrefix("TYPE");
	result += QualifiedNameToString();
	if (type.id() == LogicalTypeId::ENUM) {
		auto &values_insert_order = EnumType::GetValuesInsertOrder(type);
		idx_t size = EnumType::GetSize(type);

		result += " AS ENUM ( ";
		for (idx_t i = 0; i < size; i++) {
			result += SQLString(values_insert_order.GetValue(i).ToString());
			if (i != size - 1) {
				result += ", ";
			}
		}
		result += " )";
	} else if (type.id() == LogicalTypeId::INVALID) {
		// CREATE TYPE mood AS ENUM (SELECT 'happy')
		D_ASSERT(query);
		result += " AS ENUM (" + query->ToString() + ")";
	} else {
		result += " AS ";
		result += type.ToString();
	}
	result += ";";
	return result;
}

} // namespace duckdb
