//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/tuple_data_layout_enums.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/parser/group_by_node.hpp"

namespace duckdb {

//! LogicalAggregate represents an aggregate operation with (optional) GROUP BY
//! operator.
class LogicalAggregate : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;

public:
	LogicalAggregate(TableIndex group_index, TableIndex aggregate_index, vector<unique_ptr<Expression>> select_list);

	//! The table index for the groups of the LogicalAggregate
	TableIndex group_index;
	//! The table index for the aggregates of the LogicalAggregate
	TableIndex aggregate_index;
	//! The table index for the GROUPING function calls of the LogicalAggregate
	TableIndex groupings_index;
	//! The set of groups (optional).
	vector<unique_ptr<Expression>> groups;
	//! The set of grouping sets (optional).
	vector<GroupingSet> grouping_sets;
	//! The list of grouping function calls (optional)
	vector<unsafe_vector<ProjectionIndex>> grouping_functions;
	//! Group statistics (optional)
	vector<unique_ptr<BaseStatistics>> group_stats;
	//! Whether the inputs to all expression are non-NULL
	TupleDataValidityType distinct_validity;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	vector<ColumnBinding> GetColumnBindings() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
	idx_t EstimateCardinality(ClientContext &context) override;
	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

	const Expression &GetExpression(ColumnBinding binding) const;
	const Expression &GetGroupExpression(ProjectionIndex group_index) const;

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
