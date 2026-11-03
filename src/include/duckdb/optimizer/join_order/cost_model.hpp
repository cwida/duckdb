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

class CostModel {
public:
	explicit CostModel(CardinalityEstimator &cardinality_estimator);

public:
	//! Compute work for the selected operator and its estimated output.
	double ComputeCost(const DPJoinNode &left, const DPJoinNode &right, optional_ptr<JoinOrderOperator> join_operator,
	                   double output_rows);
	CardinalityEstimator &GetCardinalityEstimator();

private:
	CardinalityEstimator &cardinality_estimator;
};

} // namespace duckdb
