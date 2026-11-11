//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! LogicalJoin represents a join between two relations
class LogicalJoin : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_INVALID;

public:
	explicit LogicalJoin(JoinType type, LogicalOperatorType logical_type = LogicalOperatorType::LOGICAL_JOIN);

public:
	//! Gets the set of table references that are reachable from this node
	static void GetTableReferences(LogicalOperator &op, unordered_set<TableIndex> &bindings);
	static void GetExpressionBindings(const Expression &expr, unordered_set<TableIndex> &bindings);
	//! Moves the state shared by all logical joins when replacing one join operator with another
	static void MoveJoinState(LogicalJoin &source, LogicalJoin &target);
	//! Replaces a join by one of its sides while keeping the join's output layout: the side's projection map, if
	//! any, is carried by a filter without conditions so that operators above the join keep seeing the same columns
	static unique_ptr<LogicalOperator> PromoteJoinSide(unique_ptr<LogicalOperator> child,
	                                                   vector<ProjectionIndex> projection_map);

	bool HasProjectionMap() const override {
		return !left_projection_map.empty() || !right_projection_map.empty();
	}

	//! The type of the join (INNER, OUTER, etc...)
	JoinType join_type;
	//! Table index used to refer to the MARK column (in case of a MARK join)
	TableIndex mark_index {};
	//! The columns of the LHS that are output by the join
	vector<ProjectionIndex> left_projection_map;
	//! The columns of the RHS that are output by the join
	vector<ProjectionIndex> right_projection_map;

public:
	vector<ColumnBinding> GetColumnBindings() override;
	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
