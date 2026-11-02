#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> FilterPushdown::PushdownDistinct(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_DISTINCT);
	auto &distinct = op->Cast<LogicalDistinct>();
	if (!distinct.order_by) {
		// volatile filters stay above the DISTINCT, where they run once per distinct row
		vector<unique_ptr<Expression>> volatile_filters;
		for (idx_t i = 0; i < filters.size(); i++) {
			if (!filters[i]->filter->IsVolatile()) {
				continue;
			}
			volatile_filters.push_back(std::move(filters[i]->filter));
			filters.erase_at(i);
			i--;
		}
		op->children[0] = Rewrite(std::move(op->children[0]));
		return AddLogicalFilter(std::move(op), std::move(volatile_filters));
	}
	// no pushdown through DISTINCT ON (yet?)
	return FinishPushdown(std::move(op));
}

} // namespace duckdb
