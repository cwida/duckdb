//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_projection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! LogicalProjection represents the projection list in a SELECT clause
class LogicalProjection : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_PROJECTION;

public:
	LogicalProjection(TableIndex table_index, vector<unique_ptr<Expression>> select_list);

	TableIndex table_index;

public:
	//! Projects every column of the child unchanged
	static unique_ptr<LogicalProjection> CreateIdentity(TableIndex table_index, unique_ptr<LogicalOperator> child);

	vector<ColumnBinding> GetColumnBindings() override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

	const Expression &GetExpression(ColumnBinding binding) const;
	const Expression &GetExpression(ProjectionIndex proj_index) const;

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
