#include "duckdb/optimizer/deliminator.hpp"

#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter.hpp"

#include <algorithm>

namespace duckdb {

Deliminator::Deliminator(ClientContext &context_p) : context(context_p) {
}

struct JoinWithDelimGet {
	JoinWithDelimGet(unique_ptr<LogicalOperator> &join_p, idx_t depth_p) : join(join_p), depth(depth_p) {
	}
	reference<unique_ptr<LogicalOperator>> join;
	idx_t depth;
};

struct DelimCandidate {
public:
	explicit DelimCandidate(unique_ptr<LogicalOperator> &op, LogicalComparisonJoin &delim_join)
	    : op(op), delim_join(delim_join), delim_get_count(0) {
	}

public:
	unique_ptr<LogicalOperator> &op;
	LogicalComparisonJoin &delim_join;
	vector<JoinWithDelimGet> joins;
	idx_t delim_get_count;
};

static bool IsEqualityJoinCondition(const JoinCondition &cond) {
	if (!cond.IsComparison()) {
		return false;
	}
	switch (cond.GetComparisonType()) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return true;
	default:
		return false;
	}
}

unique_ptr<LogicalOperator> Deliminator::Optimize(unique_ptr<LogicalOperator> op) {
	root = op;

	vector<DelimCandidate> candidates;
	FindCandidates(op, candidates);

	if (candidates.empty()) {
		return op;
	}
	for (auto &candidate : candidates) {
		auto &delim_join = candidate.delim_join;

		// Sort these so the deepest are first
		std::sort(candidate.joins.begin(), candidate.joins.end(),
		          [](const JoinWithDelimGet &lhs, const JoinWithDelimGet &rhs) { return lhs.depth > rhs.depth; });

		bool all_removed = true;
		if (!candidate.joins.empty() && HasSelection(delim_join)) {
			// Keep the deepest join with DelimGet in these cases,
			// as the selection can greatly reduce the cost of the RHS child of the DelimJoin
			candidate.joins.erase(candidate.joins.begin());
			all_removed = false;
		}

		bool all_equality_conditions = true;
		for (auto &join : candidate.joins) {
			all_removed = RemoveJoinWithDelimGet(delim_join, candidate.delim_get_count, join.join.get(),
			                                     all_equality_conditions) &&
			              all_removed;
		}

		// Change type if there are no more duplicate-eliminated columns
		if (candidate.joins.size() == candidate.delim_get_count && all_removed) {
			delim_join.type = LogicalOperatorType::LOGICAL_COMPARISON_JOIN;
			delim_join.duplicate_eliminated_columns.clear();
		}

		// Only DelimJoins are ever created as SINGLE joins,
		// and we can switch from SINGLE to LEFT if the RHS is de-duplicated by an aggr
		if (delim_join.join_type == JoinType::SINGLE) {
			TrySwitchSingleToLeft(delim_join);
		}
	}

	return op;
}

void Deliminator::FindCandidates(unique_ptr<LogicalOperator> &op, vector<DelimCandidate> &candidates) {
	for (auto &child : op->children) {
		FindCandidates(child, candidates);
	}

	if (op->type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return;
	}

	candidates.emplace_back(op, op->Cast<LogicalComparisonJoin>());
	auto &candidate = candidates.back();

	// DelimGets are in the RHS
	FindJoinWithDelimGet(op->children[1], candidate);
}

bool Deliminator::HasSelection(const LogicalOperator &op) {
	// TODO once we implement selectivity estimation using samples we need to use that here
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		for (const auto &entry : get.table_filters) {
			auto &expr_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "Deliminator::HasSelection");
			auto &expr = *expr_filter.expr;
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
			    expr.GetExpressionType() != ExpressionType::OPERATOR_IS_NOT_NULL) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_FILTER:
		return true;
	default:
		break;
	}

	for (auto &child : op.children) {
		if (HasSelection(*child)) {
			return true;
		}
	}

	return false;
}

static bool OperatorIsDelimGet(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER &&
	    op.children[0]->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}
	return false;
}

