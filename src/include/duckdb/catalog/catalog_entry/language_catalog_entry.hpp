//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/language_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/parser/parsed_data/create_language_info.hpp"

namespace duckdb {

//! A function language registered by an extension. Lives in the system catalog only and is never
//! persisted: a database file holding functions of this language opens without it.
class LanguageCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::LANGUAGE_ENTRY;
	static constexpr const char *Name = "language";

	LanguageCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateLanguageInfo &info);

	//! The binder that resolves calls to this language's members. shared_ptr, so a bind in flight
	//! keeps its binder alive even when the language is re-registered meanwhile.
	shared_ptr<LanguageFunctionBinder> binder;

public:
	//! The registered language, attempting extension auto-load by the language's name.
	//! Returns nullptr when none is available.
	static optional_ptr<LanguageCatalogEntry> TryGet(ClientContext &context, const string &language);
	//! The language's binder as an owning handle, raising a MissingExtensionException naming the
	//! function and its language when the language is not registered
	static shared_ptr<LanguageFunctionBinder> GetBinder(ClientContext &context, const string &language,
	                                                    const string &function_name);
};

} // namespace duckdb
