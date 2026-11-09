#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

static void SimplifyNullSafeSemiJoinConditions(ClientContext &context, LogicalComparisonJoin &join) {
	D_ASSERT(join.join_type == JoinType::SEMI);
	NotNullExpressionAnalyzer analyzer(context);
	for (auto &cond : join.conditions) {
		if (!cond.IsComparison() || cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		// Once a MARK join is reduced to SEMI, a null-safe equality is equivalent to regular equality if either
		// join key is known not to be NULL. Regular equality unlocks the existing runtime-filter infrastructure.
		if (!analyzer.IsNotNull(*join.children[0], cond.GetLHS()) &&
		    !analyzer.IsNotNull(*join.children[1], cond.GetRHS())) {
			continue;
		}
		cond =
		    JoinCondition(cond.LeftReference()->Copy(), cond.RightReference()->Copy(), ExpressionType::COMPARE_EQUAL);
	}
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownMarkJoin(unique_ptr<LogicalOperator> op,
                                                             unordered_set<TableIndex> &left_bindings,
                                                             unordered_set<TableIndex> &right_bindings) {
	auto op_bindings = op->GetColumnBindings();
	auto &join = op->Cast<LogicalJoin>();
	auto &comp_join = op->Cast<LogicalComparisonJoin>();
	D_ASSERT(join.join_type == JoinType::MARK);
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN || op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN);

	right_bindings.insert(comp_join.mark_index);
	FilterPushdown left_pushdown(optimizer, convert_mark_joins, projection_mode);
	FilterPushdown right_pushdown(optimizer, convert_mark_joins, projection_mode);
#ifdef DEBUG
	bool simplified_mark_join = false;
#endif
	// now check the set of filters
	for (idx_t i = 0; i < filters.size(); i++) {
		auto side = JoinSide::GetJoinSide(filters[i]->bindings, left_bindings, right_bindings);
		if (side == JoinSide::LEFT) {
			// bindings match left side: push into left
			left_pushdown.filters.push_back(std::move(filters[i]));
			// erase the filter from the list of filters
			filters.erase_at(i);
			i--;
		} else if (side == JoinSide::RIGHT) {
#ifdef DEBUG
			D_ASSERT(!simplified_mark_join);
#endif
			// this filter references the marker
			// we can turn this into a SEMI join if the filter is on only the marker
			if (filters[i]->filter->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF && convert_mark_joins &&
			    comp_join.convert_mark_to_semi) {
				// filter just references the marker: turn into semi join
#ifdef DEBUG
				simplified_mark_join = true;
#endif
				join.join_type = JoinType::SEMI;
				filters.erase_at(i);
				i--;
				continue;
			}
			// if the filter is on NOT(marker) AND the join conditions are all set to "null_values_are_equal" we can
			// turn this into an ANTI join if all join conditions have null_values_are_equal=true, then the result of
			// the MARK join is always TRUE or FALSE, and never NULL this happens in the case of a correlated EXISTS
			// clause
			if (filters[i]->filter->GetExpressionType() == ExpressionType::OPERATOR_NOT) {
				auto &op_expr = filters[i]->filter->Cast<BoundOperatorExpression>();
				if (op_expr.GetChildren()[0]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					// the filter is NOT(marker), check the join conditions
					bool all_null_values_are_equal = true;
					for (auto &cond : comp_join.conditions) {
						if (!cond.IsComparison()) {
							continue;
						}
						if (cond.GetComparisonType() != ExpressionType::COMPARE_DISTINCT_FROM &&
						    cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
							all_null_values_are_equal = false;
							break;
						}
					}
					if (all_null_values_are_equal && convert_mark_joins && comp_join.convert_mark_to_semi) {
#ifdef DEBUG
						simplified_mark_join = true;
#endif
						// all null values are equal, convert to ANTI join
						join.join_type = JoinType::ANTI;
						filters.erase_at(i);
						i--;
						continue;
					}
				}
			}
		}
	}
	left_pushdown.RewriteChild(*op, 0);
	right_pushdown.RewriteChild(*op, 1);
	if (join.join_type == JoinType::SEMI) {
		SimplifyNullSafeSemiJoinConditions(GetContext(), comp_join);
	}
	return PushFinalFilters(std::move(op));
}

} // namespace duckdb
