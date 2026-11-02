//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/scan/physical_table_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/extra_operator_info.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/execution/physical_table_scan_enum.hpp"

namespace duckdb {

//! Represents a scan of a base table
class PhysicalTableScan : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::TABLE_SCAN;

public:
	//! Table scan that immediately projects out filter columns that are unused in the remainder of the query plan
	PhysicalTableScan(PhysicalPlan &physical_plan, vector<LogicalType> types, BoundTableFunction function,
	                  unique_ptr<FunctionData> bind_data, vector<LogicalType> returned_types,
	                  vector<ColumnIndex> column_ids, vector<idx_t> projection_ids, vector<string> names,
	                  unique_ptr<TableFilterSet> table_filters, idx_t estimated_cardinality,
	                  ExtraOperatorInfo extra_info, vector<Value> parameters, virtual_column_map_t virtual_columns);

	//! The table function
	BoundTableFunction function;
	//! Bind data of the function
	unique_ptr<FunctionData> bind_data;
	//! The types of ALL columns that can be returned by the table function
	vector<LogicalType> returned_types;
	//! The column ids used within the table function
	vector<ColumnIndex> column_ids;
	//! The projected-out column ids
	vector<idx_t> projection_ids;
	//! The names of the columns
	vector<string> names;
	//! The table filters
	unique_ptr<TableFilterSet> table_filters;
	//! Currently stores info related to filters pushed down into MultiFileLists and sample rate pushed down into the
	//! table scan
	ExtraOperatorInfo extra_info;
	//! Parameters
	vector<Value> parameters;
	//! Contains a reference to dynamically generated table filters (through e.g. a join up in the tree)
	shared_ptr<DynamicTableFilterSet> dynamic_filters;
	//! Virtual columns
	virtual_column_map_t virtual_columns;

public:
	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	bool Equals(const PhysicalOperator &other) const override;

	OrderPreservationType SourceOrder() const override {
		return function.order_preservation_type;
	}

public:
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	OperatorPartitionData GetPartitionData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate,
	                                       LocalSourceState &lstate,
	                                       const OperatorPartitionInfo &partition_info) const override;

	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override;
	TableFunctionParallelism SourceParallelism() const override;

	bool SupportsPartitioning(const OperatorPartitionInfo &partition_info) const override;

	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

	void GetMetrics(ClientContext &context, GlobalSourceState &gstate_p, LocalSourceState &lstate,
	                OperatorMetrics &operator_metrics) const;

private:
	string GetFilterInfo(const TableFilterSet &filter_set) const;
};

} // namespace duckdb
