//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/query_error_context.hpp"

#include <functional>
#include "duckdb/common/atomic.hpp"

namespace duckdb {
struct CreateSchemaInfo;
struct DropInfo;
struct BoundCreateTableInfo;
struct AlterTableInfo;
struct CreateTableFunctionInfo;
struct CreateCopyFunctionInfo;
struct CreatePragmaFunctionInfo;
struct CreateFunctionInfo;
struct CreateViewInfo;
struct CreateSequenceInfo;
struct CreateCollationInfo;

class ClientContext;
class Transaction;

class AggregateFunctionCatalogEntry;
class CollateCatalogEntry;
class SchemaCatalogEntry;
class TableCatalogEntry;
class ViewCatalogEntry;
class SequenceCatalogEntry;
class TableFunctionCatalogEntry;
class CopyFunctionCatalogEntry;
class PragmaFunctionCatalogEntry;
class CatalogSet;
class DatabaseInstance;
class DependencyManager;

//! The Catalog object represents the catalog of the database.
class Catalog {
public:
	explicit Catalog(DatabaseInstance &db);
	~Catalog();

	//! Reference to the database
	DatabaseInstance &db;
	//! The catalog set holding the schemas
	unique_ptr<CatalogSet> schemas;
	//! The DependencyManager manages dependencies between different catalog objects
	unique_ptr<DependencyManager> dependency_manager;
	//! Write lock for the catalog
	mutex write_lock;

public:
	//! Get the ClientContext from the Catalog
	static Catalog &GetCatalog(ClientContext &context);
	static Catalog &GetCatalog(DatabaseInstance &db);

	DependencyManager &GetDependencyManager() {
		return *dependency_manager;
	}

	//! Returns the current version of the catalog (incremented whenever anything changes, not stored between restarts)
	idx_t GetCatalogVersion();
	//! Trigger a modification in the catalog, increasing the catalog version and returning the previous version
	idx_t ModifyCatalog();

	//! Creates a schema in the catalog.
	CatalogEntry *CreateSchema(ClientContext &context, CreateSchemaInfo *info);
	//! Creates a table in the catalog.
	CatalogEntry *CreateTable(ClientContext &context, BoundCreateTableInfo *info);
	//! Create a table function in the catalog
	CatalogEntry *CreateTableFunction(ClientContext &context, CreateTableFunctionInfo *info);
	//! Create a copy function in the catalog
	CatalogEntry *CreateCopyFunction(ClientContext &context, CreateCopyFunctionInfo *info);
	//! Create a pragma function in the catalog
	CatalogEntry *CreatePragmaFunction(ClientContext &context, CreatePragmaFunctionInfo *info);
	//! Create a scalar or aggregate function in the catalog
	CatalogEntry *CreateFunction(ClientContext &context, CreateFunctionInfo *info);
	//! Creates a table in the catalog.
	CatalogEntry *CreateView(ClientContext &context, CreateViewInfo *info);
	//! Creates a table in the catalog.
	CatalogEntry *CreateSequence(ClientContext &context, CreateSequenceInfo *info);
	//! Creates a collation in the catalog
	CatalogEntry *CreateCollation(ClientContext &context, CreateCollationInfo *info);

	//! Creates a table in the catalog.
	CatalogEntry *CreateTable(ClientContext &context, SchemaCatalogEntry *schema, BoundCreateTableInfo *info);
	//! Create a table function in the catalog
	CatalogEntry *CreateTableFunction(ClientContext &context, SchemaCatalogEntry *schema,
	                                  CreateTableFunctionInfo *info);
	//! Create a copy function in the catalog
	CatalogEntry *CreateCopyFunction(ClientContext &context, SchemaCatalogEntry *schema, CreateCopyFunctionInfo *info);
	//! Create a pragma function in the catalog
	CatalogEntry *CreatePragmaFunction(ClientContext &context, SchemaCatalogEntry *schema,
	                                   CreatePragmaFunctionInfo *info);
	//! Create a scalar or aggregate function in the catalog
	CatalogEntry *CreateFunction(ClientContext &context, SchemaCatalogEntry *schema, CreateFunctionInfo *info);
	//! Creates a table in the catalog.
	CatalogEntry *CreateView(ClientContext &context, SchemaCatalogEntry *schema, CreateViewInfo *info);
	//! Creates a table in the catalog.
	CatalogEntry *CreateSequence(ClientContext &context, SchemaCatalogEntry *schema, CreateSequenceInfo *info);
	//! Creates a collation in the catalog
	CatalogEntry *CreateCollation(ClientContext &context, SchemaCatalogEntry *schema, CreateCollationInfo *info);

	//! Drops an entry from the catalog
	void DropEntry(ClientContext &context, DropInfo *info);

	//! Returns the schema object with the specified name, or throws an exception if it does not exist
	SchemaCatalogEntry *GetSchema(ClientContext &context, const string &name = DEFAULT_SCHEMA,
	                              QueryErrorContext error_context = QueryErrorContext());
	//! Scans all the schemas in the system one-by-one, invoking the callback for each entry
	void ScanSchemas(ClientContext &context, std::function<void(CatalogEntry *)> callback);
	//! Gets the "schema.name" entry of the specified type, if if_exists=true returns nullptr if entry does not exist,
	//! otherwise an exception is thrown
	CatalogEntry *GetEntry(ClientContext &context, CatalogType type, string schema, const string &name,
	                       bool if_exists = false, QueryErrorContext error_context = QueryErrorContext());

	template <class T>
	T *GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists = false,
	            QueryErrorContext error_context = QueryErrorContext());

	//! Alter an existing entry in the catalog.
	void Alter(ClientContext &context, AlterInfo *info);

private:
	//! The catalog version, incremented whenever anything changes in the catalog
	atomic<idx_t> catalog_version;

private:
	void DropSchema(ClientContext &context, DropInfo *info);
};

template <>
TableCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists,
                                     QueryErrorContext error_context);
template <>
SequenceCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists,
                                        QueryErrorContext error_context);
template <>
TableFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                             bool if_exists, QueryErrorContext error_context);
template <>
CopyFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                            bool if_exists, QueryErrorContext error_context);
template <>
PragmaFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                              bool if_exists, QueryErrorContext error_context);
template <>
AggregateFunctionCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name,
                                                 bool if_exists, QueryErrorContext error_context);
template <>
CollateCatalogEntry *Catalog::GetEntry(ClientContext &context, string schema_name, const string &name, bool if_exists,
                                       QueryErrorContext error_context);

} // namespace duckdb
