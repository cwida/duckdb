#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateMacroInfo::CreateMacroInfo(CatalogType type) : CreateFunctionInfo(type, Identifier::InvalidSchema()) {
}

CreateMacroInfo::CreateMacroInfo(CatalogType type, unique_ptr<MacroFunction> function,
                                 vector<unique_ptr<MacroFunction>> extra_functions)
    : CreateFunctionInfo(type, Identifier::InvalidSchema()) {
	macros.push_back(std::move(function));
	for (auto &macro : extra_functions) {
		macros.push_back(std::move(macro));
	}
}

string CreateMacroInfo::ToString() const {
	if (!macros.empty() && macros[0]->type == MacroType::LANGUAGE_MACRO) {
		// language members round-trip through the CREATE FUNCTION sugar, one statement per member:
		// replaying the later statements adds the members back as overloads
		string result;
		for (auto &function : macros) {
			if (!result.empty()) {
				result += "\n";
			}
			result += GetCreatePrefix("FUNCTION") + QualifiedNameToString() + function->ToSQL() + ";";
		}
		return result;
	}
	auto prefix = GetCreatePrefix("MACRO");
	prefix += QualifiedNameToString() + " ";
	string definitions;
	for (auto &function : macros) {
		if (!definitions.empty()) {
			definitions += ", ";
		}
		definitions += function->ToSQL();
	}
	return prefix + definitions + ";";
}

unique_ptr<CreateInfo> CreateMacroInfo::Copy() const {
	auto result = make_uniq<CreateMacroInfo>(type);
	for (auto &macro : macros) {
		result->macros.push_back(macro->Copy());
	}
	result->SetFunctionName(GetFunctionName());
	CopyFunctionProperties(*result);
	return std::move(result);
}

vector<unique_ptr<MacroFunction>> CreateMacroInfo::GetAllButFirstFunction() const {
	vector<unique_ptr<MacroFunction>> result;
	for (idx_t i = 1; i < macros.size(); i++) {
		result.push_back(macros[i]->Copy());
	}
	return result;
}

} // namespace duckdb
