#include "duckdb/optimizer/remove_redundant_distinct.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

namespace {

//! An aggregate whose output rows are exactly its groups: no aggregate functions, no GROUPING functions and a single
//! grouping set that contains all groups
bool IsDuplicateElimination(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggr = op.Cast<LogicalAggregate>();
	if (!aggr.expressions.empty() || aggr.groups.empty() || !aggr.grouping_functions.empty() ||
	    aggr.grouping_sets.size() > 1) {
		return false;
	}
	return aggr.grouping_sets.empty() || aggr.grouping_sets[0].size() == aggr.groups.size();
}

//! Replaces a duplicate elimination by a projection of its groups. The projection takes over the group index, so
//! all references to the groups stay valid
void ReplaceByProjection(unique_ptr<LogicalOperator> &op) {
	auto &aggr = op->Cast<LogicalAggregate>();
	auto projection = make_uniq<LogicalProjection>(aggr.group_index, std::move(aggr.groups));
	if (aggr.has_estimated_cardinality) {
		projection->SetEstimatedCardinality(aggr.estimated_cardinality);
	}
	projection->children.push_back(std::move(aggr.children[0]));
	projection->ResolveOperatorTypes();
	op = std::move(projection);
}

} // namespace

void RemoveRedundantDistinct::VisitOperator(unique_ptr<LogicalOperator> &op) {
	// bottom-up, so that the distinctness of an input is derived from its already reduced children
	for (auto &child : op->children) {
		VisitOperator(child);
	}
	if (!IsDuplicateElimination(*op)) {
		return;
	}
	auto &aggr = op->Cast<LogicalAggregate>();
	column_binding_set_t keys;
	for (auto &group : aggr.groups) {
		if (group->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return;
		}
		keys.insert(group->Cast<BoundColumnRefExpression>().Binding());
	}
	if (!distinctness.IsDistinctOn(*aggr.children[0], keys)) {
		return;
	}
	ReplaceByProjection(op);
}

} // namespace duckdb
