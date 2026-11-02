//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/table_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/enums/column_segment_info_scan_type.hpp"
#include "duckdb/common/enums/trigger_type.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/planner/bound_constraint.hpp"
#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/catalog/catalog_entry/table_column_type.hpp"
#include "duckdb/catalog/catalog_entry/column_dependency_manager.hpp"
#include "duckdb/common/table_column.hpp"

namespace duckdb {

class DataTable;
class DuckTableEntry;
struct CreateTriggerInfo;

struct RenameColumnInfo;
struct RenameFieldInfo;
struct AddColumnInfo;
struct AddFieldInfo;
struct RemoveColumnInfo;
struct RemoveFieldInfo;
struct SetDefaultInfo;
struct ChangeColumnTypeInfo;
struct AlterForeignKeyInfo;
struct SetNotNullInfo;
struct DropNotNullInfo;
struct SetColumnCommentInfo;
struct CreateTableInfo;
struct BoundCreateTableInfo;
struct ColumnBinding;

class TableFunction;
struct FunctionData;
struct EntryLookupInfo;

class Binder;
struct ColumnSegmentInfo;
struct ColumnSegmentInfoScanState;
class TableStorageInfo;

class LogicalGet;
class LogicalProjection;
class LogicalUpdate;

//! A table catalog entry
class TableCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::TABLE_ENTRY;
	static constexpr const char *Name = "table";

public:
	//! Create a TableCatalogEntry and initialize storage for it
	DUCKDB_API TableCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

public:
	DUCKDB_API unique_ptr<CreateInfo> GetInfo() const override;

	DUCKDB_API virtual bool HasGeneratedColumns() const;

	//! Returns whether or not a column with the given name exists
	DUCKDB_API virtual bool ColumnExists(const Identifier &name) const;
	//! Returns a reference to the column of the specified name. Throws an
	//! exception if the column does not exist.
	DUCKDB_API virtual const ColumnDefinition &GetColumn(const Identifier &name) const;
	//! Returns a reference to the column of the specified logical index. Throws an
	//! exception if the column does not exist.
	DUCKDB_API virtual const ColumnDefinition &GetColumn(LogicalIndex idx) const;
	//! Returns a list of types of the table, excluding generated columns
	DUCKDB_API virtual vector<LogicalType> GetTypes() const;
	//! Returns a list of the columns of the table
	DUCKDB_API virtual const ColumnList &GetColumns() const = 0;
	//! Returns the underlying storage of the table
	virtual DataTable &GetStorage();

	//! Returns a list of the constraints of the table
	DUCKDB_API const vector<unique_ptr<Constraint>> &GetConstraints() const;
	DUCKDB_API string ToSQL() const override;

	//! Get statistics of a column (physical or virtual) within the table
	virtual unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) = 0;

	virtual unique_ptr<BlockingSample> GetSample();

	//! Returns the column index of the specified column name.
	//! If the column does not exist:
	//! If if_column_exists is true, returns DConstants::INVALID_INDEX
	//! If if_column_exists is false, throws an exception
	DUCKDB_API virtual LogicalIndex GetColumnIndex(Identifier &name, bool if_exists = false) const;
	DUCKDB_API StorageIndex GetStorageIndex(const ColumnIndex &column_index) const;

	//! Returns the scan function that can be used to scan the given table
	virtual TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) = 0;
	virtual TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                                      const EntryLookupInfo &lookup_info);

	//! Whether this table should be bound like a DuckDB table. Entries that are not a DuckTableEntry can also
	//! return true (e.g. to have DML bound with the DuckDB table layout), so use TryGetDuckTableEntry() instead of
	//! casting.
	virtual bool IsDuckTable() const {
		return false;
	}
	//! Returns this entry as a DuckTableEntry, or nullptr if it is not one
	virtual optional_ptr<DuckTableEntry> TryGetDuckTableEntry() {
		return nullptr;
	}

	DUCKDB_API static string ColumnsToSQL(const ColumnList &columns, const vector<unique_ptr<Constraint>> &constraints);

	//! Returns the expression string list of the column names e.g. (col1, col2, col3)
	static string ColumnNamesToSQL(const ColumnList &columns);

	//! Returns a list of segment information for this table, if exists
	virtual vector<ColumnSegmentInfo>
	GetColumnSegmentInfo(const QueryContext &context,
	                     const ColumnSegmentInfoScanOptions &options = ColumnSegmentInfoScanOptions {});
	//! Initialize an incremental scan over the table's column segment info.
	virtual void InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state);
	//! Append the next row group's column segment info to result. Returns false when no row groups remain.
	virtual bool ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
	                                   vector<ColumnSegmentInfo> &result);

	//! Returns the storage info of this table
	virtual TableStorageInfo GetStorageInfo(ClientContext &context) = 0;

	virtual void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                                   ClientContext &context);

	//! Returns a pointer to the table's primary key, if exists, else nullptr.
	optional_ptr<Constraint> GetPrimaryKey() const;
	//! Returns true, if the table has a primary key, else false.
	bool HasPrimaryKey() const;

	virtual LogicalType GetExpectedTypeForInsert(const ColumnDefinition &column) const;
	virtual unique_ptr<Expression> GetDefaultExpressionForColumn(ClientContext &context, const LogicalType &input_type,
	                                                             const LogicalType &result_type, ColumnBinding binding,
	                                                             const Expression &constant_value) const;

	//! Returns the virtual columns for this table
	virtual virtual_column_map_t GetVirtualColumns() const;

	virtual vector<column_t> GetRowIdColumns() const;

	//! Create a trigger on this table (throws for table types that don't support triggers)
	virtual optional_ptr<CatalogEntry> CreateTrigger(CatalogTransaction transaction, CreateTriggerInfo &info);
	//! Scan all triggers on this table (default: no-op - non-DuckDB tables have no triggers)
	virtual void ScanTriggers(CatalogTransaction transaction,
	                          const std::function<void(CatalogEntry &)> &callback) const;
	//! Get the trigger with the given name on this table
	virtual optional_ptr<CatalogEntry> GetTrigger(CatalogTransaction transaction, const Identifier &name) const;
	//! Drop a trigger from this table (throws for table types that don't support triggers)
	virtual bool DropTrigger(CatalogTransaction transaction, const Identifier &name, bool cascade);
	//! Collect triggers matching the given event type and for_each granularity, regardless of timing
	vector<const_reference<TriggerCatalogEntry>>
	GetTriggersForEvent(CatalogTransaction transaction, TriggerEventType event_type, TriggerForEach for_each) const;
	//! Collect triggers matching the given timing, event type, and for_each granularity
	vector<const_reference<TriggerCatalogEntry>> GetTriggersForEvent(CatalogTransaction transaction,
	                                                                 TriggerTiming timing, TriggerEventType event_type,
	                                                                 TriggerForEach for_each) const;

protected:
	//! A list of constraints that are part of this table
	vector<unique_ptr<Constraint>> constraints;
};
} // namespace duckdb
