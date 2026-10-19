#include "duckdb/optimizer/partitioned_execution.hpp"
#include "duckdb/main/settings.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/logical_operator_deep_copy.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/function/partition_stats.hpp"

namespace duckdb {

PartitionedExecution::PartitionedExecution(Optimizer &optimizer_p, unique_ptr<LogicalOperator> &root_p)
    : optimizer(optimizer_p), root(root_p),
      num_threads(TaskScheduler::GetScheduler(optimizer.context).NumberOfThreads()) {
}

struct PartitionedExecutionConfig {
	//! Maximum number of columns to try to use for splitting ranges
	static constexpr idx_t MAXIMUM_COLUMNS = 3;
	//! Minimum number of row groups per thread per partition to split on
	static constexpr idx_t MIN_ROW_GROUPS_PER_THREAD_PER_PARTITION = 32;
	//! Maximum overlap (as fraction of partition size) that we allow for a split
	static constexpr double MAX_OVERLAP_RATIO = 0.1;
	//! Minimum result cardinality before we even consider splitting
	static constexpr idx_t MINIMUM_RESULT_CARDINALITY = 4194304;
};

struct PartitionedExecutionColumn {
	explicit PartitionedExecutionColumn(idx_t original_idx_p, Expression &expr,
	                                    OrderType order_type_p = OrderType::ASCENDING)
	    : original_idx(original_idx_p), column_binding(expr.Cast<BoundColumnRefExpression>().Binding()),
	      order_type(order_type_p) {
	}

	idx_t original_idx;
	ColumnBinding column_binding;
	OrderType order_type;
	ColumnIndex column_index;
	StorageIndex storage_index;
};

static bool PartitionedExecutionGetColumns(LogicalOperator &op, vector<PartitionedExecutionColumn> &columns) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		const auto &agg = op.Cast<LogicalAggregate>();
		if (agg.grouping_sets.size() > 1 || !agg.grouping_functions.empty() || agg.groups.empty()) {
			return false; // Only regular grouped aggregations
		}
		for (auto &group : agg.groups) {
			if (group->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				columns.emplace_back(columns.size(), *group); // We can partition on any colref
			}
		}
		return true;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		auto &order = op.Cast<LogicalOrder>();
		for (auto &order_by_node : order.orders) {
			if (order_by_node.expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				break; // Have to break on the first non-colref
			}
			columns.emplace_back(columns.size(), *order_by_node.expression, order_by_node.type);
		}
		return true;
	}
	case LogicalOperatorType::LOGICAL_WINDOW: {
		const auto &expr = op.expressions[0]->Cast<BoundWindowExpression>();
		for (idx_t expr_idx = 1; expr_idx < op.expressions.size(); expr_idx++) {
			if (!expr.PartitionsAreEquivalent(op.expressions[expr_idx]->Cast<BoundWindowExpression>())) {
				return false;
			}
		}
		for (auto &partition : expr.Partitions()) {
			if (partition->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				columns.emplace_back(columns.size(), *partition);
			}
		}
		return true;
	}
	default:
		return false;
	}
}

static vector<PartitionedExecutionColumn>::iterator
PartitionedExecutionHandleColumnRemoval(const LogicalOperatorType type, vector<PartitionedExecutionColumn> &columns,
                                        vector<PartitionedExecutionColumn>::iterator &it) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_WINDOW:
		return columns.erase(it); // We can partition on any colref so we can freely remove any
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		columns.erase(it, columns.end()); // Have to remove from the first non-colref onward
		return columns.end();
	default:
		throw NotImplementedException("PartitionedExecutionHandleColumnRemoval for %s", EnumUtil::ToString(type));
	}
}

