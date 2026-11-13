//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_get.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/common/enums/ordinality_request_type.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/extra_operator_info.hpp"

#include "duckdb/storage/table/row_group_order_options.hpp"

namespace duckdb {
class TableCatalogEntry;
class DynamicTableFilterSet;
struct LogicalPlanSQLExportField;

//! LogicalGet represents a scan operation from a data source
class LogicalGet : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_GET;

public:
	LogicalGet(TableIndex table_index, BoundTableFunction function, unique_ptr<FunctionData> bind_data,
	           vector<LogicalType> returned_types, vector<Identifier> returned_names,
	           virtual_column_map_t virtual_columns = virtual_column_map_t());

	//! The table index in the current bind context
	TableIndex table_index;
	//! The function that is called, together with the argument types this call was bound with
	BoundTableFunction function;
	//! The bind data of the function
	unique_ptr<FunctionData> bind_data;
	//! Process-local input that cannot be reconstructed from SQL parameters
	shared_ptr<TableFunctionInfo> bind_info;
	//! The types of ALL columns that can be returned by the table function
	vector<LogicalType> returned_types;
	//! The names of ALL columns that can be returned by the table function
	vector<Identifier> names;
	//! A mapping of column index -> type/name for all virtual columns
	virtual_column_map_t virtual_columns;
	//! Columns that are used outside the scan
	vector<ProjectionIndex> projection_ids;
	//! Filters pushed down for table scan
	TableFilterSet table_filters;
	//! The set of input parameters for the table function
	vector<Value> parameters;
	//! The set of named input parameters for the table function
	named_argument_map_t named_parameters;
	//! Whether the source invocation requested an ordinality column
	OrdinalityType source_ordinality = OrdinalityType::WITHOUT_ORDINALITY;
	//! The set of named input table types for the table-in table-out function
	vector<LogicalType> input_table_types;
	//! The set of named input table names for the table-in table-out function
	vector<Identifier> input_table_names;
	//! For a table-in-out function, the set of projected input columns
	vector<column_t> projected_input;
	//! Currently stores File Filters (as strings) applied by hive partitioning/complex filter pushdown and sample rate
	//! pushed down into the table scan
	//! Stored so the can be included in explain output
	ExtraOperatorInfo extra_info;
	//! The scan consumed a projection whose source expression is no longer retained.
	bool has_pushed_projection = false;
	//! The effective AT clause the table was looked up with, retained for SQL reconstruction
	unique_ptr<BoundAtClause> at_clause;
	//! Contains a reference to dynamically generated table filters (through e.g. a join up in the tree)
	shared_ptr<DynamicTableFilterSet> dynamic_filters;
	//! Information for WITH ORDINALITY
	optional_idx ordinality_idx;
	//! Row group order options (if set)
	unique_ptr<RowGroupOrderOptions> row_group_order_options;
	//! Partition indices to scan, empty means scan all
	vector<idx_t> scan_partition_indices;

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
	//! Returns the underlying table that is being scanned, or nullptr if there is none
	optional_ptr<TableCatalogEntry> GetTable() const;
	//! Returns any column to query - preferably the cheapest column
	//! This is used when we are running e.g. a COUNT(*) and don't care about the contents of any columns in the table
	column_t GetAnyColumn() const;

	const LogicalType &GetColumnType(const ColumnIndex &column_index) const;
	const Identifier &GetColumnName(const ColumnIndex &column_index) const;

public:
	void SetColumnIds(vector<ColumnIndex> &&column_ids);
	ProjectionIndex AddColumnId(column_t column_id);
	void ClearColumnIds();
	ProjectionIndex TryGetProjectionIndex(idx_t col_idx) const;
	const vector<ColumnIndex> &GetColumnIds() const;
	vector<ColumnIndex> &GetMutableColumnIds();
	vector<ColumnBinding> GetColumnBindings() override;
	idx_t EstimateCardinality(ClientContext &context) override;
	//! Estimate cardinality from the source without using this operator's cached estimate.
	idx_t EstimateSourceCardinality(ClientContext &context);
	bool TryGetStorageIndex(const ColumnIndex &column_index, StorageIndex &out_index) const;
	void SetScanOrder(unique_ptr<RowGroupOrderOptions> options);
	void SetPartitionsToScan(vector<idx_t> partition_indices);

	vector<TableIndex> GetTableIndex() const override;
	//! Skips the serialization check in VerifyPlan
	bool SupportSerialization() const override {
		if (!function.verify_serialization) {
			return false;
		}
		if (function.HasSerializationCallbacks()) {
			return true;
		}
		return function.bind && (!bind_data || bind_data->SupportStatementCache());
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	const ColumnIndex &GetColumnIndex(ColumnBinding binding) const;
	const ColumnIndex &GetColumnIndex(ProjectionIndex proj_index) const;

protected:
	void ResolveTypes() override;

private:
	friend class LogicalWindow;
	LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
	ExportSQLSource(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path,
	                optional_ptr<const LogicalPlanSQLExportField> ordinality = nullptr);
	LogicalGet();

private:
	//! Bound column IDs
	vector<ColumnIndex> column_ids;
};
} // namespace duckdb
