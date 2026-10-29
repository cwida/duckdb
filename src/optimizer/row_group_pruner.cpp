#include "duckdb/optimizer/row_group_pruner.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/storage/table/row_group_reorderer.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb {

RowGroupPruner::RowGroupPruner(ClientContext &context_p) : context(context_p) {
}

unique_ptr<LogicalOperator> RowGroupPruner::Optimize(unique_ptr<LogicalOperator> op) {
	if (!TryOptimize(*op)) {
		for (auto &child : op->children) {
			child = Optimize(std::move(child));
		}
	}

	return op;
}

bool RowGroupPruner::TryOptimize(LogicalOperator &op) const {
	optional_idx row_limit;
	optional_idx row_offset;

	if (op.type != LogicalOperatorType::LOGICAL_LIMIT) {
		return false;
	}

	auto &logical_limit = op.Cast<LogicalLimit>();
	GetLimitAndOffset(logical_limit, row_limit, row_offset);
	auto logical_order = FindLogicalOrder(logical_limit);
	if (!logical_order) {
		return false;
	}

	// Only reorder row groups if there are no additional limit operators since they could modify the order
	reference<LogicalOperator> current_op = *logical_order;
	while (!current_op.get().children.empty()) {
		if (current_op.get().children.size() > 1) {
			return false;
		}

		const auto op_type = current_op.get().type;
		if (op_type == LogicalOperatorType::LOGICAL_LIMIT) {
			return false;
		}
		if (op_type == LogicalOperatorType::LOGICAL_FILTER ||
		    op_type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY ||
		    op_type == LogicalOperatorType::LOGICAL_DISTINCT) {
			row_limit.SetInvalid();
			row_offset.SetInvalid();
		}
		current_op = *current_op.get().children[0];
	}

	ColumnIndex column_index;
	auto logical_get = FindLogicalGet(*logical_order, column_index);
	if (!logical_get) {
		return false;
	}
	StorageIndex storage_index;
	if (!logical_get->TryGetStorageIndex(column_index, storage_index)) {
		return false;
	}

	if (logical_get->table_filters.HasFilters()) {
		// If there are filters, we only order the row groups but do not prune
		row_limit.SetInvalid();
		row_offset.SetInvalid();
	}

	// We can use the RowGroupReorderer!
	const auto &primary_order = logical_order->orders[0];
	auto options = CreateRowGroupReordererOptions(row_limit, row_offset, primary_order, *logical_get, storage_index,
	                                              logical_limit);
	if (!options) {
		return false;
	}
	logical_get->SetScanOrder(std::move(options));

	return true;
}

void RowGroupPruner::GetLimitAndOffset(const LogicalLimit &logical_limit, optional_idx &row_limit,
                                       optional_idx &row_offset) const {
	// UNSET = no LIMIT = unbounded; leave row_limit invalid.
	if (logical_limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		row_limit = logical_limit.limit_val.GetConstantValue();
	}

	if (logical_limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		row_offset = logical_limit.offset_val.GetConstantValue();
	} else if (logical_limit.offset_val.Type() == LimitNodeType::UNSET) {
		row_offset = 0;
	}
}

optional_ptr<LogicalOrder> RowGroupPruner::FindLogicalOrder(const LogicalLimit &logical_limit) const {
	reference<LogicalOperator> current_op = *logical_limit.children[0];
	while (current_op.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		current_op = *current_op.get().children[0];
	}

	if (current_op.get().type != LogicalOperatorType::LOGICAL_ORDER_BY) {
		return nullptr;
	}

	auto &logical_order = current_op.get().Cast<LogicalOrder>();
	auto order_column_type = logical_order.orders[0].expression->GetReturnType();
	if (!order_column_type.IsNumeric() && !order_column_type.IsTemporal() &&
	    order_column_type != LogicalType::VARCHAR) {
		return nullptr;
	}

	if (logical_order.orders[0].expression->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr;
	}

	return logical_order;
}

