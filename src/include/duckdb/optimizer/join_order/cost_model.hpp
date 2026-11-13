//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/cost_model.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"

namespace duckdb {

class RelationManager;

struct JoinPlanCost {
	double work;
	double memory_excess;

	bool operator<(const JoinPlanCost &other) const {
		return memory_excess != other.memory_excess ? memory_excess < other.memory_excess : work < other.work;
	}
	bool operator==(const JoinPlanCost &other) const {
		return memory_excess == other.memory_excess && work == other.work;
	}
};

class CostModel {
public:
	explicit CostModel(CardinalityEstimator &cardinality_estimator);

public:
	//! Compute work for the selected operator and its estimated output.
	double ComputeCost(const DPJoinNode &left, const DPJoinNode &right, optional_ptr<JoinOrderOperator> join_operator,
	                   double output_rows);
	CardinalityEstimator &GetCardinalityEstimator();
	void InitializeRecursiveMemory(ClientContext &context, RelationManager &relations);
	void UpdateBuildMemory(DPJoinNode &result, const DPJoinNode &left, const DPJoinNode &right);
	JoinPlanCost GetCost(const DPJoinNode &node) const;

private:
	double GetBuildSize(const DPJoinNode &node);

private:
	CardinalityEstimator &cardinality_estimator;
	optional_ptr<RelationManager> recursive_relations;
	reference_map_t<JoinRelationSet, double> row_widths;
	double memory_budget = 0;
};

} // namespace duckdb
