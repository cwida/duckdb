#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_set.hpp"

#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/catalog/dependency_manager.hpp"

#include "duckdb/catalog/default/default_schemas.hpp"

namespace duckdb {

Catalog::Catalog(DatabaseInstance &db)
    : db(db), schemas(make_unique<CatalogSet>(*this, make_unique<DefaultSchemaGenerator>(*this))),
      dependency_manager(make_unique<DependencyManager>(*this)) {
	catalog_version = 0;
}
Catalog::~Catalog() {
}

Catalog &Catalog::GetCatalog(ClientContext &context) {
	return context.db->GetCatalog();
}

CatalogEntry *Catalog::CreateTable(ClientContext &context, BoundCreateTableInfo *info) {
	auto schema = GetSchema(context, info->base->schema);
	return CreateTable(context, schema, info);
}

CatalogEntry *Catalog::CreateTable(ClientContext &context, SchemaCatalogEntry *schema, BoundCreateTableInfo *info) {
	return schema->CreateTable(context, info);
}

CatalogEntry *Catalog::CreateView(ClientContext &context, CreateViewInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreateView(context, schema, info);
}

CatalogEntry *Catalog::CreateView(ClientContext &context, SchemaCatalogEntry *schema, CreateViewInfo *info) {
	return schema->CreateView(context, info);
}

CatalogEntry *Catalog::CreateSequence(ClientContext &context, CreateSequenceInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreateSequence(context, schema, info);
}

CatalogEntry *Catalog::CreateSequence(ClientContext &context, SchemaCatalogEntry *schema, CreateSequenceInfo *info) {
	return schema->CreateSequence(context, info);
}

CatalogEntry *Catalog::CreateTableFunction(ClientContext &context, CreateTableFunctionInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreateTableFunction(context, schema, info);
}

CatalogEntry *Catalog::CreateTableFunction(ClientContext &context, SchemaCatalogEntry *schema,
                                           CreateTableFunctionInfo *info) {
	return schema->CreateTableFunction(context, info);
}

CatalogEntry *Catalog::CreateCopyFunction(ClientContext &context, CreateCopyFunctionInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreateCopyFunction(context, schema, info);
}

CatalogEntry *Catalog::CreateCopyFunction(ClientContext &context, SchemaCatalogEntry *schema,
                                          CreateCopyFunctionInfo *info) {
	return schema->CreateCopyFunction(context, info);
}

CatalogEntry *Catalog::CreatePragmaFunction(ClientContext &context, CreatePragmaFunctionInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreatePragmaFunction(context, schema, info);
}

CatalogEntry *Catalog::CreatePragmaFunction(ClientContext &context, SchemaCatalogEntry *schema,
                                            CreatePragmaFunctionInfo *info) {
	return schema->CreatePragmaFunction(context, info);
}

CatalogEntry *Catalog::CreateFunction(ClientContext &context, CreateFunctionInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreateFunction(context, schema, info);
}

CatalogEntry *Catalog::CreateFunction(ClientContext &context, SchemaCatalogEntry *schema, CreateFunctionInfo *info) {
	return schema->CreateFunction(context, info);
}

CatalogEntry *Catalog::CreateCollation(ClientContext &context, CreateCollationInfo *info) {
	auto schema = GetSchema(context, info->schema);
	return CreateCollation(context, schema, info);
}

CatalogEntry *Catalog::CreateCollation(ClientContext &context, SchemaCatalogEntry *schema, CreateCollationInfo *info) {
	return schema->CreateCollation(context, info);
}

CatalogEntry *Catalog::CreateSchema(ClientContext &context, CreateSchemaInfo *info) {
	D_ASSERT(!info->schema.empty());
	if (info->schema == TEMP_SCHEMA) {
		throw CatalogException("Cannot create built-in schema \"%s\"", info->schema);
	}

	unordered_set<CatalogEntry *> dependencies;
	auto entry = make_unique<SchemaCatalogEntry>(this, info->schema, info->internal);
	auto result = entry.get();
	if (!schemas->CreateEntry(context, info->schema, move(entry), dependencies)) {
		if (info->on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			throw CatalogException("Schema with name %s already exists!", info->schema);
		} else {
			D_ASSERT(info->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT);
		}
		return nullptr;
	}
	return result;
}

void Catalog::DropSchema(ClientContext &context, DropInfo *info) {
	D_ASSERT(!info->name.empty());
	ModifyCatalog();
	if (!schemas->DropEntry(context, info->name, info->cascade)) {
		if (!info->if_exists) {
			throw CatalogException("Schema with name \"%s\" does not exist!", info->name);
		}
	}
}

void Catalog::DropEntry(ClientContext &context, DropInfo *info) {
	ModifyCatalog();
	if (info->type == CatalogType::SCHEMA_ENTRY) {
		// DROP SCHEMA
		DropSchema(context, info);
	} else {
		if (info->schema.empty()) {
			info->schema = DEFAULT_SCHEMA;
		}
		auto schema = GetSchema(context, info->schema);
		schema->DropEntry(context, info);
	}
}

SchemaCatalogEntry *Catalog::GetSchema(ClientContext &context, const string &schema_name,
                                       QueryErrorContext error_context) {
	D_ASSERT(!schema_name.empty());
	if (schema_name == TEMP_SCHEMA) {
		return context.temporary_objects.get();
	}
	auto entry = schemas->GetEntry(context, schema_name);
	if (!entry) {
		throw CatalogException(error_context.FormatError("Schema with name %s does not exist!", schema_name));
	}
	return (SchemaCatalogEntry *)entry;
}

void Catalog::ScanSchemas(ClientContext &context, std::function<void(CatalogEntry *)> callback) {
	// create all default schemas first
	schemas->Scan(context, [&](CatalogEntry *entry) { callback(entry); });
}

CatalogEntry *Catalog::GetEntry(ClientContext &context, CatalogType type, string schema_name, const string &name,
                                bool if_exists, QueryErrorContext error_context) {
	if (schema_name.empty()) {
		// no schema provided: check the catalog search path in order
		if (context.catalog_search_path.empty()) {
			throw InternalException("Empty catalog search path");
		}
		schema_name = DEFAULT_SCHEMA;
		for (idx_t i = 0; i < context.catalog_search_path.size(); i++) {
			auto entry = GetEntry(context, type, context.catalog_search_path[i], name, true);
			if (entry) {
				return entry;
			}
		}
	}
	auto schema = GetSchema(context, schema_name, error_context);
	return schema->GetEntry(context, type, name, if_exists, error_context);
}

template <>
TableCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists,
                                     QueryErrorContext error_context) {
	auto entry = GetEntry(context, CatalogType::TABLE_ENTRY, move(schema_name), name, if_exists);
	if (!entry) {
		return nullptr;
	}
	if (entry->type != CatalogType::TABLE_ENTRY) {
		throw CatalogException("%s is not a table", name);
	}
	return (TableCatalogEntry *)entry;
}

