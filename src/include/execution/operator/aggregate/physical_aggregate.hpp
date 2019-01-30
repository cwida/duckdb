//===----------------------------------------------------------------------===//
//                         DuckDB
//
// execution/operator/aggregate/physical_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "execution/physical_operator.hpp"

namespace duckdb {

//! PhysicalAggregate represents a group-by and aggregation operator. Note that
//! it is an abstract class, its implementation is not defined here.
class PhysicalAggregate : public PhysicalOperator {
public:
	PhysicalAggregate(LogicalOperator &op, vector<unique_ptr<Expression>> select_list,
	                  PhysicalOperatorType type = PhysicalOperatorType::BASE_GROUP_BY);
	PhysicalAggregate(LogicalOperator &op, vector<unique_ptr<Expression>> select_list,
	                  vector<unique_ptr<Expression>> groups,
	                  PhysicalOperatorType type = PhysicalOperatorType::BASE_GROUP_BY);

	void Initialize();

	void AcceptExpressions(SQLNodeVisitor *v) override {
		for (auto &e : select_list) {
			v->VisitExpression(&e);
		}
		for (auto &e : groups) {
			v->VisitExpression(&e);
		}
	}

	//! The projection list of the SELECT statement (that contains aggregates)
	vector<unique_ptr<Expression>> select_list;
	//! The groups
	vector<unique_ptr<Expression>> groups;
	//! The actual aggregates that have to be computed (i.e. the deepest
	//! aggregates in the expression)
	vector<AggregateExpression *> aggregates;
	bool is_implicit_aggr;
};

//! The operator state of the aggregate
class PhysicalAggregateOperatorState : public PhysicalOperatorState {
public:
	PhysicalAggregateOperatorState(PhysicalAggregate *parent, PhysicalOperator *child = nullptr,
	                               ExpressionExecutor *parent_executor = nullptr);

	//! Aggregate values, used only for aggregates without GROUP BY
	vector<Value> aggregates;
	//! Materialized GROUP BY expression
	DataChunk group_chunk;
	//! Materialized aggregates
	DataChunk aggregate_chunk;
};

} // namespace duckdb