optional_ptr<LogicalGet> RowGroupPruner::FindLogicalGet(const LogicalOrder &logical_order,
                                                        ColumnIndex &column_index) const {
	const auto &primary_order = logical_order.orders[0];
	auto &colref = primary_order.expression->Cast<BoundColumnRefExpression>();

	JoinFilterPushdownColumn column;
	column.probe_column_index = colref.Binding();
	vector<JoinFilterPushdownColumn> columns {std::move(column)};
	vector<PushdownFilterTarget> pushdown_targets;
	JoinFilterPushdownOptimizer::GetPushdownFilterTargets(*logical_order.children[0], std::move(columns),
	                                                      pushdown_targets);

	if (pushdown_targets.size() != 1 || pushdown_targets[0].columns.size() != 1) {
		return nullptr;
	}

	auto &pushed_column = pushdown_targets[0].columns[0];
	if (pushed_column.mode != JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION ||
	    RuntimeFilterCastUtil::RuntimeFilterUsesTryCast(pushed_column)) {
		// the sort key reaches the scan through a cast chain that does not preserve the raw column's
		// ordering (an explicit TRY_CAST, or a cast that can throw) - ordering row groups by the raw
		// column statistics would be incorrect - bail out
		return nullptr;
	}

	auto &logical_get = pushdown_targets.front().get;

	if (!logical_get.function.set_scan_order) {
		return nullptr;
	}

	auto &binding = pushdown_targets[0].columns[0].probe_column_index;
	column_index = logical_get.GetColumnIndex(binding);

	return logical_get;
}

unique_ptr<RowGroupOrderOptions>
RowGroupPruner::CreateRowGroupReordererOptions(const optional_idx row_limit, const optional_idx row_offset,
                                               const BoundOrderByNode &primary_order, const LogicalGet &logical_get,
                                               const StorageIndex &storage_index, LogicalLimit &logical_limit) const {
	const auto &colref = primary_order.expression->Cast<BoundColumnRefExpression>();
	const auto column_type =
	    colref.GetReturnType() == LogicalType::VARCHAR ? OrderByColumnType::STRING : OrderByColumnType::NUMERIC;
	const auto order_type = primary_order.type;
	const auto null_order = primary_order.null_order;
	const auto order_by = order_type == OrderType::ASCENDING ? OrderByStatistics::MIN : OrderByStatistics::MAX;
	optional_idx combined_limit = row_limit.IsValid()
	                                  ? row_limit.GetIndex() + (row_offset.IsValid() ? row_offset.GetIndex() : 0)
	                                  : optional_idx();

	if (row_offset.IsValid() && row_offset.GetIndex() > 0 && logical_get.function.get_partition_stats) {
		// Try to prune with offset
		GetPartitionStatsInput input(logical_get.function, logical_get.bind_data.get());
		auto partition_stats = logical_get.function.get_partition_stats(context, input);

		if (!partition_stats.empty()) {
			auto offset_puning_result = RowGroupReorderer::GetOffsetAfterPruning(
			    order_by, column_type, order_type, null_order, storage_index, row_offset.GetIndex(), partition_stats);
			if (offset_puning_result.pruned_row_group_count > 0 || offset_puning_result.leading_null_group_offset > 0) {
				// We can prune row groups and/or reduce the offset by consuming definite NULL-only groups
				if (!logical_limit.unpruned_offset.IsValid()) {
					logical_limit.unpruned_offset = row_offset;
				}
				logical_limit.offset_val =
				    BoundLimitNode::ConstantValue(NumericCast<int64_t>(offset_puning_result.offset_remainder));

				if (combined_limit.IsValid()) {
					combined_limit = row_limit.GetIndex() + offset_puning_result.offset_remainder;
				}

				return make_uniq<RowGroupOrderOptions>(storage_index, order_by, order_type, null_order, column_type,
				                                       combined_limit, offset_puning_result.pruned_row_group_count,
				                                       offset_puning_result.leading_null_group_offset);
			}
		}
	}
	// Only sort row groups by primary order column and prune with limit if set
	return make_uniq<RowGroupOrderOptions>(storage_index, order_by, order_type, null_order, column_type, combined_limit,
	                                       NumericCast<uint64_t>(0));
}

} // namespace duckdb
