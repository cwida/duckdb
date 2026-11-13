#include "duckdb/execution/operator/aggregate/grouped_aggregate_data.hpp"

#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

idx_t GroupedAggregateData::GroupCount() const {
	return groups.size();
}

const vector<vector<ProjectionIndex>> &GroupedAggregateData::GetGroupingFunctions() const {
	return grouping_functions;
}

void GroupedAggregateData::InitializeGroupby(vector<unique_ptr<Expression>> groups,
                                             vector<unique_ptr<Expression>> expressions,
                                             vector<unsafe_vector<ProjectionIndex>> grouping_functions) {
	InitializeGroupbyGroups(std::move(groups));

	SetGroupingFunctions(grouping_functions);

	filter_count = 0;
	for (auto &expr : expressions) {
		D_ASSERT(expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
		D_ASSERT(expr->IsAggregate());
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		bindings.push_back(&aggr);

		aggregate_return_types.push_back(aggr.GetReturnType());
		for (auto &child : aggr.GetChildren()) {
			payload_types.push_back(child->GetReturnType());
		}
		if (!aggr.Function().HasStateCombineCallback()) {
			throw InternalException("Aggregate function %s is missing a combine method", aggr.Function().GetName());
		}
		aggregates.push_back(std::move(expr));
	}
}

unordered_map<idx_t, idx_t> GroupedAggregateData::AddFilterColumns(const vector<unique_ptr<Expression>> &aggregates,
                                                                   vector<LogicalType> &payload_types) {
	// identical filters reference the same input column and share one payload column
	unordered_map<idx_t, idx_t> payload_columns;
	unordered_map<idx_t, idx_t> filter_columns;
	for (auto &aggregate : aggregates) {
		auto &aggr = aggregate->Cast<BoundAggregateExpression>();
		if (!aggr.GetFilter()) {
			continue;
		}
		auto &filter = aggr.GetFilterMutable();
		auto &filter_ref = filter->Cast<BoundReferenceExpression>();
		auto entry = payload_columns.find(filter_ref.Index());
		if (entry == payload_columns.end()) {
			entry = payload_columns.emplace(filter_ref.Index(), payload_types.size()).first;
			filter_columns.emplace(payload_types.size(), filter_ref.Index());
			payload_types.push_back(filter_ref.GetReturnType());
		}
		filter = filter_ref.WithIndex(entry->second);
	}
	return filter_columns;
}

void GroupedAggregateData::InitializeDistinct(const unique_ptr<Expression> &aggregate,
                                              const vector<unique_ptr<Expression>> *groups_p) {
	auto &aggr = aggregate->Cast<BoundAggregateExpression>();
	D_ASSERT(aggr.IsDistinct());

	// Add the (empty in ungrouped case) groups of the aggregates
	InitializeDistinctGroups(groups_p);

	// bindings.push_back(&aggr);
	filter_count = 0;
	aggregate_return_types.push_back(aggr.GetReturnType());
	for (idx_t i = 0; i < aggr.GetChildren().size(); i++) {
		auto &child = aggr.GetChildren()[i];
		group_types.push_back(child->GetReturnType());
		groups.push_back(child->Copy());
		payload_types.push_back(child->GetReturnType());
		if (aggr.GetFilter()) {
			filter_count++;
		}
	}
	if (!aggr.Function().HasStateCombineCallback()) {
		throw InternalException("Aggregate function %s is missing a combine method", aggr.Function().GetName());
	}
}

void GroupedAggregateData::InitializeDistinctGroups(const vector<unique_ptr<Expression>> *groups_p) {
	if (!groups_p) {
		return;
	}
	for (auto &expr : *groups_p) {
		group_types.push_back(expr->GetReturnType());
		groups.push_back(expr->Copy());
	}
}

void GroupedAggregateData::InitializeGroupbyGroups(vector<unique_ptr<Expression>> groups) {
	// Add all the expressions of the group by clause
	for (auto &expr : groups) {
		group_types.push_back(expr->GetReturnType());
	}
	this->groups = std::move(groups);
}

void GroupedAggregateData::SetGroupingFunctions(vector<unsafe_vector<ProjectionIndex>> &functions) {
	grouping_functions.reserve(functions.size());
	for (idx_t i = 0; i < functions.size(); i++) {
		grouping_functions.push_back(std::move(functions[i]));
	}
}

} // namespace duckdb
