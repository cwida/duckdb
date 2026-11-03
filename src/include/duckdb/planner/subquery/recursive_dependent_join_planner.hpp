//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/recursive_dependent_join_planner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

class Binder;
class JoinSide;
class LogicalJoin;

/*
 * Recursively plan subqueries and flatten dependent joins from outermost to innermost (like peeling an onion).
 */
class RecursiveDependentJoinPlanner : public LogicalOperatorVisitor {
public:
	static void Plan(Binder &binder, unique_ptr<LogicalOperator> &op);

private:
	explicit RecursiveDependentJoinPlanner(Binder &binder) : binder(binder) {
	}
	static bool TryRewritePairDependentJoinCondition(Binder &binder, unique_ptr<LogicalOperator> &op,
	                                                 BindingReplacementGraph &replacements);
	static bool CanRewritePairDependentJoinCondition(LogicalOperator &op);
	static unique_ptr<LogicalOperator> PlanPairDependentLateralJoin(Binder &binder, unique_ptr<LogicalOperator> left,
	                                                                unique_ptr<LogicalOperator> right,
	                                                                unique_ptr<Expression> condition,
	                                                                const unordered_set<TableIndex> &left_bindings,
	                                                                JoinType join_type);
	BindingReplacementGraph PlanOperator(unique_ptr<LogicalOperator> &op);
	BindingReplacementGraph PlanAnyJoinCondition(unique_ptr<LogicalOperator> &op);
	BindingReplacementGraph NormalizeJoinConditionOperands(unique_ptr<LogicalOperator> &op);
	void PlanJoinChildFilters(LogicalOperator &op);
	void PlanJoinExpressions(LogicalOperator &op);
	void PlanJoinSubqueries(LogicalJoin &join, unique_ptr<Expression> &expr, JoinSide uncorrelated_side);
	unique_ptr<Expression> VisitReplace(BoundSubqueryExpression &expr, unique_ptr<Expression> *expr_ptr) override;

	unique_ptr<LogicalOperator> root;
	Binder &binder;
};
} // namespace duckdb
