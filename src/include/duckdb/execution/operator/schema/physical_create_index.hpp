//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/schema/physical_create_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/execution/index/index_type.hpp"

namespace duckdb {
class TableCatalogEntry;
struct AlterTableInfo;

class DuckTableEntry;
class LogicalOperator;

//! Physical index creation operator.
class PhysicalCreateIndex : public PhysicalOperator {
public:
	// todo: make create index type
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::CREATE_INDEX;

public:
	PhysicalCreateIndex(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table,
	                    const vector<column_t> &column_ids, unique_ptr<CreateIndexInfo> info,
	                    vector<unique_ptr<Expression>> unbound_expressions, idx_t estimated_cardinality,
	                    IndexType index_type, unique_ptr<IndexBuildBindData> bind_data,
	                    unique_ptr<AlterTableInfo> alter_table_info);

	//! The table to create the index for.
	DuckTableEntry &table;
	//! The list of column IDs of the index.
	vector<column_t> storage_ids;
	//! Index creation information.
	unique_ptr<CreateIndexInfo> info;
	//! Unbound expressions of the indexed columns.
	vector<unique_ptr<Expression>> unbound_expressions;
	//! The index type to create.
	IndexType index_type;
	//! The bind data for index creation.
	unique_ptr<IndexBuildBindData> bind_data;
	//! Alter table information for adding indexes.
	unique_ptr<AlterTableInfo> alter_table_info;

	//! Types of the indexed columns.
	vector<LogicalType> indexed_column_types;
	vector<column_t> indexed_columns;
	vector<column_t> rowid_column;

public:
	//! Source interface, NOP for this operator
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		return SourceResultType::FINISHED;
	};

	bool IsSource() const override {
		return true;
	}

public:
	//! Sink interface, thread-local sink states. Contains an index for each state.
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	//! Sink interface, global sink state. Contains the global index.
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}
};
} // namespace duckdb
