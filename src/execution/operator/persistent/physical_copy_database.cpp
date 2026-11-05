#include "duckdb/execution/operator/persistent/physical_copy_database.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

PhysicalCopyDatabase::PhysicalCopyDatabase(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                           idx_t estimated_cardinality, unique_ptr<CopyDatabaseInfo> info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::COPY_DATABASE, std::move(types), estimated_cardinality),
      info(std::move(info_p)) {
}

PhysicalCopyDatabase::~PhysicalCopyDatabase() {
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalCopyDatabase::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                       OperatorSourceInput &input) const {
	auto &catalog = Catalog::GetCatalog(context.client, info->target_database);
	for (auto &create_info : info->entries) {
		switch (create_info->type) {
		case CatalogType::SCHEMA_ENTRY:
			catalog.CreateSchema(context.client, create_info->Cast<CreateSchemaInfo>());
			break;
		case CatalogType::VIEW_ENTRY:
			catalog.CreateView(context.client, create_info->Cast<CreateViewInfo>());
			break;
		case CatalogType::SEQUENCE_ENTRY:
			catalog.CreateSequence(context.client, create_info->Cast<CreateSequenceInfo>());
			break;
		case CatalogType::TYPE_ENTRY:
			catalog.CreateType(context.client, create_info->Cast<CreateTypeInfo>());
			break;
		case CatalogType::MACRO_ENTRY:
		case CatalogType::TABLE_MACRO_ENTRY:
			catalog.CreateFunction(context.client, create_info->Cast<CreateMacroInfo>());
			break;
		case CatalogType::TABLE_ENTRY: {
			auto binder = Binder::CreateBinder(context.client);
			auto bound_info = binder->BindCreateTableInfo(std::move(create_info));
			catalog.CreateTable(context.client, *bound_info);
			break;
		}
		case CatalogType::INDEX_ENTRY: {
			// Skip for now.
			break;
		}
		default:
			throw NotImplementedException("Entry type %s not supported in PhysicalCopyDatabase",
			                              CatalogTypeToString(create_info->type));
		}
	}

	// Create the indexes after table creation.
	for (auto &create_info : info->entries) {
		if (!create_info || create_info->type != CatalogType::INDEX_ENTRY) {
			continue;
		}
		auto index_entry = catalog.CreateIndex(context.client, create_info->Cast<CreateIndexInfo>());
		D_ASSERT(index_entry);

		auto &create_index_info = create_info->Cast<CreateIndexInfo>();
		auto &table_entry = catalog.GetEntry<TableCatalogEntry>(
		    context.client,
		    QualifiedName(catalog.GetName(), create_index_info.GetQualifiedName().Schema(), create_index_info.table));
		auto &data_table = table_entry.GetStorage();

		IndexStorageInfo storage_info(create_index_info.GetIndexName());
		storage_info.options.emplace("v1_0_0_storage", false);
		auto unbound_index = make_uniq<UnboundIndex>(create_index_info.Copy(), std::move(storage_info),
		                                             data_table.GetTableIOManager(), catalog.GetAttached());
		data_table.AddIndex(std::move(unbound_index), index_entry->oid);

		// We add unbound indexes, so we immediately bind them.
		// Otherwise, WAL serialization fails due to unbound indexes.
		auto &data_table_info = *data_table.GetDataTableInfo();
		data_table_info.BindIndexes(context.client);
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
