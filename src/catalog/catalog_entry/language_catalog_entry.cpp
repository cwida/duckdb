#include "duckdb/catalog/catalog_entry/language_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/language_function_binder.hpp"

namespace duckdb {

CreateLanguageInfo::CreateLanguageInfo(Identifier name_p) : CreateInfo(CatalogType::LANGUAGE_ENTRY) {
	name = std::move(name_p);
	SetQualifiedName(QualifiedName(Identifier(SYSTEM_CATALOG), Identifier::DefaultSchema(), name));
	internal = true;
}

unique_ptr<CreateInfo> CreateLanguageInfo::Copy() const {
	auto result = make_uniq<CreateLanguageInfo>(name);
	CopyProperties(*result);
	result->binder = binder;
	return std::move(result);
}

void CreateLanguageInfo::Serialize(Serializer &serializer) const {
	throw InternalException("Language entries are transient and cannot be serialized");
}

LanguageCatalogEntry::LanguageCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateLanguageInfo &info)
    : StandardEntry(CatalogType::LANGUAGE_ENTRY, schema, catalog, info.name), binder(info.binder) {
	internal = true;
}

optional_ptr<LanguageCatalogEntry> LanguageCatalogEntry::TryGet(ClientContext &context, const string &language) {
	QualifiedName language_name(Identifier(SYSTEM_CATALOG), Identifier::DefaultSchema(), Identifier(language));
	auto entry = Catalog::GetEntry<LanguageCatalogEntry>(context, language_name, OnEntryNotFound::RETURN_NULL);
	if (entry) {
		return entry;
	}
	// by convention a language is provided by the extension of the same name, try the auto-loader
	if (Catalog::TryAutoLoad(context, language)) {
		return Catalog::GetEntry<LanguageCatalogEntry>(context, language_name, OnEntryNotFound::RETURN_NULL);
	}
	return nullptr;
}

shared_ptr<LanguageFunctionBinder> LanguageCatalogEntry::GetBinder(ClientContext &context, const string &language,
                                                                   const string &function_name) {
	auto entry = TryGet(context, language);
	if (!entry || !entry->binder) {
		throw MissingExtensionException(
		    "Function \"%s\" is written in language \"%s\", but no binder for that language is registered. Language "
		    "support is provided by extensions, e.g.: INSTALL %s; LOAD %s;",
		    function_name, language, language, language);
	}
	// the owning handle keeps the binder alive even when the language is re-registered meanwhile
	return entry->binder;
}

} // namespace duckdb
