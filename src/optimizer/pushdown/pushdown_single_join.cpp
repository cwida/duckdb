#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

unique_ptr<LogicalOperator> FilterPushdown::PushdownSingleJoin(unique_ptr<LogicalOperator> op,
                                                               unordered_set<TableIndex> &left_bindings,
                                                               unordered_set<TableIndex> &right_bindings) {
	D_ASSERT(op->Cast<LogicalJoin>().join_type == JoinType::SINGLE);
	FilterPushdown left_pushdown(optimizer, convert_mark_joins, projection_mode);
	FilterPushdown right_pushdown(optimizer, convert_mark_joins, projection_mode);
	// now check the set of filters
	for (idx_t i = 0; i < filters.size(); i++) {
		auto side = JoinSide::GetJoinSide(filters[i]->bindings, left_bindings, right_bindings);
		if (side == JoinSide::LEFT) {
			// bindings match left side: push into left
			left_pushdown.filters.push_back(std::move(filters[i]));
			// erase the filter from the list of filters
			filters.erase_at(i);
			i--;
		}
	}
	left_pushdown.RewriteChild(*op, 0);
	right_pushdown.RewriteChild(*op, 1);
	return PushFinalFilters(std::move(op));
}

} // namespace duckdb
