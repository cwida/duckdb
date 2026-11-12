#include "duckdb/common/operator/add.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb {

void StatisticsPropagator::AddCardinalities(unique_ptr<NodeStatistics> &stats, NodeStatistics &new_stats) {
	if (!stats) {
		return;
	}
	if (!stats->has_estimated_cardinality || !new_stats.has_estimated_cardinality || !stats->has_max_cardinality ||
	    !new_stats.has_max_cardinality) {
		stats = nullptr;
		return;
	}
	if (!TryAddOperator::Operation(stats->estimated_cardinality, new_stats.estimated_cardinality,
	                               stats->estimated_cardinality) ||
	    !TryAddOperator::Operation(stats->max_cardinality, new_stats.max_cardinality, stats->max_cardinality)) {
		stats.reset();
	}
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateUnion(LogicalSetOperation &setop,
                                                                unique_ptr<LogicalOperator> &node_ptr) {
	// first propagate statistics in the child nodes
	vector<unique_ptr<NodeStatistics>> stats;
	for (auto &child : setop.children) {
		stats.push_back(PropagateStatistics(child));
	}

	// now fetch the column bindings of the children both sides
	vector<vector<ColumnBinding>> child_bindings;
	for (auto &child : setop.children) {
		child_bindings.push_back(child->GetColumnBindings());
	}
	for (idx_t i = 0; i < setop.column_count; i++) {
		// for each column binding, we fetch the statistics from both the lhs and the rhs
		unique_ptr<BaseStatistics> new_stats;
		for (idx_t child_idx = 0; child_idx < setop.children.size(); child_idx++) {
			auto stats_entry = statistics_map.find(child_bindings[child_idx][i]);
			if (stats_entry == statistics_map.end()) {
				new_stats.reset();
				break;
			}
			auto &child_stats = stats_entry->second;
			if (!new_stats) {
				new_stats = child_stats->ToUnique();
			} else {
				new_stats->Merge(*child_stats);
			}
		}
		if (!new_stats) {
			// no statistics on at least one of the sides: can't propagate stats
			continue;
		}
		// propagate the stats for this column
		ColumnBinding binding(setop.table_index, ProjectionIndex(i));
		statistics_map[binding] = std::move(new_stats);
	}
	// merge all cardinalities of the child stats together
	for (idx_t i = 1; i < stats.size(); i++) {
		if (!stats[i]) {
			return nullptr;
		}
		AddCardinalities(stats[0], *stats[i]);
	}
	return std::move(stats[0]);
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalSetOperation &setop,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	if (setop.type == LogicalOperatorType::LOGICAL_UNION) {
		return PropagateUnion(setop, node_ptr);
	}
	D_ASSERT(setop.children.size() == 2);
	// first propagate statistics in the child nodes
	auto left_stats = PropagateStatistics(setop.children[0]);
	auto right_stats = PropagateStatistics(setop.children[1]);

	// now fetch the column bindings on both sides
	auto left_bindings = setop.children[0]->GetColumnBindings();
	auto right_bindings = setop.children[1]->GetColumnBindings();

	D_ASSERT(left_bindings.size() == right_bindings.size());
	D_ASSERT(left_bindings.size() == setop.column_count);
	for (idx_t i = 0; i < setop.column_count; i++) {
		// for each column binding, we fetch the statistics from both the lhs and the rhs
		auto left_entry = statistics_map.find(left_bindings[i]);
		auto right_entry = statistics_map.find(right_bindings[i]);
		if (left_entry == statistics_map.end() || right_entry == statistics_map.end()) {
			// no statistics on one of the sides: can't propagate stats
			continue;
		}
		unique_ptr<BaseStatistics> new_stats;
		switch (setop.type) {
		case LogicalOperatorType::LOGICAL_EXCEPT:
			// except: use the stats of the LHS
			new_stats = left_entry->second->ToUnique();
			break;
		case LogicalOperatorType::LOGICAL_INTERSECT:
			// intersect: intersect the two stats
			// FIXME: for now we just use the stats of the LHS, as this is correct
			// however, the stats can be further refined to the minimal subset of the LHS and RHS
			new_stats = left_entry->second->ToUnique();
			break;
		default:
			throw InternalException("Unsupported setop type");
		}
		ColumnBinding binding(setop.table_index, ProjectionIndex(i));
		statistics_map[binding] = std::move(new_stats);
	}
	if (!left_stats || !right_stats) {
		return nullptr;
	}
	return left_stats;
}

} // namespace duckdb
