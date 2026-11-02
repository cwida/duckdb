//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/duck_index_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"

namespace duckdb {
class CommitDropState;
class TableCatalogEntry;

//! Wrapper class to allow copying a DuckIndexEntry (for altering the DuckIndexEntry metadata such as comments)
struct IndexDataTableInfo {
	explicit IndexDataTableInfo(shared_ptr<DataTableInfo> info_p);

	//! Pointer to the DataTableInfo
	shared_ptr<DataTableInfo> info;
};

//! A duck index entry
class DuckIndexEntry : public IndexCatalogEntry {
public:
	//! Create a DuckIndexEntry
	DuckIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
	               TableCatalogEntry &table);
	DuckIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
	               shared_ptr<IndexDataTableInfo> storage_info);

	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	void Rollback(CatalogEntry &prev_entry) override;

	//! The indexed table information
	shared_ptr<IndexDataTableInfo> info;
	//! We need the initial size of the index after the CREATE INDEX statement,
	//! as it is necessary to determine the auto checkpoint threshold
	idx_t initial_index_size;

public:
	Identifier GetSchemaName() const override;
	Identifier GetTableName() const override;

	DataTableInfo &GetDataTableInfo() const;

	//! Saves index removal into drop_state to be removed after FlushCommit().
	void CommitDrop(CommitDropState &drop_state);
};

} // namespace duckdb