static optional_ptr<LogicalGet> PartitionedExecutionTraceColumns(LogicalOperator &op,
                                                                 vector<PartitionedExecutionColumn> &columns) {
	if (op.children.size() != 1) {
		return nullptr; // Can't handle more than one child (yet)
	}

	reference<LogicalOperator> child_ref(*op.children[0]);
	while (child_ref.get().type != LogicalOperatorType::LOGICAL_GET) {
		switch (child_ref.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &proj = child_ref.get().Cast<LogicalProjection>();
			for (auto it = columns.begin(); it != columns.end();) {
				auto &expr = *proj.expressions[it->column_binding.column_index.GetIndex()];
				if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					it->column_binding = expr.Cast<BoundColumnRefExpression>().Binding();
					it++;
				} else {
					it = PartitionedExecutionHandleColumnRemoval(op.type, columns, it);
				}
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER: {
			break; // Don't have to update bindings
		}
		default:
			return nullptr; // Unsupported for partition pass-through
		}
		child_ref = *child_ref.get().children[0];
	}

	D_ASSERT(child_ref.get().type == LogicalOperatorType::LOGICAL_GET);
	if (!child_ref.get().children.empty()) {
		return nullptr; // Table in/out, unsupported
	}
	auto &get = child_ref.get().Cast<LogicalGet>();

	if ((!get.function.statistics && !get.function.statistics_extended) || !get.function.get_partition_stats) {
		return nullptr; // We need these
	}

	// Get the storage index
	const auto &column_ids = get.GetColumnIds();
	for (auto it = columns.begin(); it != columns.end();) {
		it->column_index = column_ids[it->column_binding.column_index.GetIndex()];
		if (get.TryGetStorageIndex(it->column_index, it->storage_index)) {
			it++;
		} else {
			it = PartitionedExecutionHandleColumnRemoval(op.type, columns, it); // Did not get a storage index
		}
	}

	return get;
}

static bool PartitionedExecutionCanUseStats(const unique_ptr<BaseStatistics> &stats) {
	if (!stats || stats->CanHaveNull()) {
		return false; // No stats or contains NULL
	}

	switch (stats->GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		return NumericStats::HasMinMax(*stats);
	case StatisticsType::STRING_STATS:
		// Let's not mess with BLOB for now
		if (stats->GetType() != LogicalType::VARCHAR) {
			return false;
		}
		if (!StringStats::HasMinMax(*stats)) {
			return false;
		}
		// Truncated multibyte UTF-8 sequences produce invalid string bytes in min/max; skip those row groups
		return Value::StringIsValid(StringStats::Min(*stats)) && Value::StringIsValid(StringStats::Max(*stats));
	default:
		return false; // Only numeric/string supported for now
	}
}

static bool PartitionedExecutionMinMaxEqual(const BaseStatistics &stats) {
	Value min;
	Value max;
	switch (stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		min = NumericStats::Min(stats);
		max = NumericStats::Max(stats);
		break;
	case StatisticsType::STRING_STATS:
		min = Value::BLOB_RAW(StringStats::Min(stats));
		max = Value::BLOB_RAW(StringStats::Max(stats));
		break;
	default:
		throw NotImplementedException("PartitionedExecutionMinMaxEqual for %s",
		                              EnumUtil::ToString(stats.GetStatsType()));
	}
	return min == max;
}

static void PartitionedExecutionHandleGlobalStats(Optimizer &optimizer, LogicalOperator &op, LogicalGet &get,
                                                  vector<PartitionedExecutionColumn> &columns) {
	for (auto it = columns.begin(); it != columns.end();) {
		unique_ptr<BaseStatistics> stats;
		if (get.function.statistics_extended) {
			TableFunctionGetStatisticsInput stats_input(get.bind_data.get(), it->column_index);
			stats = get.function.statistics_extended(optimizer.context, stats_input);
		} else {
			stats = get.function.statistics(optimizer.context, get.bind_data.get(), it->column_index.GetPrimaryIndex());
		}
		if (!PartitionedExecutionCanUseStats(stats)) {
			it = PartitionedExecutionHandleColumnRemoval(op.type, columns, it);
		} else if (PartitionedExecutionMinMaxEqual(*stats)) {
			it = columns.erase(it); // This column will never matter
		} else {
			it++;
		}
	}
}

struct PartitionedExecutionStatsNode {
	PartitionedExecutionStatsNode(Value &&val_p, int64_t count_delta_p)
	    : val(std::move(val_p)), count_delta(count_delta_p) {
	}
	Value val;
	int64_t count_delta;

	friend bool operator<(const PartitionedExecutionStatsNode &lhs, const PartitionedExecutionStatsNode &rhs) {
		if (lhs.val != rhs.val) {
			return lhs.val < rhs.val; // Not equal, just return comparison
		}
		return lhs.count_delta > rhs.count_delta; // Add start of row groups first
	}
};

static void PartitionedExecutionAddStatsNodes(const BaseStatistics &stats, const idx_t count,
                                              vector<PartitionedExecutionStatsNode> &stats_nodes) {
	Value min;
	Value max;
	switch (stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS:
		min = NumericStats::Min(stats);
		max = NumericStats::Max(stats);
		break;
	case StatisticsType::STRING_STATS:
		min = StringStats::Min(stats);
		max = StringStats::Max(stats);
		break;
	default:
		throw NotImplementedException("PartitionedExecutionAddStatsNodes for %s",
		                              EnumUtil::ToString(stats.GetStatsType()));
	}

	stats_nodes.emplace_back(std::move(min), NumericCast<int64_t>(count));
	stats_nodes.emplace_back(std::move(max), -NumericCast<int64_t>(count));
}

