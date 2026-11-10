#include "duckdb/parser/parsed_data/alter_macro_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// AlterMacroInfo
//===--------------------------------------------------------------------===//
AlterMacroInfo::AlterMacroInfo(AlterMacroType alter_macro_type)
    : AlterInfo(AlterType::ALTER_MACRO), alter_macro_type(alter_macro_type),
      entry_catalog_type(CatalogType::MACRO_ENTRY) {
}

AlterMacroInfo::AlterMacroInfo(AlterMacroType alter_macro_type, const AlterEntryData &data,
                               CatalogType entry_catalog_type)
    : AlterInfo(AlterType::ALTER_MACRO, data.qualified_name, data.if_not_found), alter_macro_type(alter_macro_type),
      entry_catalog_type(entry_catalog_type) {
}

AlterMacroInfo::~AlterMacroInfo() {
}

CatalogType AlterMacroInfo::GetCatalogType() const {
	return entry_catalog_type;
}

vector<CatalogType> AlterMacroInfo::GetEntryLookupTypes() const {
	if (entry_catalog_type == CatalogType::MACRO_ENTRY) {
		return {CatalogType::MACRO_ENTRY, CatalogType::TABLE_MACRO_ENTRY};
	}
	return {entry_catalog_type};
}

void AlterMacroInfo::SetResolvedEntryType(CatalogType type) {
	entry_catalog_type = type;
}

//===--------------------------------------------------------------------===//
// RemoveMacroOverloadInfo
//===--------------------------------------------------------------------===//
RemoveMacroOverloadInfo::RemoveMacroOverloadInfo() : AlterMacroInfo(AlterMacroType::REMOVE_OVERLOAD) {
}

RemoveMacroOverloadInfo::RemoveMacroOverloadInfo(const AlterEntryData &data, CatalogType entry_catalog_type,
                                                 vector<LogicalType> signature)
    : AlterMacroInfo(AlterMacroType::REMOVE_OVERLOAD, data, entry_catalog_type), signature(std::move(signature)) {
}

RemoveMacroOverloadInfo::~RemoveMacroOverloadInfo() {
}

unique_ptr<AlterInfo> RemoveMacroOverloadInfo::Copy() const {
	auto result = make_uniq<RemoveMacroOverloadInfo>(GetAlterEntryData(), entry_catalog_type, signature);
	result->allow_internal = allow_internal;
	return std::move(result);
}

vector<reference<LogicalType>> RemoveMacroOverloadInfo::GetTypesToBind() {
	vector<reference<LogicalType>> result;
	for (auto &type : signature) {
		result.push_back(type);
	}
	return result;
}

string RemoveMacroOverloadInfo::ToString() const {
	// the alter is how a signature drop executes: render the statement that produces it
	string result = "DROP " + ParseInfo::TypeToString(entry_catalog_type);
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += " IF EXISTS";
	}
	result += " " + GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA) + "(";
	for (idx_t i = 0; i < signature.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += signature[i].ToString();
	}
	result += ");";
	return result;
}

} // namespace duckdb
