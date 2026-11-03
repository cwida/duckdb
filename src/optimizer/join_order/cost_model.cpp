#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"

namespace duckdb {

CostModel::CostModel(CardinalityEstimator &cardinality_estimator) : cardinality_estimator(cardinality_estimator) {
}

CardinalityEstimator &CostModel::GetCardinalityEstimator() {
	return cardinality_estimator;
}

double CostModel::ComputeCost(const DPJoinNode &left, const DPJoinNode &right,
                              optional_ptr<JoinOrderOperator> join_operator, double output_rows) {
	auto input_cost =
	    join_operator && join_operator->type == JoinOrderOperatorType::LEFT ? right.cardinality.GetDouble() : 0;
	return left.cost + right.cost + input_cost + output_rows;
}

} // namespace duckdb
