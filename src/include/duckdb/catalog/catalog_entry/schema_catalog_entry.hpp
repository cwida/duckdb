//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/schema_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"

namespace duckdb {
class ClientContext;

class StandardEntry;
class TableCatalogEntry;
class TableFunctionCatalogEntry;
class SequenceCatalogEntry;

enum class OnCreateConflict : uint8_t;

struct AlterTableInfo;
struct CreateIndexInfo;
struct CreateFunctionInfo;
struct CreateCollationInfo;
struct CreateCoordinateSystemInfo;
struct CreateViewInfo;
struct BoundCreateTableInfo;
struct CreatePragmaFunctionInfo;
struct CreateSequenceInfo;
struct CreateSchemaInfo;
struct CreateTableFunctionInfo;
struct CreateCopyFunctionInfo;
struct CreateTypeInfo;

struct DropInfo;

//! A schema in the catalog
class SchemaCatalogEntry : public InCatalogEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SCHEMA_ENTRY;
	static constexpr const char *Name = "schema";

public:
	SchemaCatalogEntry(Catalog &catalog, CreateSchemaInfo &info,
	                   optional_ptr<SchemaCatalogEntry> parent_schema = nullptr);

public:
	unique_ptr<CreateInfo> GetInfo() const override;

	//! The parent schema if this is a nested schema, or nullptr for a top-level schema
	virtual optional_ptr<SchemaCatalogEntry> GetParentSchema() const {
		return parent_schema;
	}

	//! The full path of this schema (its parent chain outermost first, ending with this schema's own name)
	vector<Identifier> GetSchemaPath() const;
	//! The schema path formatted as a SQL name, without the catalog.
	DUCKDB_API string GetSchemaName() const;
	//! The fully qualified name of an entry in this schema: [catalog, schema path..., entry_name]
	QualifiedName GetQualifiedName(const Identifier &entry_name) const;

	//! Scan the specified catalog set, invoking the callback method for every entry
	virtual void Scan(ClientContext &context, CatalogType type,
	                  const std::function<void(CatalogEntry &)> &callback) = 0;
	//! Scan using an existing transaction. Override when scans can run under catalog locks.
	DUCKDB_API virtual void Scan(CatalogTransaction transaction, CatalogType type,
	                             const std::function<void(CatalogEntry &)> &callback);
	//! Scan the specified catalog set, invoking the callback method for every committed entry
	virtual void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) = 0;
	//! Visit this schema and its descendants in depth-first, parent-before-child order.
	DUCKDB_API void ScanSchemaTree(CatalogTransaction transaction,
	                               const std::function<void(SchemaCatalogEntry &)> &callback);
	//! Visit the committed schema tree in the same order.
	DUCKDB_API void ScanSchemaTree(const std::function<void(SchemaCatalogEntry &)> &callback);

	string ToSQL() const override;

	//! Creates an index with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                               TableCatalogEntry &table) = 0;
	optional_ptr<CatalogEntry> CreateIndex(ClientContext &context, CreateIndexInfo &info, TableCatalogEntry &table);
	//! Create a scalar or aggregate function within the given schema
	virtual optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) = 0;
	//! Creates a table with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) = 0;
	//! Creates a view with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) = 0;
	//! Creates a sequence with the given name in the schema
	virtual optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) = 0;
	//! Create a table function within the given schema
	virtual optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                                       CreateTableFunctionInfo &info) {
		throw NotImplementedException("Table functions are not supported in schema %s", name);
	}
	//! Create a copy function within the given schema
	virtual optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                                      CreateCopyFunctionInfo &info) {
		throw NotImplementedException("Copy functions are not supported in schema %s", name);
	}
	//! Create a pragma function within the given schema
	virtual optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                        CreatePragmaFunctionInfo &info) {
		throw NotImplementedException("Pragma functions are not supported in schema %s", name);
	}
	//! Create a collation within the given schema
	virtual optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) {
		throw NotImplementedException("Collations are not supported in schema %s", name);
	}
	//! Create a coordinate system within the given schema
	virtual optional_ptr<CatalogEntry> CreateCoordinateSystem(CatalogTransaction transaction,
	                                                          CreateCoordinateSystemInfo &info) {
		throw NotImplementedException("Coordinate systems are not supported in schema %s", name);
	}

	//! Create a enum within the given schema
	virtual optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) = 0;

	//! Lookup an entry in the schema
	DUCKDB_API virtual optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
	                                                          const EntryLookupInfo &lookup_info) = 0;
	DUCKDB_API virtual CatalogSet::EntryLookup LookupEntryDetailed(CatalogTransaction transaction,
	                                                               const EntryLookupInfo &lookup_info);
	DUCKDB_API virtual SimilarCatalogEntry GetSimilarEntry(CatalogTransaction transaction,
	                                                       const EntryLookupInfo &lookup_info);

	DUCKDB_API optional_ptr<CatalogEntry> GetEntry(CatalogTransaction transaction, CatalogType type,
	                                               const Identifier &name);

	//! Drops an entry from the schema
	virtual void DropEntry(ClientContext &context, DropInfo &info) = 0;

	//! Alters a catalog entry
	virtual void Alter(CatalogTransaction transaction, AlterInfo &info) = 0;

	CatalogTransaction GetCatalogTransaction(ClientContext &context);

protected:
	optional_ptr<SchemaCatalogEntry> parent_schema;
};
} // namespace duckdb
