#include "duckdb/catalog/catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

CatalogEntry::CatalogEntry(CatalogType type, Catalog *catalog_p, string name_p)
    : oid(catalog_p->ModifyCatalog()), type(type), catalog(catalog_p), set(nullptr), name(move(name_p)), deleted(false),
      temporary(false), internal(false), parent(nullptr) {
}

CatalogEntry::~CatalogEntry() {
}

unique_ptr<CatalogEntry> CatalogEntry::AlterEntry(ClientContext &context, AlterInfo *info) {
	throw InternalException("Unsupported alter type for catalog entry!");
}

unique_ptr<CatalogEntry> CatalogEntry::Copy(ClientContext &context) {
	throw InternalException("Unsupported copy type for catalog entry!");
}

void CatalogEntry::SetAsRoot() {
}

string CatalogEntry::ToSQL() {
	throw InternalException("Unsupported catalog type for ToSQL()");
}

} // namespace duckdb