void Deliminator::FindJoinWithDelimGet(unique_ptr<LogicalOperator> &op, DelimCandidate &candidate, idx_t depth) {
	if (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		FindJoinWithDelimGet(op->children[0], candidate, depth + 1);
	} else if (op->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		candidate.delim_get_count++;
	} else {
		for (auto &child : op->children) {
			FindJoinWithDelimGet(child, candidate, depth + 1);
		}
	}

	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    (OperatorIsDelimGet(*op->children[0]) || OperatorIsDelimGet(*op->children[1]))) {
		candidate.joins.emplace_back(op, depth);
	}
}

static bool ChildJoinTypeCanBeDeliminated(JoinType &join_type) {
	switch (join_type) {
	case JoinType::INNER:
	case JoinType::SEMI:
		return true;
	default:
		return false;
	}
}

bool Deliminator::RemoveJoinWithDelimGet(LogicalComparisonJoin &delim_join, const idx_t delim_get_count,
                                         unique_ptr<LogicalOperator> &join, bool &all_equality_conditions) {
	auto &comparison_join = join->Cast<LogicalComparisonJoin>();
	if (!ChildJoinTypeCanBeDeliminated(comparison_join.join_type)) {
		return false;
	}

	// Get the index (left or right) of the DelimGet side of the join
	const idx_t delim_idx = OperatorIsDelimGet(*join->children[0]) ? 0 : 1;

	// Get the filter (if any)
	optional_ptr<LogicalFilter> filter;
	vector<unique_ptr<Expression>> filter_expressions;
	if (join->children[delim_idx]->type == LogicalOperatorType::LOGICAL_FILTER) {
		filter = &join->children[delim_idx]->Cast<LogicalFilter>();
		for (auto &expr : filter->expressions) {
			filter_expressions.emplace_back(expr->Copy());
		}
	}

	auto &delim_get = (filter ? filter->children[0] : join->children[delim_idx])->Cast<LogicalDelimGet>();
	if (comparison_join.conditions.size() != delim_get.chunk_types.size()) {
		return false; // Joining with DelimGet adds new information
	}

	// Check if joining with the DelimGet is redundant, and collect relevant column information
	ColumnBindingReplacer replacer;
	auto &replacement_bindings = replacer.replacement_bindings;
	for (auto &cond : comparison_join.conditions) {
		if (!cond.IsComparison()) {
			return false;
		}
		all_equality_conditions = all_equality_conditions && IsEqualityJoinCondition(cond);
		auto &delim_side = delim_idx == 0 ? cond.GetLHS() : cond.GetRHS();
		auto &other_side = delim_idx == 0 ? cond.GetRHS() : cond.GetLHS();
		if (delim_side.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
		    other_side.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto &delim_colref = delim_side.Cast<BoundColumnRefExpression>();
		auto &other_colref = other_side.Cast<BoundColumnRefExpression>();
		replacement_bindings.emplace_back(delim_colref.Binding(), other_colref.Binding());

		// Only add IS NOT NULL filter for regular equality/inequality comparisons
		// Do NOT add for DISTINCT FROM variants, as they handle NULL correctly
		if (cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM &&
		    cond.GetComparisonType() != ExpressionType::COMPARE_DISTINCT_FROM) {
			auto is_not_null_expr =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			is_not_null_expr->GetChildrenMutable().push_back(other_side.Copy());
			filter_expressions.push_back(std::move(is_not_null_expr));
		}
	}

	if (!all_equality_conditions &&
	    !RemoveInequalityJoinWithDelimGet(delim_join, delim_get_count, join, replacement_bindings)) {
		return false;
	}

	unique_ptr<LogicalOperator> replacement_op = std::move(comparison_join.children[1 - delim_idx]);
	if (!filter_expressions.empty()) { // Create filter if necessary
		auto new_filter = make_uniq<LogicalFilter>();
		new_filter->expressions = std::move(filter_expressions);
		new_filter->children.emplace_back(std::move(replacement_op));
		replacement_op = std::move(new_filter);
	}

	join = std::move(replacement_op);

	// TODO: Maybe go from delim join instead to save work
	replacer.VisitOperator(*root);
	return true;
}

static bool InequalityDelimJoinCanBeEliminated(JoinType &join_type) {
	return join_type == JoinType::ANTI || join_type == JoinType::MARK || join_type == JoinType::SEMI ||
	       join_type == JoinType::SINGLE;
}

bool FindAndReplaceBindings(vector<ColumnBinding> &traced_bindings, const vector<unique_ptr<Expression>> &expressions,
                            const vector<ColumnBinding> &current_bindings) {
	for (auto &binding : traced_bindings) {
		idx_t current_idx;
		for (current_idx = 0; current_idx < expressions.size(); current_idx++) {
			if (binding == current_bindings[current_idx]) {
				break;
			}
		}

		if (current_idx == expressions.size() ||
		    expressions[current_idx]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false; // Didn't find / can't deal with non-colref
		}

		auto &colref = expressions[current_idx]->Cast<BoundColumnRefExpression>();
		binding = colref.Binding();
	}
	return true;
}

bool Deliminator::RemoveInequalityJoinWithDelimGet(LogicalComparisonJoin &delim_join, const idx_t delim_get_count,
                                                   unique_ptr<LogicalOperator> &join,
                                                   const vector<ReplacementBinding> &replacement_bindings) {
	auto &comparison_join = join->Cast<LogicalComparisonJoin>();
	auto &delim_conditions = delim_join.conditions;
	const auto &join_conditions = comparison_join.conditions;
	if (delim_get_count != 1 || !InequalityDelimJoinCanBeEliminated(delim_join.join_type) ||
	    delim_conditions.size() != join_conditions.size()) {
		return false;
	}
	NotNullExpressionAnalyzer nullability(context, root);

	// TODO: we cannot perform the optimization here because our pure inequality joins don't implement
	//  JoinType::SINGLE yet, and JoinType::MARK is a special case
	if (delim_join.join_type == JoinType::SINGLE || delim_join.join_type == JoinType::MARK) {
		bool has_one_equality = false;
		for (auto &cond : join_conditions) {
			has_one_equality = has_one_equality || IsEqualityJoinCondition(cond);
		}
		if (!has_one_equality) {
			return false;
		}
	}

	// We only support colref's
	vector<ColumnBinding> traced_bindings;
	for (const auto &cond : delim_conditions) {
		if (!cond.IsComparison() || cond.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto &colref = cond.GetRHS().Cast<BoundColumnRefExpression>();
		traced_bindings.emplace_back(colref.Binding());
	}

	// Now we trace down the bindings to the join (for now, we only trace it through a few operators)
	reference<LogicalOperator> current_op = *delim_join.children[1];
	while (&current_op.get() != join.get()) {
		if (current_op.get().children.size() != 1) {
			return false;
		}

		switch (current_op.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (!FindAndReplaceBindings(traced_bindings, current_op.get().expressions,
			                            current_op.get().GetColumnBindings())) {
				return false;
			}
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			break; // Doesn't change bindings
		default:
			return false;
		}
		current_op = *current_op.get().children[0];
	}

	// Get the index (left or right) of the DelimGet side of the join
	const idx_t delim_idx = OperatorIsDelimGet(*join->children[0]) ? 0 : 1;

	vector<JoinCondition> rewritten_conditions;
	rewritten_conditions.reserve(delim_conditions.size());
	for (auto &condition : delim_conditions) {
		rewritten_conditions.push_back(condition.Copy());
	}

	bool found_all = true;
	for (idx_t cond_idx = 0; cond_idx < rewritten_conditions.size(); cond_idx++) {
		auto &delim_condition = rewritten_conditions[cond_idx];
		if (!delim_condition.IsComparison()) {
			continue;
		}
		const auto &traced_binding = traced_bindings[cond_idx];

		bool found = false;
		for (auto &join_condition : join_conditions) {
			auto &delim_side = delim_idx == 0 ? join_condition.GetLHS() : join_condition.GetRHS();
			auto &colref = delim_side.Cast<BoundColumnRefExpression>();
			if (colref.Binding() == traced_binding) {
				if (!join_condition.IsComparison()) {
					continue;
				}
				auto join_comparison = join_condition.GetComparisonType();
				auto original_join_comparison = join_condition.GetComparisonType(); // Save original for later check
				if (delim_join.join_type == JoinType::MARK &&
				    original_join_comparison == ExpressionType::COMPARE_NOTEQUAL) {
					if (!nullability.IsNotNull(*delim_join.children[0], delim_condition.GetLHS())) {
						return false;
					}
				}
				if (delim_condition.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM ||
				    delim_condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
					// We need to compare NULL values
					if (join_comparison == ExpressionType::COMPARE_EQUAL) {
						join_comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
					} else if (join_comparison == ExpressionType::COMPARE_NOTEQUAL) {
						join_comparison = ExpressionType::COMPARE_DISTINCT_FROM;
					} else if (join_comparison != ExpressionType::COMPARE_DISTINCT_FROM &&
					           join_comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
						// The optimization does not work here
						found = false;
						break;
					}
				}
				auto left_copy = delim_condition.LeftReference()->Copy();
				auto right_copy = delim_condition.RightReference()->Copy();

				rewritten_conditions[cond_idx] = JoinCondition(std::move(left_copy), std::move(right_copy),
				                                               FlipComparisonExpression(join_comparison));
				// join condition was a not equal and filtered out all NULLS.
				// DELIM JOIN need to do that for not DELIM_GET side. Easiest way is to change the
				// comparison expression type. See duckdb/duckdb#16803
				// Only convert if the ORIGINAL join had != or = (not DISTINCT FROM variants)
				if (delim_join.join_type != JoinType::MARK &&
				    original_join_comparison != ExpressionType::COMPARE_DISTINCT_FROM &&
				    original_join_comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
					auto final_comparison = rewritten_conditions[cond_idx].GetComparisonType();
					if (final_comparison == ExpressionType::COMPARE_DISTINCT_FROM) {
						final_comparison = ExpressionType::COMPARE_NOTEQUAL;
					} else if (final_comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
						final_comparison = ExpressionType::COMPARE_EQUAL;
					}
					rewritten_conditions[cond_idx] =
					    JoinCondition(rewritten_conditions[cond_idx].LeftReference()->Copy(),
					                  rewritten_conditions[cond_idx].RightReference()->Copy(), final_comparison);
				}
				found = true;
				break;
			}
		}
		found_all = found_all && found;
	}

	if (!found_all) {
		return false;
	}
	delim_join.conditions = std::move(rewritten_conditions);
	return true;
}

void Deliminator::TrySwitchSingleToLeft(LogicalComparisonJoin &delim_join) {
	D_ASSERT(delim_join.join_type == JoinType::SINGLE);

	// Collect RHS bindings
	vector<ColumnBinding> join_bindings;
	for (const auto &cond : delim_join.conditions) {
		if (!IsEqualityJoinCondition(cond)) {
			return;
		}
		if (!cond.IsComparison() || cond.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return;
		}
		auto &colref = cond.GetRHS().Cast<BoundColumnRefExpression>();
		join_bindings.emplace_back(colref.Binding());
	}

	// Now try to find an aggr in the RHS such that the join_column_bindings is a superset of the groups
	reference<LogicalOperator> current_op = *delim_join.children[1];
	while (current_op.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		if (current_op.get().children.size() != 1) {
			return;
		}

		switch (current_op.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			FindAndReplaceBindings(join_bindings, current_op.get().expressions, current_op.get().GetColumnBindings());
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			break; // Doesn't change bindings
		default:
			return;
		}
		current_op = *current_op.get().children[0];
	}

	D_ASSERT(current_op.get().type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
	const auto &aggr = current_op.get().Cast<LogicalAggregate>();
	if (!aggr.grouping_functions.empty()) {
		return;
	}
	if (aggr.grouping_sets.size() > 1) {
		// multiple grouping sets emit one row per set, so the groups do not determine a unique row
		return;
	}

	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		if (std::find(join_bindings.begin(), join_bindings.end(),
		              ColumnBinding(aggr.group_index, ProjectionIndex(group_idx))) == join_bindings.end()) {
			return;
		}
	}

	delim_join.join_type = JoinType::LEFT;
}

} // namespace duckdb
