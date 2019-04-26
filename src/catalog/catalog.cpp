#include "catalog/catalog.hpp"

#include "catalog/catalog_entry/list.hpp"
#include "common/exception.hpp"
#include "parser/expression/function_expression.hpp"
#include "storage/storage_manager.hpp"

using namespace duckdb;
using namespace std;

Catalog::Catalog(StorageManager &storage) : storage(storage) {
}

void Catalog::CreateSchema(Transaction &transaction, CreateSchemaInformation *info) {
	auto entry = make_unique_base<CatalogEntry, SchemaCatalogEntry>(this, info->schema);
	if (!schemas.CreateEntry(transaction, info->schema, move(entry))) {
		if (!info->if_not_exists) {
			throw CatalogException("Schema with name %s already exists!", info->schema.c_str());
		}
	}
}

void Catalog::DropSchema(Transaction &transaction, DropSchemaInformation *info) {
	if (!schemas.DropEntry(transaction, info->schema, info->cascade)) {
		if (!info->if_exists) {
			throw CatalogException("Schema with name \"%s\" does not exist!", info->schema.c_str());
		}
	}
}

SchemaCatalogEntry *Catalog::GetSchema(Transaction &transaction, const string &schema_name) {
	auto entry = schemas.GetEntry(transaction, schema_name);
	if (!entry) {
		throw CatalogException("Schema with name %s does not exist!", schema_name.c_str());
	}
	return (SchemaCatalogEntry *)entry;
}

void Catalog::CreateTable(Transaction &transaction, CreateTableInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->CreateTable(transaction, info);
}

void Catalog::CreateView(Transaction &transaction, CreateViewInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->CreateView(transaction, info);
}

void Catalog::DropView(Transaction &transaction, DropViewInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->DropView(transaction, info);
}

void Catalog::DropTable(Transaction &transaction, DropTableInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->DropTable(transaction, info);
}

void Catalog::CreateSequence(Transaction &transaction, CreateSequenceInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->CreateSequence(transaction, info);
}

void Catalog::DropSequence(Transaction &transaction, DropSequenceInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->DropSequence(transaction, info);
}

void Catalog::AlterTable(Transaction &transaction, AlterTableInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->AlterTable(transaction, info);
}

TableCatalogEntry *Catalog::GetTable(Transaction &transaction, const string &schema_name, const string &table_name) {
	auto table = GetTableOrView(transaction, schema_name, table_name);
	if (table->type != CatalogType::TABLE) {
		throw CatalogException("%s is not a table", table_name.c_str());
	}
	return (TableCatalogEntry *)table;
}

CatalogEntry *Catalog::GetTableOrView(Transaction &transaction, const string &schema_name, const string &table_name) {
	auto schema = GetSchema(transaction, schema_name);
	return schema->GetTableOrView(transaction, table_name);
}

SequenceCatalogEntry *Catalog::GetSequence(Transaction &transaction, const string &schema_name,
                                           const string &sequence) {
	auto schema = GetSchema(transaction, schema_name);
	return schema->GetSequence(transaction, sequence);
}

void Catalog::CreateTableFunction(Transaction &transaction, CreateTableFunctionInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->CreateTableFunction(transaction, info);
}

TableFunctionCatalogEntry *Catalog::GetTableFunction(Transaction &transaction, FunctionExpression *expression) {
	auto schema = GetSchema(transaction, expression->schema);
	return schema->GetTableFunction(transaction, expression);
}

void Catalog::CreateScalarFunction(Transaction &transaction, CreateScalarFunctionInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->CreateScalarFunction(transaction, info);
}

ScalarFunctionCatalogEntry *Catalog::GetScalarFunction(Transaction &transaction, const string &schema_name,
                                                       const string &name) {
	auto schema = GetSchema(transaction, schema_name);
	return schema->GetScalarFunction(transaction, name);
}

void Catalog::DropIndex(Transaction &transaction, DropIndexInformation *info) {
	auto schema = GetSchema(transaction, info->schema);
	schema->DropIndex(transaction, info);
}
