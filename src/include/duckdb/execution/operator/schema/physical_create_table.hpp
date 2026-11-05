//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/schema/physical_create_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! Physically CREATE TABLE statement
class PhysicalCreateTable : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::CREATE_TABLE;

public:
	PhysicalCreateTable(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
	                    unique_ptr<BoundCreateTableInfo> info, idx_t estimated_cardinality);

	//! Schema to insert to
	SchemaCatalogEntry &schema;
	//! Table name to create
	unique_ptr<BoundCreateTableInfo> info;

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}
};
} // namespace duckdb