static vector<vector<PartitionedExecutionStatsNode>>
PartitionedExecutionCollectStatsNodes(LogicalOperator &op, vector<PartitionedExecutionColumn> &columns,
                                      const vector<PartitionStatistics> &partition_stats) {
	// We impose a maximum number of columns otherwise we could be fetching a massive amount of stats from storage
	vector<vector<PartitionedExecutionStatsNode>> column_stats_nodes;
	for (auto it = columns.begin();
	     it != columns.end() && column_stats_nodes.size() < PartitionedExecutionConfig::MAXIMUM_COLUMNS;) {
		vector<PartitionedExecutionStatsNode> stats_nodes;
		stats_nodes.reserve(partition_stats.size() * 2);

		bool success = true;
		for (auto &ps : partition_stats) {
			if (!ps.partition_row_group) {
				success = false; // No row group to get stats from
				break;
			}
			const auto stats = ps.partition_row_group->GetColumnStatistics(it->storage_index);
			if (PartitionedExecutionCanUseStats(stats)) {
				PartitionedExecutionAddStatsNodes(*stats, ps.count, stats_nodes);
			} else {
				success = false; // Unable to use these stats
				break;
			}
		}
		if (success) {
			column_stats_nodes.emplace_back(std::move(stats_nodes));
			it++;
		} else {
			it = PartitionedExecutionHandleColumnRemoval(op.type, columns, it);
		}
	}
	while (columns.size() > column_stats_nodes.size()) {
		columns.pop_back();
	}
	D_ASSERT(columns.size() == column_stats_nodes.size());
	return column_stats_nodes;
}

static void PartitionedExecutionGrowPartition(
    const vector<PartitionedExecutionStatsNode> &stats_nodes, idx_t &i, int64_t &current_overlap,
    idx_t &partition_count, double &partition_overlap_ratio, const std::function<bool()> &stopping_criterion,
    const std::function<void()> &update_callback = [] {}) {
	while (i < stats_nodes.size() && !stopping_criterion()) {
		const auto &node = stats_nodes[i++];
		current_overlap += node.count_delta; // Keep track of overall overlap at "i"
		D_ASSERT(current_overlap >= 0);
		if (node.count_delta > 0) {
			partition_count += NumericCast<idx_t>(node.count_delta); // Only add if it's the start of a row group
		}
		if (partition_count == 0) {
			partition_overlap_ratio = 1;
		} else {
			partition_overlap_ratio = static_cast<double>(current_overlap) / static_cast<double>(partition_count);
		}
		update_callback();
	}
}

struct PartitionedExecutionRange {
	Value min;
	Value max;
	idx_t overlap;
	idx_t count;
};

