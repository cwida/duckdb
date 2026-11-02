#include "duckdb/catalog/entry_lookup_info.hpp"

namespace duckdb {

EntryLookupInfo::EntryLookupInfo(CatalogType catalog_type_p, QualifiedName name_p, QueryErrorContext error_context_p)
    : catalog_type(catalog_type_p), name(std::move(name_p)), error_context(error_context_p) {
}

EntryLookupInfo::EntryLookupInfo(CatalogType catalog_type_p, QualifiedName name_p,
                                 optional_ptr<BoundAtClause> at_clause_p, QueryErrorContext error_context_p)
    : catalog_type(catalog_type_p), name(std::move(name_p)), at_clause(at_clause_p), error_context(error_context_p) {
}

EntryLookupInfo::EntryLookupInfo(CatalogType catalog_type_p, Identifier name_p, QueryErrorContext error_context_p)
    : EntryLookupInfo(catalog_type_p, QualifiedName(std::move(name_p)), error_context_p) {
}

EntryLookupInfo::EntryLookupInfo(CatalogType catalog_type_p, Identifier name_p, optional_ptr<BoundAtClause> at_clause_p,
                                 QueryErrorContext error_context_p)
    : EntryLookupInfo(catalog_type_p, QualifiedName(std::move(name_p)), at_clause_p, error_context_p) {
}

EntryLookupInfo::EntryLookupInfo(const EntryLookupInfo &parent, QualifiedName name_p)
    : catalog_type(parent.catalog_type), name(std::move(name_p)), at_clause(parent.at_clause),
      error_context(parent.error_context) {
}

EntryLookupInfo::EntryLookupInfo(const EntryLookupInfo &parent, optional_ptr<BoundAtClause> at_clause)
    : EntryLookupInfo(parent.catalog_type, parent.name, parent.at_clause ? parent.at_clause : at_clause,
                      parent.error_context) {
}

EntryLookupInfo EntryLookupInfo::SchemaLookup(const EntryLookupInfo &parent, vector<Identifier> schema_path) {
	return EntryLookupInfo(CatalogType::SCHEMA_ENTRY, QualifiedName::FromPath(std::move(schema_path)), parent.at_clause,
	                       parent.error_context);
}

CatalogType EntryLookupInfo::GetCatalogType() const {
	return catalog_type;
}

const QualifiedName &EntryLookupInfo::GetQualifiedName() const {
	return name;
}

const Identifier &EntryLookupInfo::GetEntryIdentifier() const {
	return name.Name();
}

const string &EntryLookupInfo::GetEntryName() const {
	return name.Name().GetIdentifierName();
}

const Identifier &EntryLookupInfo::GetCatalog() const {
	return name.Catalog();
}

const Identifier &EntryLookupInfo::GetSchema() const {
	return name.Schema();
}

const QueryErrorContext &EntryLookupInfo::GetErrorContext() const {
	return error_context;
}

const optional_ptr<BoundAtClause> EntryLookupInfo::GetAtClause() const {
	return at_clause;
}

} // namespace duckdb
