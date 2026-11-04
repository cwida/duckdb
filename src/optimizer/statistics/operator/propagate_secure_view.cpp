#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalSecureView &op,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// remember which statistics we knew about before descending into the view
	column_binding_set_t bindings_before;
	for (auto &entry : statistics_map) {
		bindings_before.insert(entry.first);
	}

	// propagate into the view - the view definition itself is always optimized using its own statistics
	auto child_stats = PropagateStatistics(op.children[0]);

	// statistics describe the data itself (min/max, null counts, distinct counts). Only the statistics of the columns
	// that the view emits may cross the boundary, and only if the view emits every row it reads - otherwise they would
	// expose values from rows that the view does not return, both through stats() and by allowing filters on top of
	// the view to be pruned.
	column_binding_set_t exposed_bindings;
	if (op.propagate_statistics) {
		for (auto &binding : op.GetColumnBindings()) {
			exposed_bindings.insert(binding);
		}
	}
	vector<ColumnBinding> leaked_bindings;
	for (auto &entry : statistics_map) {
		if (bindings_before.find(entry.first) != bindings_before.end()) {
			continue;
		}
		if (exposed_bindings.find(entry.first) != exposed_bindings.end()) {
			continue;
		}
		leaked_bindings.push_back(entry.first);
	}
	for (auto &binding : leaked_bindings) {
		statistics_map.erase(binding);
	}
	if (!op.propagate_statistics) {
		// no cardinality information escapes the view either
		return nullptr;
	}
	return child_stats;
}

} // namespace duckdb
