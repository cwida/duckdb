#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> FilterPushdown::PushdownMarkJoin(unique_ptr<LogicalOperator> op,
                                                             unordered_set<TableIndex> &left_bindings,
                                                             unordered_set<TableIndex> &right_bindings) {
	auto &join = op->Cast<LogicalComparisonJoin>();
	D_ASSERT(join.join_type == JoinType::MARK);
	right_bindings.insert(join.mark_index);
	FilterPushdown left_pushdown(optimizer, projection_mode);
	FilterPushdown right_pushdown(optimizer, projection_mode);
	for (idx_t i = 0; i < filters.size(); i++) {
		if (JoinSide::GetJoinSide(filters[i]->bindings, left_bindings, right_bindings) == JoinSide::LEFT) {
			left_pushdown.filters.push_back(std::move(filters[i]));
			filters.erase_at(i--);
		}
	}
	left_pushdown.RewriteChild(*op, 0);
	right_pushdown.RewriteChild(*op, 1);
	return PushFinalFilters(std::move(op));
}

} // namespace duckdb
