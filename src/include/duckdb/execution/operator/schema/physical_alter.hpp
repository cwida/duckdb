//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/schema/physical_alter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"

namespace duckdb {

//! PhysicalAlter represents an ALTER TABLE command
class PhysicalAlter : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::ALTER;

public:
	explicit PhysicalAlter(PhysicalPlan &physical_plan, unique_ptr<AlterInfo> info, idx_t estimated_cardinality,
	                       bool defer_column_rewrite = false)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::ALTER, {LogicalType::BOOLEAN}, estimated_cardinality),
	      info(std::move(info)), defer_column_rewrite(defer_column_rewrite) {
	}

	unique_ptr<AlterInfo> info;
	//! Leave computing the column of an ADD COLUMN or ALTER TYPE to the PhysicalAlterColumn above this operator
	bool defer_column_rewrite;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
};

} // namespace duckdb
