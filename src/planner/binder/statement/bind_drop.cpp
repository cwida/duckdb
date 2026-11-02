#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_drop.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

void Binder::BindDropTrigger(DropStatement &stmt, StatementProperties &properties) {
	if (!stmt.info->extra_drop_info) {
		throw BinderException("DROP TRIGGER requires an ON clause specifying the table");
	}
	auto &trigger_extra = stmt.info->extra_drop_info->Cast<ExtraDropTriggerInfo>();
	if (!trigger_extra.base_table) {
		throw BinderException("DROP TRIGGER requires an ON clause specifying the table");
	}
	auto &base_table_ref = trigger_extra.base_table->Cast<BaseTableRef>();
	// resolve the (possibly nested) catalog/schema qualification of the base table
	base_table_ref.SetQualifiedName(BindTableName(base_table_ref.GetQualifiedName()));
	// IF EXISTS only guards the trigger, not the table (PostgreSQL-compatible behavior).
	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context, base_table_ref.GetQualifiedName());
	// the trigger lives in the same (possibly nested) schema as its base table
	stmt.info->SetQualifiedName(table_entry.ParentSchema().GetQualifiedName(stmt.info->GetQualifiedName().Name()));
	properties.RegisterDBModify(table_entry.ParentCatalog(), context, DatabaseModificationType::DROP_CATALOG_ENTRY);
}

BoundStatement Binder::Bind(DropStatement &stmt) {
	BoundStatement result;

	auto &properties = GetStatementProperties();
	switch (stmt.info->type) {
	case CatalogType::PREPARED_STATEMENT:
		// dropping prepared statements is always possible
		// it also does not require a valid transaction
		properties.requires_valid_transaction = false;
		break;
	case CatalogType::SCHEMA_ENTRY: {
		// resolve the leading component of the dotted path into a catalog, leaving the path as
		// [catalog, parent schemas..., schema] (mirrors CREATE SCHEMA - see Binder::BindCreateSchema)
		stmt.info->SetQualifiedName(ResolveCatalog(context, stmt.info->GetQualifiedName()));
		// dropping a schema is never read-only because there are no temporary schemas. The catalog is the leading
		// component of the resolved path ([catalog, parent schemas..., schema])
		auto &catalog = Catalog::GetCatalog(context, stmt.info->GetQualifiedName().Path().front());
		properties.RegisterDBModify(catalog, context, stmt.info->GetModifications());
		break;
	}
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::TABLE_ENTRY:
	case CatalogType::TYPE_ENTRY: {
		// Resolve the catalog + (possibly nested) schema path. A leading component is the catalog when it names an
		// attached database, and otherwise the outermost schema of a nested schema path. The entry lookup below
		// navigates whatever qualification comes out of this.
		stmt.info->SetQualifiedName(BindTableName(stmt.info->GetQualifiedName()));
		auto catalog = Catalog::GetCatalogEntry(context, stmt.info->GetQualifiedName().Catalog());
		if (catalog) {
			// mark catalog as accessed
			properties.RegisterDBRead(*catalog, context);
		}
		optional_ptr<CatalogEntry> entry;
		if (stmt.info->type == CatalogType::MACRO_ENTRY) {
			// We also support "DROP MACRO" (instead of "DROP MACRO TABLE") for table macros
			// First try to drop a scalar macro
			EntryLookupInfo macro_entry_lookup(stmt.info->type, stmt.info->GetQualifiedName());
			entry = Catalog::GetEntry(context, macro_entry_lookup, OnEntryNotFound::RETURN_NULL);
			if (!entry) {
				// Unable to find a scalar macro, try to drop a table macro
				EntryLookupInfo table_macro_entry_lookup(CatalogType::TABLE_MACRO_ENTRY, stmt.info->GetQualifiedName());
				entry = Catalog::GetEntry(context, table_macro_entry_lookup, OnEntryNotFound::RETURN_NULL);
				if (entry) {
					// Change type to table macro so future lookups get the correct one
					stmt.info->type = CatalogType::TABLE_MACRO_ENTRY;
				}
			}

			if (!entry) {
				// Unable to find table macro, try again with original OnEntryNotFound to ensure we throw if necessary
				entry = Catalog::GetEntry(context, macro_entry_lookup, stmt.info->if_not_found);
			}
		} else {
			EntryLookupInfo entry_lookup(stmt.info->type, stmt.info->GetQualifiedName());
			entry = Catalog::GetEntry(context, entry_lookup, stmt.info->if_not_found);
		}
		if (!entry) {
			break;
		}
		if (entry->internal) {
			throw CatalogException("Cannot drop internal catalog entry %s!", entry->name);
		}
		// keep the entry's full (possibly nested) schema path so execution navigates the same schema
		stmt.info->SetQualifiedName(entry->ParentSchema().GetQualifiedName(stmt.info->GetQualifiedName().Name()));
		properties.RegisterDBModify(entry->ParentCatalog(), context, stmt.info->GetModifications());
		break;
	}
	case CatalogType::SECRET_ENTRY: {
		//! Secrets are stored in the secret manager; they can always be dropped
		properties.requires_valid_transaction = false;
		break;
	}
	case CatalogType::TRIGGER_ENTRY:
		BindDropTrigger(stmt, properties);
		break;
	default:
		throw BinderException("Unknown catalog type for drop statement: '%s'", CatalogTypeToString(stmt.info->type));
	}
	result.plan = make_uniq<LogicalDrop>(std::move(stmt.info));
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