static pair<idx_t, vector<PartitionedExecutionRange>>
PartitionedExecutionComputeRanges(LogicalOperator &op, vector<PartitionedExecutionColumn> &columns,
                                  const vector<PartitionStatistics> &partition_stats, const idx_t num_threads) {
	auto column_stats_nodes = PartitionedExecutionCollectStatsNodes(op, columns, partition_stats);
	if (columns.empty()) {
		return {}; // None of the columns ended up being eligible
	}

	// Tuned constants for finding reasonable partitions
	const auto min_row_groups_per_partition =
	    num_threads * PartitionedExecutionConfig::MIN_ROW_GROUPS_PER_THREAD_PER_PARTITION;
	const auto min_partition_count = min_row_groups_per_partition * DEFAULT_ROW_GROUP_SIZE;

	// Sort indices based on the values in the stats nodes
	idx_t max_col_idx = 0;
	vector<vector<PartitionedExecutionRange>> column_ranges;
	for (idx_t col_idx = 0; col_idx < columns.size(); col_idx++) {
		auto &stats_nodes = column_stats_nodes[col_idx];
		std::sort(stats_nodes.begin(), stats_nodes.end());

		column_ranges.emplace_back();
		auto &ranges = column_ranges[col_idx];

		int64_t current_overlap = 0;
		for (idx_t i = 0; i < stats_nodes.size();) {
			const auto partition_start_i = i;
			idx_t partition_count = 0;
			double partition_overlap_ratio = 1;

			// Grow partition until "partition_count" is at least "min_partition_count"
			PartitionedExecutionGrowPartition(
			    stats_nodes, i, current_overlap, partition_count, partition_overlap_ratio,
			    [&partition_count, &min_partition_count] { return partition_count >= min_partition_count; });

			// Grow partition until "partition_overlap" is less than the allowed maximum
			PartitionedExecutionGrowPartition(
			    stats_nodes, i, current_overlap, partition_count, partition_overlap_ratio, [&partition_overlap_ratio] {
				    return partition_overlap_ratio <= PartitionedExecutionConfig::MAX_OVERLAP_RATIO;
			    });

			if (partition_overlap_ratio != 0) {
				// Temp variables for looking ahead
				auto temp_current_overlap = current_overlap;
				auto temp_i = i;
				auto temp_partition_count = partition_count;
				auto temp_partition_overlap_ratio = partition_overlap_ratio;

				// To keep track of the lowest overlap
				auto lowest_overlap_ratio = partition_overlap_ratio;
				auto lowest_i = i;

				// Look ahead for at most half of "min_partition_count" to see if there's a point with less overlap
				const auto end = partition_count + min_partition_count / 2;
				PartitionedExecutionGrowPartition(
				    stats_nodes, temp_i, temp_current_overlap, temp_partition_count, temp_partition_overlap_ratio,
				    [&temp_partition_count, &end] { return temp_partition_count >= end; },
				    [&temp_i, &temp_partition_overlap_ratio, &lowest_overlap_ratio, &lowest_i] {
					    if (temp_partition_overlap_ratio < lowest_overlap_ratio) {
						    lowest_overlap_ratio = temp_partition_overlap_ratio;
						    lowest_i = temp_i;
					    }
				    });

				// Actually grow if it's a success
				if (lowest_i != i) {
					PartitionedExecutionGrowPartition(stats_nodes, i, current_overlap, partition_count,
					                                  partition_overlap_ratio,
					                                  [&i, &lowest_i] { return i == lowest_i; });
				}

				// Grow until the delta is non-negative
				if (partition_overlap_ratio != 0) {
					PartitionedExecutionGrowPartition(stats_nodes, i, current_overlap, partition_count,
					                                  partition_overlap_ratio,
					                                  [&] { return stats_nodes[i].count_delta > 0; });
				}
			}

			// Finally, Grow if we would otherwise leave a remainder that is less than half of
			// min_row_groups_per_partition
			if (stats_nodes.size() - i < min_row_groups_per_partition / 2) {
				PartitionedExecutionGrowPartition(stats_nodes, i, current_overlap, partition_count,
				                                  partition_overlap_ratio, [] { return false; });
			}

			// Add the new range to the ranges
			const auto partition_end_i = i;
			PartitionedExecutionRange range;
			if (partition_start_i != 0) {
				range.min = stats_nodes[partition_start_i].val;
			}
			if (partition_end_i != stats_nodes.size()) {
				range.max = stats_nodes[partition_end_i].val;
			}
			range.overlap = NumericCast<idx_t>(current_overlap);
			range.count = partition_count;
			ranges.emplace_back(std::move(range));
		}

		if (column_ranges[col_idx].size() > column_ranges[max_col_idx].size()) {
			max_col_idx = col_idx;
		}
	}

	auto &max_ranges = column_ranges[max_col_idx];
	if (columns[max_col_idx].order_type == OrderType::DESCENDING) {
		std::reverse(max_ranges.begin(), max_ranges.end()); // Reverse for DESC order
	}

	return make_pair(max_col_idx, std::move(max_ranges));
}

enum class PartitionedExecutionStatsType : uint8_t { MIN, MAX };

ExpressionType PartitionedExecutionGetExpressionType(PartitionedExecutionStatsType stats_type,
                                                     const bool is_last_column) {
	// We always have to be inclusive for either the min or the max, we choose to be inclusive for the max
	// Furthermore, we have to be inclusive for any column that's not the last column
	switch (stats_type) {
	case PartitionedExecutionStatsType::MIN:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	case PartitionedExecutionStatsType::MAX:
		return is_last_column ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_LESSTHANOREQUALTO;
	default:
		throw NotImplementedException("PartitionedExecutionGetExpressionType for PartitionedExecutionStatsType");
	}
}

