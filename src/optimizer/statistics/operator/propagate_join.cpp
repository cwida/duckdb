#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/storage/statistics/validity_statistics.hpp"

namespace duckdb {

void StatisticsPropagator::PropagateStatistics(LogicalComparisonJoin &join, unique_ptr<LogicalOperator> *node_ptr) {
	for (idx_t i = 0; i < join.conditions.size(); i++) {
		auto &condition = join.conditions[i];
		auto stats_left = PropagateExpression(condition.left);
		auto stats_right = PropagateExpression(condition.right);
		if (stats_left && stats_right) {
			if (condition.null_values_are_equal && stats_left->CanHaveNull() && stats_right->CanHaveNull()) {
				// null values are equal in this join, and both sides can have null values
				// nothing to do here
				continue;
			}
			auto prune_result = PropagateComparison(*stats_left, *stats_right, condition.comparison);
			// Add stats to logical_join for perfect hash join
			join.join_stats.push_back(move(stats_left));
			join.join_stats.push_back(move(stats_right));
			switch (prune_result) {
			case FilterPropagateResult::FILTER_FALSE_OR_NULL:
			case FilterPropagateResult::FILTER_ALWAYS_FALSE:
				// filter is always false or null, none of the join conditions matter
				switch (join.join_type) {
				case JoinType::SEMI:
				case JoinType::INNER:
					// semi or inner join on false; entire node can be pruned
					ReplaceWithEmptyResult(*node_ptr);
					return;
				case JoinType::ANTI:
					// anti join: replace entire join with LHS
					*node_ptr = move(join.children[0]);
					return;
				case JoinType::LEFT:
					// anti/left outer join: replace right side with empty node
					ReplaceWithEmptyResult(join.children[1]);
					return;
				case JoinType::RIGHT:
					// right outer join: replace left side with empty node
					ReplaceWithEmptyResult(join.children[0]);
					return;
				default:
					// other join types: can't do much meaningful with this information
					// full outer join requires both sides anyway; we can skip the execution of the actual join, but eh
					// mark/single join requires knowing if the rhs has null values or not
					break;
				}
				break;
			case FilterPropagateResult::FILTER_ALWAYS_TRUE:
				// filter is always true
				if (join.conditions.size() > 1) {
					// there are multiple conditions: erase this condition
					join.conditions.erase(join.conditions.begin() + i);
					i--;
					continue;
				} else {
					// this is the only condition and it is always true: all conditions are true
					switch (join.join_type) {
					case JoinType::SEMI:
						// semi join on true: replace entire join with LHS
						*node_ptr = move(join.children[0]);
						return;
					case JoinType::INNER:
					case JoinType::LEFT:
					case JoinType::RIGHT:
					case JoinType::OUTER: {
						// inner/left/right/full outer join, replace with cross product
						// since the condition is always true, left/right/outer join are equivalent to inner join here
						auto cross_product = make_unique<LogicalCrossProduct>();
						cross_product->children = move(join.children);
						*node_ptr = move(cross_product);
						return;
					}
					case JoinType::ANTI:
						// anti join on true: empty result
						ReplaceWithEmptyResult(*node_ptr);
						return;
					default:
						// we don't handle mark/single join here yet
						break;
					}
				}
				break;
			default:
				break;
			}
		}
		// after we have propagated, we can update the statistics on both sides
		// note that it is fine to do this now, even if the same column is used again later
		// e.g. if we have i=j AND i=k, and the stats for j and k are disjoint, we know there are no results
		// so if we have e.g. i: [0, 100], j: [0, 25], k: [75, 100]
		// we can set i: [0, 25] after the first comparison, and statically determine that the second comparison is fals

		// note that we can't update statistics the same for all join types
		// mark and single joins don't filter any tuples -> so there is no propagation possible
		// anti joins have inverse statistics propagation
		// (i.e. if we have an anti join on i: [0, 100] and j: [0, 25], the resulting stats are i:[25,100])
		// for now we don't handle anti joins
		if (condition.null_values_are_equal) {
			// skip update when null values are equal (for now?)
			continue;
		}
		switch (join.join_type) {
		case JoinType::INNER:
		case JoinType::SEMI: {
			UpdateFilterStatistics(*condition.left, *condition.right, condition.comparison);
			auto stats_left = PropagateExpression(condition.left);
			auto stats_right = PropagateExpression(condition.right);
			// Update join_stats when is already part of the join
			if (join.join_stats.size() == 2) {
				join.join_stats[0] = move(stats_left);
				join.join_stats[1] = move(stats_right);
			}
			break;
		}
		default:
			break;
		}
	}
}

void StatisticsPropagator::PropagateStatistics(LogicalAnyJoin &join, unique_ptr<LogicalOperator> *node_ptr) {
	// propagate the expression into the join condition
	PropagateExpression(join.condition);
}

void StatisticsPropagator::MultiplyCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats) {
	if (!stats->has_estimated_cardinality || !new_stats.has_estimated_cardinality || !stats->has_max_cardinality ||
	    !new_stats.has_max_cardinality) {
		stats = nullptr;
		return;
	}
	stats->estimated_cardinality = MaxValue<idx_t>(stats->estimated_cardinality, new_stats.estimated_cardinality);
	auto new_max = Hugeint::Multiply(stats->max_cardinality, new_stats.max_cardinality);
	if (new_max < NumericLimits<int64_t>::Maximum()) {
		int64_t result;
		if (!Hugeint::TryCast<int64_t>(new_max, result)) {
			throw InternalException("Overflow in cast in statistics propagation");
		}
		D_ASSERT(result >= 0);
		stats->max_cardinality = idx_t(result);
	} else {
		stats = nullptr;
	}
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalJoin &join,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	// first propagate through the children of the join
	node_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			node_stats = nullptr;
		} else if (node_stats) {
			MultiplyCardinalities(node_stats, *child_stats);
		}
	}

	auto join_type = join.join_type;
	vector<ColumnBinding> left_bindings, right_bindings;
	if (IsRightOuterJoin(join_type)) {
		left_bindings = join.children[0]->GetColumnBindings();
	}
	if (IsLeftOuterJoin(join_type)) {
		right_bindings = join.children[1]->GetColumnBindings();
	}

	// then propagate into the join conditions
	switch (join.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		PropagateStatistics((LogicalComparisonJoin &)join, node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		PropagateStatistics((LogicalAnyJoin &)join, node_ptr);
		break;
	default:
		break;
	}

	// now depending on the join type, we might need to alter the statistics
	// LEFT, FULL and RIGHT OUTER joins can introduce null values
	// this requires us to alter the statistics after this point in the query plan
	if (IsLeftOuterJoin(join_type)) {
		// left or full outer join: set is_null to true for all rhs statistics
		for (auto &binding : right_bindings) {
			auto stats = statistics_map.find(binding);
			if (stats != statistics_map.end()) {
				stats->second->validity_stats = make_unique<ValidityStatistics>(true);
			}
		}
	}
	if (IsRightOuterJoin(join_type)) {
		// right or full outer join: set is_null to true for all lhs statistics
		for (auto &binding : left_bindings) {
			auto stats = statistics_map.find(binding);
			if (stats != statistics_map.end()) {
				stats->second->validity_stats = make_unique<ValidityStatistics>(true);
			}
		}
	}
	return move(node_stats);
}

} // namespace duckdb
