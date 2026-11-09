#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/parser/parsed_data/alter_macro_info.hpp"

namespace duckdb {

MacroCatalogEntry::MacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : FunctionEntry(
          (info.macros[0]->type == MacroType::SCALAR_MACRO ? CatalogType::MACRO_ENTRY : CatalogType::TABLE_MACRO_ENTRY),
          catalog, schema, info),
      macros(std::move(info.macros)) {
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->extension_name = info.extension_name;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
}

unique_ptr<MacroCatalogEntry> MacroCatalogEntry::Create(Catalog &catalog, SchemaCatalogEntry &schema,
                                                        CreateMacroInfo &info) {
	if (info.type == CatalogType::TABLE_MACRO_ENTRY) {
		return make_uniq<TableMacroCatalogEntry>(catalog, schema, info);
	}
	return make_uniq<ScalarMacroCatalogEntry>(catalog, schema, info);
}

ScalarMacroCatalogEntry::ScalarMacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : MacroCatalogEntry(catalog, schema, info) {
}

unique_ptr<CatalogEntry> ScalarMacroCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateMacroInfo>();
	auto result = make_uniq<ScalarMacroCatalogEntry>(catalog, schema, cast_info);
	return std::move(result);
}

TableMacroCatalogEntry::TableMacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info)
    : MacroCatalogEntry(catalog, schema, info) {
}

unique_ptr<CatalogEntry> TableMacroCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateMacroInfo>();
	auto result = make_uniq<TableMacroCatalogEntry>(catalog, schema, cast_info);
	return std::move(result);
}

unique_ptr<CreateInfo> MacroCatalogEntry::GetInfo() const {
	auto info = make_uniq<CreateMacroInfo>(type);
	info->SetQualifiedName(schema.GetQualifiedName(name));
	for (auto &function : macros) {
		info->macros.push_back(function->Copy());
	}
	info->extension_name = extension_name;
	info->dependencies = dependencies;
	info->comment = comment;
	info->tags = tags;
	return std::move(info);
}

optional_idx MacroCatalogEntry::FindMember(const vector<LogicalType> &signature) const {
	// the signature names a declared overload exactly: a type matches an equal declared type and
	// ANY matches an untyped slot, which the create-time conflict rules keep unique
	for (idx_t i = 0; i < macros.size(); i++) {
		if (macros[i]->MatchesSignature(signature)) {
			return i;
		}
	}
	return optional_idx();
}

string MacroCatalogEntry::ToSQL() const {
	auto create_info = GetInfo();
	create_info->StripCatalogQualification();
	return create_info->ToString();
}

unique_ptr<CatalogEntry> MacroCatalogEntry::AlterEntry(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_MACRO) {
		return CatalogEntry::AlterEntry(transaction, info);
	}
	auto &macro_info = info.Cast<AlterMacroInfo>();
	if (macro_info.alter_macro_type != AlterMacroType::REMOVE_OVERLOAD) {
		throw InternalException("Unsupported ALTER MACRO type");
	}
	auto &remove_info = macro_info.Cast<RemoveMacroOverloadInfo>();
	auto member_idx = FindMember(remove_info.signature);
	if (!member_idx.IsValid()) {
		if (remove_info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			// IF EXISTS at member granularity: nothing to drop, siblings are unaffected
			return nullptr;
		}
		string signature_text;
		for (auto &sig_type : remove_info.signature) {
			if (!signature_text.empty()) {
				signature_text += ", ";
			}
			signature_text += sig_type.ToString();
		}
		throw CatalogException("Function %s has no overload with signature (%s)", name, signature_text);
	}
	if (macros.size() == 1) {
		// the binder plans a DROP for the last overload, an alter cannot remove the entry itself
		throw CatalogException("Cannot remove the last overload of %s: drop the macro instead", name);
	}
	if (remove_info.cascade) {
		// dependencies are tracked per entry, not per member, so a cascade that leaves siblings
		// behind has no well-defined scope: require an explicit choice
		throw CatalogException("Cannot drop an overload of %s with CASCADE while other overloads remain: drop the "
		                       "whole entry or its dependents first",
		                       name);
	}
	auto new_info_base = GetInfo();
	auto &new_info = new_info_base->Cast<CreateMacroInfo>();
	auto removed_member = std::move(new_info.macros[member_idx.GetIndex()]);
	new_info.macros.erase(new_info.macros.begin() + NumericCast<int64_t>(member_idx.GetIndex()));
	new_info.temporary = temporary;
	// derive the replacement's dependencies from the members' create-time attribution: a dependency
	// owned only by the removed member must not tie the replacement to a later cascade
	bool attribution_known = removed_member->dependencies_recorded;
	for (auto &member : new_info.macros) {
		attribution_known = attribution_known && member->dependencies_recorded;
	}
	if (!attribution_known && !new_info.dependencies.Set().empty()) {
		throw CatalogException("Cannot drop an overload of %s: the entry was stored before member dependency "
		                       "tracking, so its dependencies cannot be attributed to a single overload. Drop or "
		                       "recreate the whole entry instead",
		                       name);
	}
	LogicalDependencyList member_union = removed_member->recorded_dependencies;
	LogicalDependencyList derived_dependencies;
	for (auto &member : new_info.macros) {
		for (auto &dep : member->recorded_dependencies.Set()) {
			derived_dependencies.AddDependency(dep);
			member_union.AddDependency(dep);
		}
	}
	// entry-level dependencies no member accounts for are kept
	for (auto &dep : new_info.dependencies.Set()) {
		if (member_union.Set().count(dep) == 0) {
			derived_dependencies.AddDependency(dep);
		}
	}
	new_info.dependencies = std::move(derived_dependencies);
	// the derived list replaces the entry's registered dependencies during the alter. The containing
	// schema is registered separately from the entry's own list at CREATE, so re-add it here
	info.new_dependencies = make_uniq<LogicalDependencyList>(new_info.dependencies);
	info.new_dependencies->AddDependency(schema);
	auto function = MacroCatalogEntry::Create(ParentCatalog(), schema, new_info);
	function->internal = internal;
	return std::move(function);
}

} // namespace duckdb