template <>
SequenceCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists,
                                        QueryErrorContext error_context) {
	return (SequenceCatalogEntry *)GetEntry(context, CatalogType::SEQUENCE_ENTRY, move(schema_name), name, if_exists,
	                                        error_context);
}

template <>
TableFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                             bool if_exists, QueryErrorContext error_context) {
	return (TableFunctionCatalogEntry *)GetEntry(context, CatalogType::TABLE_FUNCTION_ENTRY, move(schema_name), name,
	                                             if_exists, error_context);
}

template <>
CopyFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                            bool if_exists, QueryErrorContext error_context) {
	return (CopyFunctionCatalogEntry *)GetEntry(context, CatalogType::COPY_FUNCTION_ENTRY, move(schema_name), name,
	                                            if_exists, error_context);
}

template <>
PragmaFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                              bool if_exists, QueryErrorContext error_context) {
	return (PragmaFunctionCatalogEntry *)GetEntry(context, CatalogType::PRAGMA_FUNCTION_ENTRY, move(schema_name), name,
	                                              if_exists, error_context);
}

template <>
AggregateFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                                 bool if_exists, QueryErrorContext error_context) {
	auto entry =
	    GetEntry(context, CatalogType::AGGREGATE_FUNCTION_ENTRY, move(schema_name), name, if_exists, error_context);
	if (entry->type != CatalogType::AGGREGATE_FUNCTION_ENTRY) {
		throw CatalogException("%s is not an aggregate function", name);
	}
	return (AggregateFunctionCatalogEntry *)entry;
}

template <>
CollateCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists,
                                       QueryErrorContext error_context) {
	return (CollateCatalogEntry *)GetEntry(context, CatalogType::COLLATION_ENTRY, move(schema_name), name, if_exists,
	                                       error_context);
}

void Catalog::Alter(ClientContext &context, AlterInfo *info) {
	ModifyCatalog();
	if (info->schema.empty()) {
		auto catalog_type = info->GetCatalogType();
		// invalid schema: search the catalog search path
		info->schema = DEFAULT_SCHEMA;
		for (idx_t i = 0; i < context.catalog_search_path.size(); i++) {
			auto entry = GetEntry(context, catalog_type, context.catalog_search_path[i], info->name, true);
			if (entry) {
				// entry exists in this schema: alter there
				info->schema = context.catalog_search_path[i];
				break;
			}
		}
	}
	auto schema = GetSchema(context, info->schema);
	return schema->Alter(context, info);
}

idx_t Catalog::GetCatalogVersion() {
	return catalog_version;
}

idx_t Catalog::ModifyCatalog() {
	return catalog_version++;
}

} // namespace duckdb
