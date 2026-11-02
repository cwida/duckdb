#include "duckdb/execution/operator/schema/physical_drop.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalDrop::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                               OperatorSourceInput &input) const {
	switch (info->type) {
	case CatalogType::PREPARED_STATEMENT: {
		// DEALLOCATE silently ignores errors
		auto &statements = ClientData::Get(context.client).prepared_statements;
		auto stmt_iter = statements.find(info->GetQualifiedName().Name());
		if (stmt_iter != statements.end()) {
			statements.erase(stmt_iter);
		}
		break;
	}
	case CatalogType::SCHEMA_ENTRY: {
		// the catalog is the leading component of the resolved path ([catalog, parent schemas..., schema])
		auto &catalog_name = info->GetQualifiedName().Path().front();
		auto &catalog = Catalog::GetCatalog(context.client, catalog_name);
		catalog.DropEntry(context.client, *info);

		// Check if the dropped schema was set as the current schema. Compare the resolved catalog names: the dropped
		// schema's catalog is catalog.GetName(); the current default catalog may be empty (= the default database).
		auto &client_data = ClientData::Get(context.client);
		auto &default_entry = client_data.catalog_search_path->GetDefault();
		auto &current_catalog = default_entry.GetCatalog();
		auto &current_schema = default_entry.GetSchema();
		D_ASSERT(info->GetQualifiedName().Name() != DEFAULT_SCHEMA);

		auto resolved_current_catalog =
		    IsInvalidCatalog(current_catalog) ? DatabaseManager::GetDefaultDatabase(context.client) : current_catalog;
		if (catalog.GetName() == resolved_current_catalog && current_schema == info->GetQualifiedName().Name()) {
			// Reset the schema to default
			SchemaSetting::SetLocal(context.client, DEFAULT_SCHEMA);
		}
		break;
	}
	case CatalogType::SECRET_ENTRY: {
		// Note: the schema param is used to optionally pass the storage to drop from
		D_ASSERT(info->extra_drop_info);
		auto &extra_info = info->extra_drop_info->Cast<ExtraDropSecretInfo>();
		SecretManager::Get(context.client)
		    .DropSecretByName(context.client, info->GetQualifiedName().Name(), info->if_not_found,
		                      extra_info.persist_mode, Identifier(extra_info.secret_storage));
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		// Triggers are stored per-table; must be dropped through the table, not the schema
		if (!info->extra_drop_info) {
			throw InternalException("DROP TRIGGER: missing extra_drop_info (expected ExtraDropTriggerInfo)");
		}
		auto &trigger_extra = info->extra_drop_info->Cast<ExtraDropTriggerInfo>();
		if (!trigger_extra.base_table) {
			throw InternalException("DROP TRIGGER: ExtraDropTriggerInfo has no base_table");
		}
		auto &base_table_ref = trigger_extra.base_table->Cast<BaseTableRef>();
		auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context.client, base_table_ref.GetQualifiedName());
		auto transaction = table_entry.ParentCatalog().GetCatalogTransaction(context.client);
		if (!table_entry.DropTrigger(transaction, info->GetQualifiedName().Name(), info->cascade)) {
			if (info->if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw CatalogException("Trigger with name %s does not exist on table %s",
				                       info->GetQualifiedName().Name(), base_table_ref.Table());
			}
		}
		break;
	}
	default: {
		// the catalog may be empty -> the default catalog (e.g. for an unresolved DROP ... IF EXISTS of a missing
		// entry)
		auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
		catalog.DropEntry(context.client, *info);
		break;
	}
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
