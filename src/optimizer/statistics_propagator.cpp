#include "duckdb/optimizer/statistics_propagator.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_positional_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"

namespace duckdb {

StatisticsPropagator::StatisticsPropagator(Optimizer &optimizer_p, unique_ptr<LogicalOperator> &root_p,
                                           StatisticsPropagationMode mode_p)
    : optimizer(optimizer_p), context(optimizer.context), mode(mode_p), root(root_p) {
	root->ResolveOperatorTypes();
}

void StatisticsPropagator::ReplaceWithEmptyResult(unique_ptr<LogicalOperator> &node) {
	node = make_uniq<LogicalEmptyResult>(std::move(node));
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateChildren(LogicalOperator &node,
                                                                   unique_ptr<LogicalOperator> &node_ptr) {
	for (idx_t child_idx = 0; child_idx < node.children.size(); child_idx++) {
		PropagateStatistics(node.children[child_idx]);
	}
	return nullptr;
}

void StatisticsPropagator::ResetAdditiveStatistics(const vector<ColumnBinding> &bindings) {
	// additive statistics (e.g. total_string_length) describe a fixed row population - an operator that can emit an
	// input row more than once invalidates them for all pass-through columns
	for (auto &binding : bindings) {
		auto entry = statistics_map.find(binding);
		if (entry != statistics_map.end() && entry->second) {
			entry->second->ResetAdditiveStatistics();
		}
	}
}

void StatisticsPropagator::ResetAdditiveVerificationStats(Expression &expr) {
	auto &stats = expr.GetVerificationStats();
	if (stats) {
		stats->ResetAdditiveStatistics();
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { ResetAdditiveVerificationStats(child); });
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalOperator &node,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	propagation_path.emplace_back(node_ptr);
	unique_ptr<NodeStatistics> result;
	switch (node.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		result = PropagateStatistics(node.Cast<LogicalAggregate>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		result = PropagateStatistics(node.Cast<LogicalCopyToFile>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		result = PropagateStatistics(node.Cast<LogicalCrossProduct>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_CTE_REF:
		result = PropagateStatistics(node.Cast<LogicalCTERef>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		result = PropagateStatistics(node.Cast<LogicalMaterializedCTE>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_FILTER:
		result = PropagateStatistics(node.Cast<LogicalFilter>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_GET:
		result = PropagateStatistics(node.Cast<LogicalGet>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION:
		result = PropagateStatistics(node.Cast<LogicalProjection>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		result = PropagateStatistics(node.Cast<LogicalSecureView>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		result = PropagateStatistics(node.Cast<LogicalJoin>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_LIMIT:
		result = PropagateStatistics(node.Cast<LogicalLimit>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
		result = PropagateStatistics(node.Cast<LogicalPositionalJoin>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
		result = PropagateStatistics(node.Cast<LogicalSetOperation>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		result = PropagateStatistics(node.Cast<LogicalOrder>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_WINDOW:
		result = PropagateStatistics(node.Cast<LogicalWindow>(), node_ptr);
		break;
	case LogicalOperatorType::LOGICAL_UNNEST:
		result = PropagateChildren(node, node_ptr);
		// unnest emits its input row once per unnested entry
		ResetAdditiveStatistics(node.children[0]->GetColumnBindings());
		break;
	default:
		result = PropagateChildren(node, node_ptr);
	}

	if (mode == StatisticsPropagationMode::FULL &&
	    !optimizer.OptimizerDisabled(OptimizerType::COMPRESSED_MATERIALIZATION)) {
		// compress data based on statistics for materializing operators
		CompressedMaterialization compressed_materialization(optimizer, *root, propagation_path, statistics_map);
		compressed_materialization.Compress(node_ptr);
	}

	propagation_path.pop_back();
	return result;
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(unique_ptr<LogicalOperator> &node_ptr) {
	return PropagateStatistics(*node_ptr, node_ptr);
}

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalCopyToFile &op,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	auto stats = PropagateChildren(op, node_ptr);
	if (op.children.empty()) {
		return stats;
	}
	auto bindings = op.children[0]->GetColumnBindings();
	if (op.partition_output) {
		// the partition column statistics let the partitioned write pick its partitioning scheme
		op.partition_stats.clear();
		for (auto &col : op.partition_columns) {
			unique_ptr<BaseStatistics> partition_stats;
			if (col < bindings.size()) {
				auto entry = statistics_map.find(bindings[col]);
				if (entry != statistics_map.end() && entry->second) {
					partition_stats = entry->second->ToUnique();
				}
			}
			op.partition_stats.push_back(std::move(partition_stats));
		}
	}
	if (!op.function.copy_to_propagate_statistics || !op.bind_data) {
		return stats;
	}
	vector<optional_ptr<BaseStatistics>> column_stats(bindings.size());
	for (idx_t i = 0; i < bindings.size(); i++) {
		auto entry = statistics_map.find(bindings[i]);
		if (entry != statistics_map.end()) {
			column_stats[i] = entry->second.get();
		}
	}
	CopyToPropagateStatsInput input {context, *op.bind_data, column_stats};
	op.function.copy_to_propagate_statistics(input);
	return stats;
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(Expression &expr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_AGGREGATE:
		return PropagateExpression(expr.Cast<BoundAggregateExpression>(), expr_ptr);
	case ExpressionClass::BOUND_CASE:
		return PropagateExpression(expr.Cast<BoundCaseExpression>(), expr_ptr);
	case ExpressionClass::BOUND_CONJUNCTION:
		return PropagateExpression(expr.Cast<BoundConjunctionExpression>(), expr_ptr);
	case ExpressionClass::BOUND_FUNCTION:
		return PropagateExpression(expr.Cast<BoundFunctionExpression>(), expr_ptr);
	case ExpressionClass::BOUND_CONSTANT:
		return PropagateExpression(expr.Cast<BoundConstantExpression>(), expr_ptr);
	case ExpressionClass::BOUND_COLUMN_REF:
		return PropagateExpression(expr.Cast<BoundColumnRefExpression>(), expr_ptr);
	case ExpressionClass::BOUND_OPERATOR:
		return PropagateExpression(expr.Cast<BoundOperatorExpression>(), expr_ptr);
	default:
		break;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) { PropagateExpression(child); });
	return nullptr;
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(unique_ptr<Expression> &expr) {
#ifdef D_ASSERT_IS_ENABLED
	auto return_type = expr->GetReturnType();
#endif
	auto stats = PropagateExpression(*expr, expr);
	D_ASSERT(expr->GetReturnType() != return_type || expr->GetReturnType().EqualsIncludingCollation(return_type));
	if (Settings::Get<DebugVerifyStatsSetting>(context) && stats) {
		expr->SetVerificationStats(stats->ToUnique());
	}
	return stats;
}

} // namespace duckdb
