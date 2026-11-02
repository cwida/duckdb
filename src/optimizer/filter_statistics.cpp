#include "duckdb/optimizer/filter_statistics.hpp"

#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

namespace duckdb {

FilterStatisticsOptimizer::FilterStatisticsOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

static bool HasMultipleBindings(const Expression &expr) {
	unordered_set<TableIndex> bindings;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		if (ref.Depth() == 0) {
			bindings.insert(ref.Binding().table_index);
		}
	});
	return bindings.size() > 1;
}

bool FilterStatisticsOptimizer::HasMultiBindingFilter(const LogicalOperator &op) const {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		for (auto &expr : op.Cast<LogicalFilter>().expressions) {
			if (HasMultipleBindings(*expr)) {
				return true;
			}
		}
	}
	for (auto &child : op.children) {
		if (HasMultiBindingFilter(*child)) {
			return true;
		}
	}
	return false;
}

bool FilterStatisticsOptimizer::ContainsDelimJoin(const LogicalOperator &op) const {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsDelimJoin(*child)) {
			return true;
		}
	}
	return false;
}

void FilterStatisticsOptimizer::Optimize(unique_ptr<LogicalOperator> &plan) {
	if (ContainsDelimJoin(*plan) || !HasMultiBindingFilter(*plan)) {
		return;
	}

	bool filter_bindings_changed = false;
	optimizer.RunOptimizer(OptimizerType::STATISTICS_PROPAGATION, [&]() {
		StatisticsPropagator propagator(optimizer, plan, StatisticsPropagationMode::FILTER_SIMPLIFICATION);
		propagator.PropagateStatistics(plan);
		filter_bindings_changed = propagator.FilterBindingsChanged();
	});
	if (!filter_bindings_changed) {
		return;
	}

	optimizer.RunOptimizer(OptimizerType::FILTER_PUSHDOWN, [&]() {
		FilterPushdown filter_pushdown(optimizer);
		unordered_set<TableIndex> top_bindings;
		filter_pushdown.CheckMarkToSemi(*plan, top_bindings);
		plan = filter_pushdown.Rewrite(std::move(plan));
	});
}

} // namespace duckdb
