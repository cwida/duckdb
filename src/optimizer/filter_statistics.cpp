#include "duckdb/optimizer/filter_statistics.hpp"

#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

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

//! Collects the columns that the filters and join conditions of the plan read
static void GetComparedBindings(LogicalOperator &op, column_binding_set_t &bindings) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN: {
		auto add_binding = [&](const BoundColumnRefExpression &ref) {
			if (ref.Depth() == 0) {
				bindings.insert(ref.Binding());
			}
		};
		LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
			ExpressionIterator::VisitExpression<BoundColumnRefExpression>(**expr, add_binding);
		});
		break;
	}
	default:
		break;
	}
	for (auto &child : op.children) {
		GetComparedBindings(*child, bindings);
	}
}

//! Whether a scan of the plan emits one of the bindings from a column that holds only NULLs
static bool ScansAllNullBinding(ClientContext &context, LogicalOperator &op, const column_binding_set_t &bindings) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto &column_ids = get.GetColumnIds();
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (bindings.find(ColumnBinding(get.table_index, ProjectionIndex(i))) == bindings.end()) {
				continue;
			}
			auto stats = StatisticsPropagator::GetColumnStatistics(context, get, column_ids[i]);
			if (StatisticsPropagator::HasNoValidValue(stats.get())) {
				return true;
			}
		}
	}
	for (auto &child : op.children) {
		if (ScansAllNullBinding(context, *child, bindings)) {
			return true;
		}
	}
	return false;
}

bool FilterStatisticsOptimizer::ComparesAllNullColumn(LogicalOperator &plan) const {
	if (optimizer.OptimizerDisabled(OptimizerType::STATISTICS_PROPAGATION)) {
		return false;
	}
	column_binding_set_t bindings;
	GetComparedBindings(plan, bindings);
	return !bindings.empty() && ScansAllNullBinding(optimizer.context, plan, bindings);
}

void FilterStatisticsOptimizer::Optimize(unique_ptr<LogicalOperator> &plan) {
	if (ContainsDelimJoin(*plan)) {
		return;
	}
	// a comparison with a column that holds only NULLs never passes: propagating the statistics here removes
	// the filters and joins on it before the join order is chosen
	if (!HasMultiBindingFilter(*plan) && !ComparesAllNullColumn(*plan)) {
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
