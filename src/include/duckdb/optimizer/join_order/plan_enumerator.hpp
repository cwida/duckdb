//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/plan_enumerator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/query_graph.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

#include <functional>

namespace duckdb {

class QueryGraphManager;

class PlanEnumerator {
public:
	explicit PlanEnumerator(QueryGraphManager &query_graph_manager, CostModel &cost_model,
	                        const QueryGraphEdges &query_graph);

public:
	//! Perform the join order solving
	bool SolveJoinOrder();
	void InitLeafPlans();

	const reference_map_t<JoinRelationSet, unique_ptr<DPJoinNode>> &GetPlans() const;

private:
	unique_ptr<DPJoinNode> CreateJoinTree(JoinRelationSet &set,
	                                      const vector<reference<NeighborInfo>> &possible_connections, DPJoinNode &left,
	                                      DPJoinNode &right);

	//! Emit a pair as a potential join candidate. Returns the best plan found for the (left, right) connection (either
	//! the newly created plan, or an existing plan). Returns nullptr if the partition violates a non-inner join's
	//! semantic input sides.
	optional_ptr<DPJoinNode> EmitPair(JoinRelationSet &left, JoinRelationSet &right,
	                                  const vector<reference<NeighborInfo>> &info);
	//! Tries to emit a potential join candidate pair. Returns false if too many pairs have already been emitted,
	//! cancelling the dynamic programming step.
	bool TryEmitPair(JoinRelationSet &left, JoinRelationSet &right, const vector<reference<NeighborInfo>> &info);
	const vector<reference<NeighborInfo>> &GetConnections(JoinRelationSet &left, JoinRelationSet &right);
	const vector<reference<JoinRelationSet>> &GetAllNeighborRelationSets(vector<RelationIndex> neighbors);

	//! Estimated cardinality of the recursive frontier when planning inside a recursive member
	double recursive_frontier_estimate = 0;

	bool EnumerateCmpRecursive(JoinRelationSet &left, JoinRelationSet &right,
	                           unordered_set<RelationIndex> &exclusion_set);
	//! Emit a relation set node
	bool EmitCSG(JoinRelationSet &node);
	//! Enumerate the possible connected subgraphs that can be joined together in the join graph
	bool EnumerateCSGRecursive(JoinRelationSet &node, unordered_set<RelationIndex> &exclusion_set);
	//! Solve the join order exactly using dynamic programming. Returns true if it was completed successfully (i.e. did
	//! not time-out)
	bool SolveJoinOrderExactly();
	//! Solve the join order approximately using a greedy algorithm
	bool SolveJoinOrderApproximately();
	bool PlanUsesCrossProduct(const DPJoinNode &node) const;
	bool HasCompletePlan() const;
	bool ActivateRequiredCrossProducts();

private:
	//! The set of edges used in the join optimizer
	QueryGraphEdges const &query_graph;
	//! The total amount of join pairs that have been considered
	idx_t pairs = 0;
	//! Grant access to the set manager and the relation manager
	QueryGraphManager &query_graph_manager;
	//! Cost model to evaluate cost of joins
	CostModel &cost_model;
	//! A map to store the optimal join plan found for a specific JoinRelationSet*
	reference_map_t<JoinRelationSet, unique_ptr<DPJoinNode>> plans;
	reference_map_t<JoinRelationSet, reference_map_t<JoinRelationSet, vector<reference<NeighborInfo>>>>
	    connection_cache;
	unordered_map<idx_t, vector<reference<JoinRelationSet>>> neighbor_set_cache;

	unordered_set<string> join_nodes_in_full_plan;
};

} // namespace duckdb
