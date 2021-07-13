#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

FilterPropagateResult StatisticsPropagator::PropagateTableFilter(BaseStatistics &stats, TableFilter &filter) {
	return filter.CheckStatistics(stats);
}

void StatisticsPropagator::UpdateFilterStatistics(BaseStatistics &input, TableFilter &filter) {
	// FIXME: update stats...
	switch (filter.filter_type) {
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_and = (ConjunctionAndFilter &)filter;
		for (auto &child_filter : conjunction_and.child_filters) {
			UpdateFilterStatistics(input, *child_filter);
		}
		break;
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = (ConstantFilter &)filter;
		UpdateFilterStatistics(input, constant_filter.comparison_type, constant_filter.constant);
		break;
	}
	default:
		break;
	}
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalGet &get,
                                                                     unique_ptr<LogicalOperator> *node_ptr) {
	if (get.function.cardinality) {
		node_stats = get.function.cardinality(context, get.bind_data.get());
	}
	if (!get.function.statistics) {
		// no column statistics to get
		return move(node_stats);
	}
	for (idx_t i = 0; i < get.column_ids.size(); i++) {
		auto stats = get.function.statistics(context, get.bind_data.get(), get.column_ids[i]);
		if (stats) {
			ColumnBinding binding(get.table_index, i);
			statistics_map.insert(make_pair(binding, move(stats)));
		}
	}
	// push table filters into the statistics
	vector<idx_t> column_indexes;
	column_indexes.reserve(get.table_filters.filters.size());
	for (auto &kv : get.table_filters.filters) {
		column_indexes.push_back(kv.first);
	}

	for (auto &table_filter_column : column_indexes) {
		idx_t column_index;
		for (column_index = 0; column_index < get.column_ids.size(); column_index++) {
			if (get.column_ids[column_index] == table_filter_column) {
				break;
			}
		}
		D_ASSERT(column_index < get.column_ids.size());
		D_ASSERT(get.column_ids[column_index] == table_filter_column);

		// find the stats
		ColumnBinding stats_binding(get.table_index, column_index);
		auto entry = statistics_map.find(stats_binding);
		if (entry == statistics_map.end()) {
			// no stats for this entry
			continue;
		}
		auto &stats = *entry->second;

		// fetch the table filter
		D_ASSERT(get.table_filters.filters.count(table_filter_column) > 0);
		auto &filter = get.table_filters.filters[table_filter_column];
		auto propagate_result = PropagateTableFilter(stats, *filter);
		switch (propagate_result) {
		case FilterPropagateResult::FILTER_ALWAYS_TRUE:
			// filter is always true; it is useless to execute it
			// erase this condition
			get.table_filters.filters.erase(table_filter_column);
			break;
		case FilterPropagateResult::FILTER_FALSE_OR_NULL:
		case FilterPropagateResult::FILTER_ALWAYS_FALSE:
			// filter is always false; this entire filter should be replaced by an empty result block
			ReplaceWithEmptyResult(*node_ptr);
			return make_unique<NodeStatistics>(0, 0);
		default:
			// general case: filter can be true or false, update this columns' statistics
			UpdateFilterStatistics(stats, *filter);
			break;
		}
	}
	return move(node_stats);
}

} // namespace duckdb