void PartitionedExecutionSplitPipeline(Optimizer &optimizer, unique_ptr<LogicalOperator> &root,
                                       unique_ptr<LogicalOperator> &op,
                                       const vector<PartitionedExecutionColumn> &columns, const idx_t &range_col_idx,
                                       const vector<PartitionedExecutionRange> &ranges) {
	vector<unique_ptr<LogicalOperator>> children;
	for (const auto &range : ranges) {
		LogicalOperatorDeepCopy deep_copy(optimizer.binder, nullptr);
		unique_ptr<LogicalOperator> copy;
		try {
			copy = deep_copy.DeepCopy(op);
		} catch (NotImplementedException &) {
			return; // Cannot copy this operator
		}

		// Do this again for the copied plan so we can get the new column bindings
		vector<PartitionedExecutionColumn> copy_columns;
		if (!PartitionedExecutionGetColumns(*copy, copy_columns) || copy_columns.empty()) {
			throw InternalException("PartitionedExecutionSplitPipeline failed");
		}

		// Create filter operator
		unique_ptr<LogicalOperator> filter = make_uniq<LogicalFilter>();
		const auto &column = copy_columns[columns[range_col_idx].original_idx];

		// Create lower bound filter
		if (!range.min.IsNull()) {
			filter->expressions.emplace_back(BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			    make_uniq<BoundColumnRefExpression>(range.min.type(), column.column_binding),
			    make_uniq<BoundConstantExpression>(range.min)));
		}

		// Create upper bound filter
		if (!range.max.IsNull()) {
			filter->expressions.emplace_back(BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_LESSTHAN,
			    make_uniq<BoundColumnRefExpression>(range.max.type(), column.column_binding),
			    make_uniq<BoundConstantExpression>(range.max)));
		}

		// Add the filter under the operator
		filter->children.emplace_back(std::move(copy->children[0]));
		copy->children[0] = std::move(filter);

		// Now push it down
		FilterPushdown filter_pushdown(optimizer, false);
		copy = filter_pushdown.Rewrite(std::move(copy));

		// Add cardinality estimates for these GETs
		reference<LogicalOperator> get = *copy;
		while (!get.get().children.empty()) {
			get = *get.get().children[0];
		}

		if (get.get().type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
			continue; // Range incompatible with existing filters - skip
		}

		get.get().SetEstimatedCardinality(range.count);

		// Add it the children for the union
		children.emplace_back(std::move(copy));
	}

	if (children.size() < 2) {
		return; // Less than two ranges were compatible with existing filters, bail
	}

	// Replace operator with union
	const auto old_bindings = op->GetColumnBindings();
	op = make_uniq<LogicalSetOperation>(optimizer.binder.GenerateTableIndex(), old_bindings.size(), std::move(children),
	                                    LogicalOperatorType::LOGICAL_UNION, true,
	                                    op->type != LogicalOperatorType::LOGICAL_ORDER_BY);
	const auto new_bindings = op->GetColumnBindings();

	// Fix up column bindings
	ColumnBindingReplacer replacer;
	for (idx_t col_idx = 0; col_idx < old_bindings.size(); col_idx++) {
		replacer.replacement_bindings.emplace_back(old_bindings[col_idx], new_bindings[col_idx]);
	}
	replacer.stop_operator = op.get();
	replacer.VisitOperator(*root);
}

void PartitionedExecution::Optimize(unique_ptr<LogicalOperator> &op) {
	if (Settings::Get<PortablePlanSetting>(optimizer.context)) {
		// the split arms carry filters derived from observed row-group statistics
		return;
	}
	for (auto &child : op->children) {
		Optimize(child); // Depth-first
	}

	if (op->EstimateCardinality(optimizer.context) < PartitionedExecutionConfig::MINIMUM_RESULT_CARDINALITY) {
		return; // Too small
	}

	vector<PartitionedExecutionColumn> columns;
	if (!PartitionedExecutionGetColumns(*op, columns) || columns.empty()) {
		return; // Unable to get partition columns from this operator
	}

	optional_ptr<LogicalGet> get = PartitionedExecutionTraceColumns(*op, columns);
	if (!get || columns.empty()) {
		return; // Unable to trace any binding down to scan
	}

	// Check global stats first before getting partition stats
	PartitionedExecutionHandleGlobalStats(optimizer, *op, *get, columns);
	if (columns.empty()) {
		return; // Stats don't allow for the optimization
	}

	if (op->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		// Can't pick any arbitrary column for ORDER BY as we have to respect order, reduce columns down to 1
		while (columns.size() > 1) {
			columns.pop_back();
		}
	}

	GetPartitionStatsInput input(get->function, get->bind_data.get());
	const auto partition_stats = get->function.get_partition_stats(optimizer.context, input);
	if (partition_stats.size() <= 1) {
		return; // Can't split 0 or 1 partitions
	}

	const auto &[range_col_idx, ranges] = PartitionedExecutionComputeRanges(*op, columns, partition_stats, num_threads);
	if (ranges.size() < 2) {
		return; // Unable to compute useful ranges
	}

	PartitionedExecutionSplitPipeline(optimizer, root, op, columns, range_col_idx, ranges); // Success!
}

} // namespace duckdb
