#include "duckdb/parser/parsed_data/alter_table_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// AlterTableFunctionInfo
//===--------------------------------------------------------------------===//
AlterTableFunctionInfo::AlterTableFunctionInfo(AlterTableFunctionType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_TABLE_FUNCTION, data.GetQualifiedName(), data.if_not_found),
      alter_table_function_type(type) {
}
AlterTableFunctionInfo::~AlterTableFunctionInfo() {
}

CatalogType AlterTableFunctionInfo::GetCatalogType() const {
	return CatalogType::TABLE_FUNCTION_ENTRY;
}

//===--------------------------------------------------------------------===//
// AddTableFunctionOverloadInfo
//===--------------------------------------------------------------------===//
AddTableFunctionOverloadInfo::AddTableFunctionOverloadInfo(const AlterEntryData &data, TableFunctionSet new_overloads_p,
                                                           vector<FunctionDescription> new_descriptions_p)
    : AlterTableFunctionInfo(AlterTableFunctionType::ADD_FUNCTION_OVERLOADS, data),
      new_overloads(std::move(new_overloads_p)), new_descriptions(std::move(new_descriptions_p)) {
	this->allow_internal = true;
}

AddTableFunctionOverloadInfo::~AddTableFunctionOverloadInfo() {
}

unique_ptr<AlterInfo> AddTableFunctionOverloadInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddTableFunctionOverloadInfo>(GetAlterEntryData(), new_overloads,
	                                                               new_descriptions);
}

string AddTableFunctionOverloadInfo::ToString() const {
	throw NotImplementedException("NOT PARSABLE");
}

} // namespace duckdb
