#include "duckdb/optimizer/distinctness.hpp"

#include "duckdb/optimizer/apply_functional_dependencies.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

bool Distinctness::IsDistinctOn(LogicalOperator &op, const column_binding_set_t &keys) {
	if (keys.empty()) {
		return false;
	}
	// a superset of a unique set is unique as well
	for (auto &unique_set : GetUniqueSets(op)) {
		bool covered = true;
		for (auto &binding : unique_set) {
			if (keys.find(binding) == keys.end()) {
				covered = false;
				break;
			}
		}
		if (covered) {
			return true;
		}
	}
	return false;
}

bool Distinctness::GetJoinEqualityKeys(const LogicalComparisonJoin &join, idx_t child_idx, column_binding_set_t &keys) {
	keys.clear();
	for (auto &cond : join.conditions) {
		// other conditions only filter the matches
		if (!cond.IsComparison()) {
			continue;
		}
		if (cond.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
		    cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		auto &key = child_idx == 0 ? cond.GetLHS() : cond.GetRHS();
		if (key.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			// e.g. a cast: several values of the column can be equal to the same value of the other side
			return false;
		}
		keys.insert(key.Cast<BoundColumnRefExpression>().Binding());
	}
	return !keys.empty();
}

bool Distinctness::IsDistinctOnJoinKeys(LogicalComparisonJoin &join, idx_t child_idx) {
	column_binding_set_t keys;
	if (!GetJoinEqualityKeys(join, child_idx, keys)) {
		return false;
	}
	return IsDistinctOn(*join.children[child_idx], keys);
}

vector<column_binding_set_t> Distinctness::GetJoinUniqueSets(LogicalComparisonJoin &join) {
	vector<column_binding_set_t> result;
	switch (join.join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
	case JoinType::SINGLE:
		// these emit every row of the left side at most once
		return GetUniqueSets(*join.children[0]);
	case JoinType::INNER:
		// a row that is joined to a side that is distinct on its join keys finds at most one match
		if (IsDistinctOnJoinKeys(join, 1)) {
			result = GetUniqueSets(*join.children[0]);
		}
		if (IsDistinctOnJoinKeys(join, 0)) {
			auto right_sets = GetUniqueSets(*join.children[1]);
			result.insert(result.end(), right_sets.begin(), right_sets.end());
		}
		return result;
	case JoinType::LEFT:
		if (IsDistinctOnJoinKeys(join, 1)) {
			result = GetUniqueSets(*join.children[0]);
		}
		return result;
	default:
		return result;
	}
}

vector<column_binding_set_t> Distinctness::GetUniqueSets(LogicalOperator &op) {
	vector<column_binding_set_t> result;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		// PRIMARY KEY / UNIQUE NOT NULL constraints of the scanned table
		return ApplyFunctionalDependencies::GetUniqueColumnCombinations(op);
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		// the groups of an aggregate with a single grouping set that contains all of them
		auto &aggr = op.Cast<LogicalAggregate>();
		if (aggr.groups.empty() || aggr.grouping_sets.size() > 1 || !aggr.grouping_functions.empty()) {
			return result;
		}
		if (!aggr.grouping_sets.empty() && aggr.grouping_sets[0].size() != aggr.groups.size()) {
			return result;
		}
		column_binding_set_t groups;
		for (auto group_idx : ProjectionIndex::GetIndexes(aggr.groups.size())) {
			groups.insert(ColumnBinding(aggr.group_index, group_idx));
		}
		result.push_back(std::move(groups));
		return result;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		// only plain column references forward a unique set: other expressions (e.g. casts) are not injective
		auto &projection = op.Cast<LogicalProjection>();
		auto child_sets = GetUniqueSets(*projection.children[0]);
		if (child_sets.empty()) {
			return result;
		}
		column_binding_map_t<ColumnBinding> forwarded;
		for (auto expr_idx : ProjectionIndex::GetIndexes(projection.expressions.size())) {
			auto &expr = *projection.expressions[expr_idx];
			if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				forwarded.emplace(expr.Cast<BoundColumnRefExpression>().Binding(),
				                  ColumnBinding(projection.table_index, expr_idx));
			}
		}
		for (auto &child_set : child_sets) {
			column_binding_set_t unique_set;
			bool complete = true;
			for (auto &binding : child_set) {
				auto entry = forwarded.find(binding);
				if (entry == forwarded.end()) {
					complete = false;
					break;
				}
				unique_set.insert(entry->second);
			}
			if (complete) {
				result.push_back(std::move(unique_set));
			}
		}
		return result;
	}
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_SAMPLE:
	case LogicalOperatorType::LOGICAL_WINDOW:
		// these remove rows or add columns, but never duplicate a row
		return GetUniqueSets(*op.children[0]);
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		return GetJoinUniqueSets(op.Cast<LogicalComparisonJoin>());
	default:
		// unknown
		return result;
	}
}

} // namespace duckdb
