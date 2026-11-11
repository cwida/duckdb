//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_create_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {
class SchemaCatalogEntry;
struct CreateInfo;

class LogicalCreateTable : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_CREATE_TABLE;

public:
	LogicalCreateTable(SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info);

	//! Schema to insert to
	SchemaCatalogEntry &schema;
	//! Create Table information
	unique_ptr<BoundCreateTableInfo> info;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	idx_t EstimateCardinality(ClientContext &context) override;

protected:
	void ResolveTypes() override;

private:
	LogicalCreateTable(ClientContext &context, unique_ptr<CreateInfo> info);
};
} // namespace duckdb
