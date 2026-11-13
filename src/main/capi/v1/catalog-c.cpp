#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"

namespace duckdb {
namespace {

struct CCatalogWrapper {
	Catalog &catalog;
	string catalog_type;
	CCatalogWrapper(Catalog &catalog, const string &catalog_type) : catalog(catalog), catalog_type(catalog_type) {
	}
};

struct CCatalogEntryWrapper {
	CatalogEntry &entry;
	//! Set when the entry is the function implementing a collation: the entry is reported as that collation
	string collation_name;
	CCatalogEntryWrapper(CatalogEntry &entry) : entry(entry) {
	}
};

CatalogType CatalogTypeFromC(duckdb_catalog_entry_type type) {
	switch (type) {
	case DUCKDB_CATALOG_ENTRY_TYPE_TABLE:
		return CatalogType::TABLE_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_SCHEMA:
		return CatalogType::SCHEMA_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_VIEW:
		return CatalogType::VIEW_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_INDEX:
		return CatalogType::INDEX_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_PREPARED_STATEMENT:
		return CatalogType::PREPARED_STATEMENT;
	case DUCKDB_CATALOG_ENTRY_TYPE_SEQUENCE:
		return CatalogType::SEQUENCE_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_COLLATION:
		return CatalogType::COLLATION_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_TYPE:
		return CatalogType::TYPE_ENTRY;
	case DUCKDB_CATALOG_ENTRY_TYPE_DATABASE:
		return CatalogType::DATABASE_ENTRY;
	default:
		return CatalogType::INVALID;
	}
}

duckdb_catalog_entry_type CatalogTypeToC(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_TABLE;
	case CatalogType::SCHEMA_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_SCHEMA;
	case CatalogType::VIEW_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_VIEW;
	case CatalogType::INDEX_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_INDEX;
	case CatalogType::PREPARED_STATEMENT:
		return DUCKDB_CATALOG_ENTRY_TYPE_PREPARED_STATEMENT;
	case CatalogType::SEQUENCE_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_SEQUENCE;
	case CatalogType::COLLATION_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_COLLATION;
	case CatalogType::TYPE_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_TYPE;
	case CatalogType::DATABASE_ENTRY:
		return DUCKDB_CATALOG_ENTRY_TYPE_DATABASE;
	default:
		return DUCKDB_CATALOG_ENTRY_TYPE_INVALID;
	}
}

} // namespace
} // namespace duckdb

//----------------------------------------------------------------------------------------------------------------------
// Catalog
//----------------------------------------------------------------------------------------------------------------------
duckdb_catalog duckdb_client_context_get_catalog(duckdb_client_context context, const char *name) {
	if (!context || !name || strlen(name) == 0) {
		return nullptr;
	}

	auto &context_ref = *reinterpret_cast<duckdb::CClientContextWrapper *>(context);
	if (!context_ref.context.transaction.HasActiveTransaction()) {
		return nullptr;
	}

	auto catalog_ptr = duckdb::Catalog::GetCatalogEntry(context_ref.context, duckdb::Identifier(name));

	if (!catalog_ptr) {
		return nullptr;
	}

	auto &catalog_ref = *catalog_ptr;
	auto catalog_wrapper = new duckdb::CCatalogWrapper(catalog_ref, catalog_ref.GetCatalogType());
	return reinterpret_cast<duckdb_catalog>(catalog_wrapper);
}

void duckdb_destroy_catalog(duckdb_catalog *catalog) {
	if (!catalog || !*catalog) {
		return;
	}
	auto catalog_ptr = reinterpret_cast<duckdb::CCatalogWrapper *>(*catalog);
	delete catalog_ptr;
	*catalog = nullptr;
}

const char *duckdb_catalog_get_type_name(duckdb_catalog catalog) {
	if (!catalog) {
		return nullptr;
	}
	auto &catalog_ref = *reinterpret_cast<duckdb::CCatalogWrapper *>(catalog);
	return catalog_ref.catalog_type.c_str();
}

duckdb_catalog_entry duckdb_catalog_get_entry(duckdb_catalog catalog, duckdb_client_context context,
                                              duckdb_catalog_entry_type entry_type, const char *schema_name,
                                              const char *entry_name) {
	if (!catalog || !context || !schema_name || !entry_name) {
		return nullptr;
	}

	auto &catalog_ref = *reinterpret_cast<duckdb::CCatalogWrapper *>(catalog);
	auto &context_ref = *reinterpret_cast<duckdb::CClientContextWrapper *>(context);

	auto type = duckdb::CatalogTypeFromC(entry_type);
	duckdb::Identifier schema(schema_name);
	if (type == duckdb::CatalogType::COLLATION_ENTRY) {
		// a collation is the scalar function of the same name in the collation schema
		type = duckdb::CatalogType::SCALAR_FUNCTION_ENTRY;
		schema = duckdb::CollationFunctions::GetSchema();
	}
	auto entry = catalog_ref.catalog.GetEntry(context_ref.context, type, schema, duckdb::Identifier(entry_name),
	                                          duckdb::OnEntryNotFound::RETURN_NULL);

	if (!entry) {
		return nullptr;
	}

	auto wrapper = new duckdb::CCatalogEntryWrapper(*entry);
	if (entry_type == DUCKDB_CATALOG_ENTRY_TYPE_COLLATION) {
		wrapper->collation_name = entry_name;
	}
	return reinterpret_cast<duckdb_catalog_entry>(wrapper);
}

//----------------------------------------------------------------------------------------------------------------------
// Catalog Entry
//----------------------------------------------------------------------------------------------------------------------

duckdb_catalog_entry_type duckdb_catalog_entry_get_type(duckdb_catalog_entry entry) {
	if (!entry) {
		return DUCKDB_CATALOG_ENTRY_TYPE_INVALID;
	}

	auto &entry_ref = *reinterpret_cast<duckdb::CCatalogEntryWrapper *>(entry);
	if (!entry_ref.collation_name.empty()) {
		return DUCKDB_CATALOG_ENTRY_TYPE_COLLATION;
	}
	return duckdb::CatalogTypeToC(entry_ref.entry.type);
}

const char *duckdb_catalog_entry_get_name(duckdb_catalog_entry entry) {
	if (!entry) {
		return nullptr;
	}
	auto &entry_ref = *reinterpret_cast<duckdb::CCatalogEntryWrapper *>(entry);
	if (!entry_ref.collation_name.empty()) {
		return entry_ref.collation_name.c_str();
	}
	return entry_ref.entry.name.c_str();
}

void duckdb_destroy_catalog_entry(duckdb_catalog_entry *entry) {
	if (!entry || !*entry) {
		return;
	}
	auto entry_ptr = reinterpret_cast<duckdb::CCatalogEntryWrapper *>(*entry);
	delete entry_ptr;
	*entry = nullptr;
}
