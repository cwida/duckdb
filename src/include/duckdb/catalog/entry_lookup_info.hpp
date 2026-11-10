//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/entry_lookup_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_access_intent.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class BoundAtClause;

struct EntryLookupInfo {
public:
	//! Look up a (possibly catalog/schema-qualified) entry. The catalog/schema qualification - if any - is carried in
	//! the QualifiedName; an unqualified lookup passes a bare QualifiedName(name).
	EntryLookupInfo(CatalogType catalog_type, QualifiedName name,
	                QueryErrorContext error_context = QueryErrorContext());
	EntryLookupInfo(CatalogType catalog_type, QualifiedName name, optional_ptr<BoundAtClause> at_clause,
	                QueryErrorContext error_context);
	//! Look up an entry that the statement accesses with 'access_intent' (see GetAccessIntent)
	EntryLookupInfo(CatalogType catalog_type, QualifiedName name, optional_ptr<BoundAtClause> at_clause,
	                QueryErrorContext error_context, CatalogAccessIntent access_intent);
	//! Deprecated: pass a QualifiedName instead (use QualifiedName(name) for an unqualified lookup)
	[[deprecated("Pass a QualifiedName instead, e.g. QualifiedName(name)")]] EntryLookupInfo(
	    CatalogType catalog_type, Identifier name, QueryErrorContext error_context = QueryErrorContext());
	[[deprecated("Pass a QualifiedName instead, e.g. QualifiedName(name)")]] EntryLookupInfo(
	    CatalogType catalog_type, Identifier name, optional_ptr<BoundAtClause> at_clause,
	    QueryErrorContext error_context);
	//! Re-use a parent lookup's type/at-clause/error-context/access intent with a different (re-qualified) name
	EntryLookupInfo(const EntryLookupInfo &parent, QualifiedName name);
	EntryLookupInfo(const EntryLookupInfo &parent, optional_ptr<BoundAtClause> at_clause);
	//! Re-use a parent lookup with a different access intent
	EntryLookupInfo(const EntryLookupInfo &parent, CatalogAccessIntent access_intent);

public:
	CatalogType GetCatalogType() const;
	//! The (optionally qualified) name being looked up
	const QualifiedName &GetQualifiedName() const;
	//! The identifier being looked up (the catalog stores/compares names case-insensitively).
	const Identifier &GetEntryIdentifier() const;
	//! The raw name of the identifier being looked up.
	const string &GetEntryName() const;
	//! The catalog qualification of the entry being looked up (empty if unqualified)
	const Identifier &GetCatalog() const;
	//! The schema qualification of the entry being looked up (empty if unqualified)
	const Identifier &GetSchema() const;
	const QueryErrorContext &GetErrorContext() const;
	const optional_ptr<BoundAtClause> GetAtClause() const;
	//! What the statement does with the entry: DEFAULT (read its definition and data) unless the lookup is for a
	//! statement's target, e.g. INSERT_DATA for INSERT or DROP_ENTRY for DROP
	CatalogAccessIntent GetAccessIntent() const;

	//! Build a schema lookup from a (possibly nested) schema path ([outermost..innermost]); the innermost component is
	//! the schema name and the preceding components are its parent schemas. The schema lookup has the DEFAULT intent:
	//! the parent's intent applies to its entry, not to the schema
	static EntryLookupInfo SchemaLookup(const EntryLookupInfo &parent, vector<Identifier> schema_path);

private:
	CatalogType catalog_type;
	QualifiedName name;
	optional_ptr<BoundAtClause> at_clause;
	QueryErrorContext error_context;
	CatalogAccessIntent access_intent;
};

//! Return value of Catalog::LookupEntry
struct CatalogEntryLookup {
	optional_ptr<SchemaCatalogEntry> schema;
	optional_ptr<CatalogEntry> entry;
	ErrorData error;

	DUCKDB_API bool Found() const {
		return entry;
	}
};

} // namespace duckdb
