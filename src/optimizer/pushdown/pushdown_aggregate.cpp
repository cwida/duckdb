#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_join.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

static bool IsVolatile(LogicalAggregate &aggr, const Expression &expr) {
	bool is_volatile = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		D_ASSERT(colref.Depth() == 0);
		if (aggr.GetExpression(colref.Binding()).IsVolatile()) {
			is_volatile = true;
		}
	});
	return is_volatile;
}

static unique_ptr<Expression> ReplaceGroupBindings(LogicalAggregate &aggr, unique_ptr<Expression> root_expr) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    root_expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &expr) {
		    D_ASSERT(colref.Depth() == 0);
		    // replace the binding with a copy to the expression at the referenced index
		    expr = aggr.GetExpression(colref.Binding()).Copy();
	    });
	return root_expr;
}

void FilterPushdown::ExtractFilterBindings(const Expression &expr, vector<ColumnBinding> &bindings) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expr, [&](const BoundColumnRefExpression &colref) { bindings.push_back(colref.Binding()); });
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownAggregate(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
	auto &aggr = op->Cast<LogicalAggregate>();

	// pushdown into AGGREGATE and GROUP BY
	// we cannot push expressions that refer to the aggregate
	FilterPushdown child_pushdown(optimizer, convert_mark_joins, projection_mode);
	for (idx_t i = 0; i < filters.size(); i++) {
		auto &f = *filters[i];
		if (f.bindings.find(aggr.aggregate_index) != f.bindings.end()) {
			// filter on aggregate: cannot pushdown
			continue;
		}
		if (f.bindings.find(aggr.groupings_index) != f.bindings.end()) {
			// filter on GROUPINGS function: cannot pushdown
			continue;
		}
		// no aggregate! we are filtering on a group
		// we can only push this down if the filter is in all grouping sets
		if (aggr.groups.empty()) {
			// empty group - we cannot pushdown the filter
			continue;
		}

		vector<ColumnBinding> bindings;
		ExtractFilterBindings(*f.filter, bindings);
		if (bindings.empty()) {
			// we can never push down empty grouping sets
			continue;
		}

		bool can_pushdown_filter = true;
		for (auto &grp : aggr.grouping_sets) {
			// check for each of the grouping sets if they contain all groups
			for (auto &binding : bindings) {
				if (grp.find(binding.column_index) == grp.end()) {
					can_pushdown_filter = false;
					break;
				}
			}
			if (!can_pushdown_filter) {
				break;
			}
		}
		if (!can_pushdown_filter) {
			continue;
		}
		if (f.filter->IsVolatile() || IsVolatile(aggr, *f.filter)) {
			// a volatile filter is evaluated once per group, not once per input row
			continue;
		}
		// no aggregate! we can push this down
		// rewrite any group bindings within the filter
		f.filter = ReplaceGroupBindings(aggr, std::move(f.filter));
		// add the filter to the child node
		if (child_pushdown.AddFilter(std::move(f.filter)) == FilterResult::UNSATISFIABLE) {
			// filter statically evaluates to false, strip tree
			return make_uniq<LogicalEmptyResult>(std::move(op));
		}
		// erase the filter from here
		filters.erase_at(i);
		i--;
	}
	child_pushdown.GenerateFilters();

	op->children[0] = child_pushdown.Rewrite(std::move(op->children[0]));
	return PushFinalFilters(std::move(op));
}

} // namespace duckdb
