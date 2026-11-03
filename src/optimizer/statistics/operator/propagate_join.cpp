#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_positional_join.hpp"

namespace duckdb {

bool StatisticsPropagator::HandleJoinNeverMatches(LogicalJoin &join, unique_ptr<LogicalOperator> &node_ptr) {
	switch (join.join_type) {
	case JoinType::RIGHT_SEMI:
	case JoinType::SEMI:
	case JoinType::INNER:
		// semi or inner join on false; entire node can be pruned
		ReplaceWithEmptyResult(node_ptr);
		return true;

		// If the filter is always false or Null, the join output is exactly one of its sides:
		// promote that side together with its projection map so the output layout stays unchanged.
	case JoinType::RIGHT_ANTI:
		node_ptr = LogicalJoin::PromoteJoinSide(std::move(join.children[1]), std::move(join.right_projection_map));
		return true;
	case JoinType::ANTI:
		node_ptr = LogicalJoin::PromoteJoinSide(std::move(join.children[0]), std::move(join.left_projection_map));
		return true;
	case JoinType::LEFT:
		// anti/left outer join: replace right side with empty node
		ReplaceWithEmptyResult(join.children[1]);
		return true;
	case JoinType::RIGHT:
		// right outer join: replace left side with empty node
		ReplaceWithEmptyResult(join.children[0]);
		return true;
	default:
		// other join types: can't do much meaningful with this information
		// full outer join requires both sides anyway; we can skip the execution of the actual join, but eh
		// mark/single join requires knowing if the rhs has null values or not
		return false;
	}
}

//! Appends the columns of a join side that the join emits to the projection map of its replacement
static void AddProjectedColumns(vector<ProjectionIndex> &result, const vector<ProjectionIndex> &projection_map,
                                idx_t column_count, idx_t offset) {
	if (projection_map.empty()) {
		for (idx_t column_idx = 0; column_idx < column_count; column_idx++) {
			result.emplace_back(offset + column_idx);
		}
		return;
	}
	for (auto &column_idx : projection_map) {
		result.emplace_back(offset + column_idx.GetIndex());
	}
}

bool StatisticsPropagator::HandleJoinAlwaysMatches(LogicalJoin &join, unique_ptr<LogicalOperator> &node_ptr) {
	switch (join.join_type) {
	case JoinType::RIGHT_SEMI:
	case JoinType::SEMI: {
		auto &emitted_projection_map =
		    join.join_type == JoinType::RIGHT_SEMI ? join.right_projection_map : join.left_projection_map;
		if (join.join_type == JoinType::RIGHT_SEMI) {
			std::swap(join.children[0], join.children[1]);
		}
		// the cross product also emits the columns of the other side: project them out again
		vector<ProjectionIndex> projection_map;
		AddProjectedColumns(projection_map, emitted_projection_map, join.children[0]->GetColumnBindings().size(), 0);
		// when the right child has no data, return an empty set
		// cannot just return the left child because if the right child has no cardinality
		// then the whole result should be empty.
		// TODO: write better CE logic for limits so that we can just look at
		//  join.children[1].estimated_cardinality.
		auto limit = make_uniq<LogicalLimit>(BoundLimitNode::ConstantValue(1), BoundLimitNode());
		limit->SetEstimatedCardinality(1);
		limit->AddChild(std::move(join.children[1]));
		auto cross_product = LogicalCrossProduct::Create(std::move(join.children[0]), std::move(limit));
		cross_product->SetEstimatedCardinality(join.estimated_cardinality);
		node_ptr = LogicalJoin::PromoteJoinSide(std::move(cross_product), std::move(projection_map));
		return true;
	}
	case JoinType::INNER: {
		// inner, replace with cross product - keeping the columns that the join emits
		vector<ProjectionIndex> projection_map;
		if (!join.left_projection_map.empty() || !join.right_projection_map.empty()) {
			auto left_count = join.children[0]->GetColumnBindings().size();
			AddProjectedColumns(projection_map, join.left_projection_map, left_count, 0);
			AddProjectedColumns(projection_map, join.right_projection_map, join.children[1]->GetColumnBindings().size(),
			                    left_count);
		}
		auto cross_product = LogicalCrossProduct::Create(std::move(join.children[0]), std::move(join.children[1]));
		cross_product->SetEstimatedCardinality(join.estimated_cardinality);
		node_ptr = LogicalJoin::PromoteJoinSide(std::move(cross_product), std::move(projection_map));
		return true;
	}
	default:
		// anti joins only produce an empty result when the other side has rows - with no rows to match against,
		// every tuple survives. left/mark/single/outer joins still emit their non-matching rows.
		return false;
	}
}

void StatisticsPropagator::PropagateStatistics(LogicalComparisonJoin &join, unique_ptr<LogicalOperator> &node_ptr) {
	for (idx_t i = 0; i < join.conditions.size(); i++) {
		auto &condition = join.conditions[i];
		if (!condition.IsComparison()) {
			PropagateExpression(condition.JoinExpressionReference());
			continue;
		}

		const auto stats_left = PropagateExpression(condition.LeftReference());
		const auto stats_right = PropagateExpression(condition.RightReference());
		if (join.join_type != JoinType::MARK) {
			// Ordinary joins reject both FALSE and NULL candidate pairs.
			condition.SetComparisonType(SimplifyDistinctComparison(condition.GetComparisonType(), stats_left.get(),
			                                                       stats_right.get(), ComparisonContext::JOIN));
		}
		if (stats_left && stats_right) {
			if ((condition.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM ||
			     condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) &&
			    stats_left->CanHaveNull() && stats_right->CanHaveNull()) {
				// null values are equal in this join, and both sides can have null values
				// nothing to do here
				continue;
			}
			auto prune_result = PropagateComparison(*stats_left, *stats_right, condition.GetComparisonType());
			// Add stats to logical_join for perfect hash join
			condition.SetLeftStats(stats_left->ToUnique());
			condition.SetRightStats(stats_right->ToUnique());
			switch (prune_result) {
			case FilterPropagateResult::FILTER_FALSE_OR_NULL:
			case FilterPropagateResult::FILTER_ALWAYS_FALSE:
				// filter is always false or null, none of the join conditions matter
				if (HandleJoinNeverMatches(join, node_ptr)) {
					return;
				}
				break;
			case FilterPropagateResult::FILTER_ALWAYS_TRUE:
				// filter is always true
				// If this is the inequality for an AsOf join,
				// then we must leave it in because it also flags
				// the semantics of restricting to a single match
				// so we can't replace it with an equi-join on the remaining conditions.
				if (join.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
					switch (condition.GetComparisonType()) {
					case ExpressionType::COMPARE_GREATERTHAN:
					case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
					case ExpressionType::COMPARE_LESSTHAN:
					case ExpressionType::COMPARE_LESSTHANOREQUALTO:
						continue;
					default:
						break;
					}
				}
				if (join.conditions.size() > 1) {
					// there are multiple conditions: erase this condition
					join.conditions.erase_at(i);
					i--;
					removed_expressions = true;
					continue;
				}
				// this is the only condition and it is always true: all conditions are true
				if (HandleJoinAlwaysMatches(join, node_ptr)) {
					removed_expressions = true;
					return;
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
		// we can set i: [0, 25] after the first comparison, and statically determine that the second comparison is
		// false

		// note that we can't update statistics the same for all join types
		// mark and single joins don't filter any tuples -> so there is no propagation possible
		// anti joins have inverse statistics propagation
		// (i.e. if we have an anti join on i: [0, 100] and j: [0, 25], the resulting stats are i:[25,100])
		// for now we don't handle anti joins
		if (condition.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM ||
		    condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			// skip update when null values are equal (for now?)
			continue;
		}
		switch (join.join_type) {
		case JoinType::INNER:
		case JoinType::SEMI:
		case JoinType::RIGHT_SEMI: {
			UpdateFilterStatistics(condition.GetLHS(), condition.GetRHS(), condition.GetComparisonType());
			auto updated_stats_left = PropagateExpression(condition.LeftReference());
			auto updated_stats_right = PropagateExpression(condition.RightReference());

			// Try to push lhs stats down rhs and vice versa
			if (stats_left && stats_right && updated_stats_left && updated_stats_right &&
			    condition.GetLHS().GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
			    condition.GetRHS().GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				CreateFilterFromJoinStats(join.children[0], condition.LeftReference(), *stats_left,
				                          *updated_stats_left);
				CreateFilterFromJoinStats(join.children[1], condition.RightReference(), *stats_right,
				                          *updated_stats_right);

				// Update join_stats when is already part of the join
				condition.SetLeftStats(std::move(updated_stats_left));
				condition.SetRightStats(std::move(updated_stats_right));
			}
			break;
		}
		default:
			break;
		}
	}

	// join conditions can be evaluated over candidate pairs that repeat rows of either side,
	// so their verification statistics must not carry additive totals
	for (auto &condition : join.conditions) {
		if (condition.IsComparison()) {
			ResetAdditiveVerificationStats(condition.GetLHS());
			ResetAdditiveVerificationStats(condition.GetRHS());
		} else {
			ResetAdditiveVerificationStats(condition.GetJoinExpression());
		}
	}
}

void StatisticsPropagator::PropagateStatistics(LogicalAnyJoin &join, unique_ptr<LogicalOperator> &node_ptr) {
	// propagate the expression into the join condition
	// note that a condition that is TRUE_OR_NULL does not always match: a NULL condition rejects the pair
	PropagateExpression(join.condition);
	// the condition is evaluated over candidate pairs that repeat rows of either side,
	// so its verification statistics must not carry additive totals
	ResetAdditiveVerificationStats(*join.condition);
	switch (ClassifyFilter(*join.condition)) {
	case FilterPropagateResult::FILTER_ALWAYS_TRUE:
		HandleJoinAlwaysMatches(join, node_ptr);
		break;
	case FilterPropagateResult::FILTER_ALWAYS_FALSE:
	case FilterPropagateResult::FILTER_FALSE_OR_NULL:
		HandleJoinNeverMatches(join, node_ptr);
		break;
	default:
		break;
	}
}

void StatisticsPropagator::MultiplyCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats) {
	if (!stats->has_estimated_cardinality || !new_stats.has_estimated_cardinality || !stats->has_max_cardinality ||
	    !new_stats.has_max_cardinality) {
		stats = nullptr;
		return;
	}
	stats->estimated_cardinality = MaxValue<idx_t>(stats->estimated_cardinality, new_stats.estimated_cardinality);
	hugeint_t new_max;
	if (!Hugeint::TryMultiply(Hugeint::Convert(stats->max_cardinality), Hugeint::Convert(new_stats.max_cardinality),
	                          new_max)) {
		stats = nullptr;
		return;
	}
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
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
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
	// depending on the join type, we might need to alter the statistics
	// LEFT, FULL, RIGHT OUTER and SINGLE joins can introduce null values
	// this requires us to alter the statistics after this point in the query plan
	bool adds_null_on_left = IsRightOuterJoin(join_type);
	bool adds_null_on_right = IsLeftOuterJoin(join_type) || join_type == JoinType::SINGLE;

	vector<ColumnBinding> left_bindings, right_bindings;
	if (adds_null_on_left) {
		left_bindings = join.children[0]->GetColumnBindings();
	}
	if (adds_null_on_right) {
		right_bindings = join.children[1]->GetColumnBindings();
	}

	// joins that can emit a row of a side multiple times invalidate that side's additive statistics
	bool duplicates_left = join_type == JoinType::INNER || join_type == JoinType::LEFT ||
	                       join_type == JoinType::RIGHT || join_type == JoinType::OUTER;
	bool duplicates_right = duplicates_left || join_type == JoinType::SINGLE;
	if (duplicates_left) {
		ResetAdditiveStatistics(join.children[0]->GetColumnBindings());
	}
	if (duplicates_right) {
		ResetAdditiveStatistics(join.children[1]->GetColumnBindings());
	}

	// then propagate into the join conditions
	switch (join.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		PropagateStatistics(join.Cast<LogicalComparisonJoin>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		PropagateStatistics(join.Cast<LogicalAnyJoin>(), node_ptr);
		break;
	default:
		break;
	}

	if (adds_null_on_right) {
		// left or full outer join: set IsNull() to true for all rhs statistics
		for (auto &binding : right_bindings) {
			auto stats = statistics_map.find(binding);
			if (stats != statistics_map.end()) {
				stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			}
		}
	}
	if (adds_null_on_left) {
		// right or full outer join: set IsNull() to true for all lhs statistics
		for (auto &binding : left_bindings) {
			auto stats = statistics_map.find(binding);
			if (stats != statistics_map.end()) {
				stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
			}
		}
	}
	return std::move(node_stats);
}

static void MaxCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats) {
	if (!stats->has_estimated_cardinality || !new_stats.has_estimated_cardinality || !stats->has_max_cardinality ||
	    !new_stats.has_max_cardinality) {
		stats = nullptr;
		return;
	}
	stats->estimated_cardinality = MaxValue<idx_t>(stats->estimated_cardinality, new_stats.estimated_cardinality);
	stats->max_cardinality = MaxValue<idx_t>(stats->max_cardinality, new_stats.max_cardinality);
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalPositionalJoin &join,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	D_ASSERT(join.type == LogicalOperatorType::LOGICAL_POSITIONAL_JOIN);

	// first propagate through the children of the join
	node_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			node_stats = nullptr;
		} else if (node_stats) {
			if (!node_stats->has_estimated_cardinality || !child_stats->has_estimated_cardinality ||
			    !node_stats->has_max_cardinality || !child_stats->has_max_cardinality) {
				node_stats = nullptr;
			} else {
				MaxCardinalities(node_stats, *child_stats);
			}
		}
	}

	// No conditions.

	// Positional Joins are always FULL OUTER

	// set IsNull() to true for all lhs statistics
	auto left_bindings = join.children[0]->GetColumnBindings();
	for (auto &binding : left_bindings) {
		auto stats = statistics_map.find(binding);
		if (stats != statistics_map.end()) {
			stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
	}

	// set IsNull() to true for all rhs statistics
	auto right_bindings = join.children[1]->GetColumnBindings();
	for (auto &binding : right_bindings) {
		auto stats = statistics_map.find(binding);
		if (stats != statistics_map.end()) {
			stats->second->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
	}

	return std::move(node_stats);
}

void StatisticsPropagator::CreateFilterFromJoinStats(unique_ptr<LogicalOperator> &child, unique_ptr<Expression> &expr,
                                                     const BaseStatistics &stats_before,
                                                     const BaseStatistics &stats_after) {
	// Defer join-derived filters until after join ordering.
	if (mode != StatisticsPropagationMode::FULL) {
		return;
	}
	// Only do this for integral colref's that have stats
	if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF || !expr->GetReturnType().IsIntegral() ||
	    !NumericStats::HasMinMax(stats_before) || !NumericStats::HasMinMax(stats_after)) {
		return;
	}

	// Retrieve min/max
	auto min_before = NumericStats::Min(stats_before);
	auto max_before = NumericStats::Max(stats_before);
	auto min_after = NumericStats::Min(stats_after);
	auto max_after = NumericStats::Max(stats_after);

	vector<unique_ptr<Expression>> filter_exprs;
	if (min_after > min_before) {
		filter_exprs.emplace_back(
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHANOREQUALTO, expr->Copy(),
		                                      make_uniq<BoundConstantExpression>(std::move(min_after))));
	}
	if (max_after < max_before) {
		filter_exprs.emplace_back(
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHANOREQUALTO, expr->Copy(),
		                                      make_uniq<BoundConstantExpression>(std::move(max_after))));
	}

	if (filter_exprs.empty()) {
		return;
	}

	auto filter = make_uniq<LogicalFilter>();
	filter->children.emplace_back(std::move(child));
	child = std::move(filter);

	for (auto &filter_expr : filter_exprs) {
		child->expressions.emplace_back(std::move(filter_expr));
	}

	// not allowed to let filter pushdown change mark joins to semi joins.
	// semi joins are potentially slower AND the conversion can ruin column binding information
	FilterPushdown filter_pushdown(optimizer, false);
	child = filter_pushdown.Rewrite(std::move(child));
	PropagateExpression(expr);
}

} // namespace duckdb
