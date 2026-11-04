#include "duckdb/parser/parsed_data/alter_schema_info.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// AlterSchemaInfo
//===--------------------------------------------------------------------===//
AlterSchemaInfo::AlterSchemaInfo(AlterSchemaType alter_schema_type)
    : AlterInfo(AlterType::ALTER_SCHEMA), alter_schema_type(alter_schema_type) {
}

AlterSchemaInfo::AlterSchemaInfo(AlterSchemaType alter_schema_type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_SCHEMA, data.qualified_name, data.if_not_found), alter_schema_type(alter_schema_type) {
}

AlterSchemaInfo::~AlterSchemaInfo() {
}

CatalogType AlterSchemaInfo::GetCatalogType() const {
	return CatalogType::SCHEMA_ENTRY;
}

const Identifier &AlterSchemaInfo::SchemaName() const {
	return GetQualifiedName().Schema();
}

const Identifier &AlterSchemaInfo::SchemaCatalog() const {
	return GetQualifiedName().Catalog();
}

vector<Identifier> AlterSchemaInfo::SchemaPath() const {
	auto &path = GetQualifiedName().Path();
	// skip the catalog (if any) and the empty trailing name
	idx_t start = path.size() >= 3 ? 1 : 0;
	vector<Identifier> result;
	for (idx_t i = start; i + 1 < path.size(); i++) {
		result.push_back(path[i]);
	}
	return result;
}

string AlterSchemaInfo::QualifiedSchemaToString() const {
	string result = "ALTER SCHEMA ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	auto &path = GetQualifiedName().Path();
	for (idx_t i = 0; i + 1 < path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(path[i]);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// SetSchemaOptionsInfo
//===--------------------------------------------------------------------===//
SetSchemaOptionsInfo::SetSchemaOptionsInfo() : AlterSchemaInfo(AlterSchemaType::SET_SCHEMA_OPTIONS) {
}

SetSchemaOptionsInfo::SetSchemaOptionsInfo(const AlterEntryData &data,
                                           case_insensitive_map_t<unique_ptr<ParsedExpression>> options)
    : AlterSchemaInfo(AlterSchemaType::SET_SCHEMA_OPTIONS, data), options(std::move(options)) {
}

SetSchemaOptionsInfo::~SetSchemaOptionsInfo() {
}

unique_ptr<AlterInfo> SetSchemaOptionsInfo::Copy() const {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> options_copy;
	for (auto &option : options) {
		options_copy.emplace(option.first, option.second->Copy());
	}
	return make_uniq<SetSchemaOptionsInfo>(GetAlterEntryData(), std::move(options_copy));
}

string SetSchemaOptionsInfo::ToString() const {
	string result = QualifiedSchemaToString();
	result += " SET (";
	idx_t i = 0;
	for (auto &entry : options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(entry.first) + "=" + entry.second->ToString();
		i++;
	}
	result += ")";
	return result;
}

//===--------------------------------------------------------------------===//
// ResetSchemaOptionsInfo
//===--------------------------------------------------------------------===//
ResetSchemaOptionsInfo::ResetSchemaOptionsInfo() : AlterSchemaInfo(AlterSchemaType::RESET_SCHEMA_OPTIONS) {
}

ResetSchemaOptionsInfo::ResetSchemaOptionsInfo(const AlterEntryData &data, identifier_set_t options)
    : AlterSchemaInfo(AlterSchemaType::RESET_SCHEMA_OPTIONS, data), options(std::move(options)) {
}

ResetSchemaOptionsInfo::~ResetSchemaOptionsInfo() {
}

unique_ptr<AlterInfo> ResetSchemaOptionsInfo::Copy() const {
	return make_uniq<ResetSchemaOptionsInfo>(GetAlterEntryData(), options);
}

string ResetSchemaOptionsInfo::ToString() const {
	string result = QualifiedSchemaToString();
	result += " RESET (";
	idx_t i = 0;
	for (auto &entry : options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(entry.GetIdentifierName());
		i++;
	}
	result += ")";
	return result;
}

} // namespace duckdb
