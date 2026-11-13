//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/schema/physical_alter_column.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class DuckTableEntry;

//! Computes the column of an ADD COLUMN or ALTER TYPE that the PhysicalAlter below it left pending
class PhysicalAlterColumn : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::ALTER;

public:
	PhysicalAlterColumn(PhysicalPlan &physical_plan, QualifiedName table, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::ALTER, {LogicalType::BOOLEAN}, estimated_cardinality),
	      table(std::move(table)) {
	}

	//! The altered table
	QualifiedName table;

public:
	//! The new version of the altered table, if the ALTER left a column for this operator to compute
	static optional_ptr<DuckTableEntry> GetPendingTable(ClientContext &context, const QualifiedName &table);
	//! Gives up on a pending column that was not installed, so the transaction cannot commit the table without it
	static void AbortPendingColumn(ClientContext &context, DuckTableEntry &table_entry);

public:
	string GetName() const override {
		return "ALTER_COLUMN";
	}

	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}
	OrderPreservationType SourceOrder() const override {
		// the operator outputs no rows, so the result does not need to keep them in order
		return OrderPreservationType::NO_ORDER;
	}

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}
};

} // namespace duckdb
