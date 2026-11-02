#include "duckdb/execution/operator/aggregate/grouped_aggregate_data.hpp"

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
	vector<LogicalType> payload_types_filters;
	vector<reference<const Expression>> filters;

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
		if (aggr.GetFilter()) {
			// aggregates with an identical filter share one payload column
			bool shared = false;
			for (auto &filter : filters) {
				if (filter.get().Equals(*aggr.GetFilter())) {
					shared = true;
					break;
				}
			}
			if (!shared) {
				filters.push_back(*aggr.GetFilter());
				filter_count++;
				payload_types_filters.push_back(aggr.GetFilter()->GetReturnType());
			}
		}
		if (!aggr.Function().HasStateCombineCallback()) {
			throw InternalException("Aggregate function %s is missing a combine method", aggr.Function().GetName());
		}
		aggregates.push_back(std::move(expr));
	}
	for (const auto &pay_filters : payload_types_filters) {
		payload_types.push_back(pay_filters);
	}
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
