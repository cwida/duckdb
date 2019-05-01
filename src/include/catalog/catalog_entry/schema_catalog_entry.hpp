//===----------------------------------------------------------------------===//
//                         DuckDB
//
// catalog/catalog_entry/schema_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "catalog/catalog_entry.hpp"
#include "catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "catalog/catalog_entry/table_catalog_entry.hpp"
#include "catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "catalog/catalog_set.hpp"
#include "transaction/transaction.hpp"

namespace duckdb {
class FunctionExpression;
class Catalog;
class Constraint;

//! A schema in the catalog
class SchemaCatalogEntry : public CatalogEntry {
public:
	SchemaCatalogEntry(Catalog *catalog, string name);

	//! Returns a pointer to a table of the given name. Throws an exception if
	//! the table does not exist.
	TableCatalogEntry *GetTable(Transaction &transaction, const string &table);
	CatalogEntry *GetTableOrView(Transaction &transaction, const string &table);

	//! Creates a table with the given name in the schema
	void CreateTable(Transaction &transaction, CreateTableInformation *info);

	//! Creates a view with the given name in the schema
	void CreateView(Transaction &transaction, CreateViewInformation *info);
	//! Creates a view with the given name in the schema
	void DropView(Transaction &transaction, DropViewInformation *info);

	//! Creates a sequence with the given name in the schema
	void CreateSequence(Transaction &transaction, CreateSequenceInformation *info);
	//! Creates a sequence with the given name in the schema
	void DropSequence(Transaction &transaction, DropSequenceInformation *info);

	//! Creates an index with the given name in the schema
	bool CreateIndex(Transaction &transaction, CreateIndexInformation *info);
	//! Drops a index with the given name
	void DropIndex(Transaction &transaction, DropIndexInformation *info);
	//! Drops a table with the given name
	void DropTable(Transaction &transaction, DropTableInformation *info);

	//! Alters a table
	void AlterTable(Transaction &transaction, AlterTableInformation *info);

	//! Gets a table function matching the given function expression
	TableFunctionCatalogEntry *GetTableFunction(Transaction &transaction, FunctionExpression *expression);
	//! Create a table function within the given schema
	void CreateTableFunction(Transaction &transaction, CreateTableFunctionInformation *info);
	//! Drops a table function within the given schema
	void DropTableFunction(Transaction &transaction, DropTableFunctionInformation *info);

	//! Create a scalar function within the given schema
	void CreateScalarFunction(Transaction &transaction, CreateScalarFunctionInformation *info);

	//! Gets a scalar function with the given name
	ScalarFunctionCatalogEntry *GetScalarFunction(Transaction &transaction, const string &name);
	//! Gets the sequence with the given name
	SequenceCatalogEntry *GetSequence(Transaction &transaction, const string &name);

	//! The catalog set holding the tables
	CatalogSet tables;
	//! The catalog set holding the indexes
	CatalogSet indexes;
	//! The catalog set holding the table functions
	CatalogSet table_functions;
	//! The catalog set holding the scalar functions
	CatalogSet scalar_functions;
	//! The catalog set holding the sequences
	CatalogSet sequences;
};
} // namespace duckdb
