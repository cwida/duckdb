//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/bound_constraint.hpp"

namespace duckdb {
struct CreateInfo;
class TableCatalogEntry;

class LogicalDelete : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_DELETE;

public:
	explicit LogicalDelete(TableCatalogEntry &table, TableIndex table_index);

	TableCatalogEntry &table;
	TableIndex table_index;
	bool return_chunk;
	vector<idx_t> return_columns;
	vector<unique_ptr<BoundConstraint>> bound_constraints;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	idx_t EstimateCardinality(ClientContext &context) override;
	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

protected:
	vector<ColumnBinding> GetColumnBindings() override;
	void ResolveTypes() override;

private:
	LogicalDelete(ClientContext &context, const unique_ptr<CreateInfo> &table_info);
};
} // namespace duckdb
