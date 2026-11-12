#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/parser/parsed_data/alter_macro_info.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/common/storage_compatibility.hpp"

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

bool MacroCatalogEntry::InvalidatesDependents(const CatalogEntry &previous) const {
	return previous.type == type && previous.Cast<MacroCatalogEntry>().macros.size() > macros.size();
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
	// releases before v2.1.0 do not know the ALTER_MACRO WAL record, like the RETURNS declarations
	auto &attached = ParentCatalog().GetAttached();
	if (!temporary && attached.HasStorageManager() && !attached.IsTemporary()) {
		auto &storage_manager = attached.GetStorageManager();
		const auto since = StorageCompatibility::FromString("v2.1.0").storage_version;
		if (!storage_manager.InMemory() && storage_manager.GetStorageVersion() < since) {
			string msg = "Dropping a single overload is only supported for storage versions v2.1.0 and higher.\n";
			msg += "Use an in-memory database, ATTACH with (STORAGE_VERSION v2.1.0), or a TEMP macro";
			throw CatalogException(msg);
		}
	}
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
		// an alter keeps the entry, like ALTER TABLE DROP COLUMN cannot drop the last column
		throw CatalogException("Cannot drop the last overload of %s: drop the macro itself instead", name);
	}
	auto new_info_base = GetInfo();
	auto &new_info = new_info_base->Cast<CreateMacroInfo>();
	new_info.macros.erase(new_info.macros.begin() + NumericCast<int64_t>(member_idx.GetIndex()));
	new_info.temporary = temporary;
	// the entry keeps its registered dependencies: AlterObject carries them over when the alter brings no new list
	auto function = MacroCatalogEntry::Create(ParentCatalog(), schema, new_info);
	function->internal = internal;
	return std::move(function);
}

} // namespace duckdb
