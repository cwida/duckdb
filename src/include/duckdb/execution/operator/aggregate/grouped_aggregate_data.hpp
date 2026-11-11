//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/grouped_aggregate_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

class GroupedAggregateData {
public:
	GroupedAggregateData() {
	}
	//! The groups
	vector<unique_ptr<Expression>> groups;
	//! The set of GROUPING functions
	vector<vector<ProjectionIndex>> grouping_functions;
	//! The group types
	vector<LogicalType> group_types;

	//! The aggregates that have to be computed
	vector<unique_ptr<Expression>> aggregates;
	//! The payload types
	vector<LogicalType> payload_types;
	//! The aggregate return types
	vector<LogicalType> aggregate_return_types;
	//! Pointers to the aggregates
	vector<BoundAggregateExpression *> bindings;
	idx_t filter_count;

public:
	idx_t GroupCount() const;

	const vector<vector<ProjectionIndex>> &GetGroupingFunctions() const;

	void InitializeGroupby(vector<unique_ptr<Expression>> groups, vector<unique_ptr<Expression>> expressions,
	                       vector<unsafe_vector<ProjectionIndex>> grouping_functions);

	//! Initialize a GroupedAggregateData object for use with distinct aggregates
	void InitializeDistinct(const unique_ptr<Expression> &aggregate, const vector<unique_ptr<Expression>> *groups_p);

	//! Points the filters of the aggregates at payload columns following the children (one per distinct filter) and
	//! appends their types; returns the input column that each of those payload columns is read from
	static unordered_map<idx_t, idx_t> AddFilterColumns(const vector<unique_ptr<Expression>> &aggregates,
	                                                    vector<LogicalType> &payload_types);

private:
	void InitializeDistinctGroups(const vector<unique_ptr<Expression>> *groups);
	void InitializeGroupbyGroups(vector<unique_ptr<Expression>> groups);
	void SetGroupingFunctions(vector<unsafe_vector<ProjectionIndex>> &functions);
};

} // namespace duckdb
