//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/duck_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"

namespace duckdb {

class CommitDropState;

struct AddConstraintInfo;
struct CreateTriggerInfo;

//! A table catalog entry
class DuckTableEntry : public TableCatalogEntry {
public:
	//! Create a TableCatalogEntry and initialize storage for it
	DuckTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, BoundCreateTableInfo &info,
	               shared_ptr<DataTable> inherited_storage = nullptr,
	               shared_ptr<CatalogSet> inherited_triggers = nullptr);

public:
	const ColumnList &GetColumns() const override;

	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;
	unique_ptr<CatalogEntry> AlterEntry(CatalogTransaction, AlterInfo &info) override;
	void UndoAlter(ClientContext &context, AlterInfo &info) override;
	void Rollback(CatalogEntry &prev_entry) override;
	void OnDrop() override;

	//! Returns the underlying storage of the table
	DataTable &GetStorage() override;

	//! Get statistics of a column (physical or virtual) within the table
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, const StorageIndex &storage_index);
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	unique_ptr<BlockingSample> GetSample() override;

	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;

	void SetAsRoot() override;

	void CommitAlter(string &column_name, CommitDropState &drop_state);
	void CommitDrop(CommitDropState &drop_state);

	//! Returns the backing index OIDs of UNIQUE constraints that are not in prev_table.
	vector<idx_t> GetAddedUniqueIndexOids(const DuckTableEntry &prev_table) const;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	vector<ColumnSegmentInfo>
	GetColumnSegmentInfo(const QueryContext &context,
	                     const ColumnSegmentInfoScanOptions &options = ColumnSegmentInfoScanOptions {}) override;
	void InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state) override;
	bool ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
	                           vector<ColumnSegmentInfo> &result) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	bool IsDuckTable() const override {
		return true;
	}
	optional_ptr<DuckTableEntry> TryGetDuckTableEntry() override {
		return this;
	}

	//! Returns the virtual columns for this table
	virtual_column_map_t GetVirtualColumns() const override;

	optional_ptr<CatalogEntry> CreateTrigger(CatalogTransaction transaction, CreateTriggerInfo &info) override;
	optional_ptr<CatalogEntry> GetTrigger(CatalogTransaction transaction, const Identifier &name) const override;
	void ScanTriggers(CatalogTransaction transaction,
	                  const std::function<void(CatalogEntry &)> &callback) const override;
	//! Scan the triggers visible at the bound (used by checkpoint writer)
	void ScanTriggers(VisibilityBound bound, const std::function<void(CatalogEntry &)> &callback);
	//! Drop a trigger by name
	bool DropTrigger(CatalogTransaction transaction, const Identifier &name, bool cascade) override;

private:
	unique_ptr<CatalogEntry> RenameColumn(ClientContext &context, RenameColumnInfo &info);
	unique_ptr<CatalogEntry> RenameField(ClientContext &context, RenameFieldInfo &info);
	unique_ptr<CatalogEntry> AddColumn(ClientContext &context, AddColumnInfo &info);
	unique_ptr<CatalogEntry> AddField(ClientContext &context, AddFieldInfo &info);
	unique_ptr<CatalogEntry> RemoveColumn(ClientContext &context, RemoveColumnInfo &info);
	unique_ptr<CatalogEntry> RemoveField(ClientContext &context, RemoveFieldInfo &info);
	unique_ptr<CatalogEntry> SetDefault(ClientContext &context, SetDefaultInfo &info);
	unique_ptr<CatalogEntry> ChangeColumnType(ClientContext &context, ChangeColumnTypeInfo &info,
	                                          AlterTableType alter_table_type);
	unique_ptr<CatalogEntry> SetNotNull(ClientContext &context, SetNotNullInfo &info);
	unique_ptr<CatalogEntry> DropNotNull(ClientContext &context, DropNotNullInfo &info);
	unique_ptr<CatalogEntry> AddForeignKeyConstraint(AlterForeignKeyInfo &info);
	unique_ptr<CatalogEntry> DropForeignKeyConstraint(ClientContext &context, AlterForeignKeyInfo &info);
	unique_ptr<CatalogEntry> SetColumnComment(ClientContext &context, SetColumnCommentInfo &info);
	unique_ptr<CatalogEntry> AddConstraint(ClientContext &context, AddConstraintInfo &info);

	void UpdateConstraintsOnColumnDrop(const LogicalIndex &removed_index, const vector<LogicalIndex> &adjusted_indices,
	                                   const RemoveColumnInfo &info, CreateTableInfo &create_info,
	                                   const vector<unique_ptr<BoundConstraint>> &bound_constraints, bool is_generated);

protected:
	//! A list of columns that are part of this table
	ColumnList columns;

private:
	//! A reference to the underlying storage unit used for this table
	shared_ptr<DataTable> storage;
	//! The catalog set holding triggers for this table
	shared_ptr<CatalogSet> triggers;
	//! Manages dependencies of the individual columns of the table
	ColumnDependencyManager column_dependency_manager;
};
} // namespace duckdb
