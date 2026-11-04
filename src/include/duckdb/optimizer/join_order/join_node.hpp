//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_node.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/join_order/cardinality_value.hpp"

namespace duckdb {

class JoinPredicate;
struct JoinOrderOperator;

class DPJoinNode {
public:
	//! Create an intermediate node in the join tree. base_cardinality = estimated_props.cardinality
	DPJoinNode(JoinRelationSet &set, optional_ptr<JoinOrderOperator> join_operator,
	           vector<reference<JoinPredicate>> predicates, bool generated_cross_product, JoinRelationSet &left,
	           JoinRelationSet &right, double cost);
	//! Create a leaf node in the join tree
	//! set cost to 0 for leaf nodes
	//! cost will be the cost to *produce* an intermediate table
	explicit DPJoinNode(JoinRelationSet &set);

public:
	//! Represents a node in the join plan
	JoinRelationSet &set;
	//! The original operator occurrence selected for this node, if any.
	optional_ptr<JoinOrderOperator> join_operator;
	//! Independently movable predicates selected for this node.
	vector<reference<JoinPredicate>> predicates;
	bool generated_cross_product;
	bool is_leaf;
	//! left and right plans
	JoinRelationSet &left_set;
	JoinRelationSet &right_set;

	//! The cost of the join node. The cost is stored here so that the cost of
	//! a join node stays in sync with how the join node is constructed. Storing the cost in an unordered_set
	//! in the cost model is error prone. If the plan enumerator join node is updated and not the cost model
	//! the whole Join Order Optimizer can start exhibiting undesired behavior.
	double cost;
	//! Candidate output used when composing parent estimates and costs.
	CardinalityValue cardinality {0};
};

} // namespace duckdb
