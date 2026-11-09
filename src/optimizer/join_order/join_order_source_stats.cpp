#include "duckdb/optimizer/join_order/join_order_source_stats.hpp"

#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"

namespace duckdb {

JoinOrderSourceStats::JoinOrderSourceStats(ClientContext &context, LogicalOperator &root) {
	AddPlan(context, root);
}

void JoinOrderSourceStats::AddPlan(ClientContext &context, LogicalOperator &root) {
	vector<reference<LogicalOperator>> pending {root};
	while (!pending.empty()) {
		auto &op = pending.back().get();
		pending.pop_back();
		if (op.type == LogicalOperatorType::LOGICAL_GET && op.children.empty()) {
			auto &get = op.Cast<LogicalGet>();
			if (scans.find(get.table_index) == scans.end()) {
				auto cardinality = get.EstimateSourceCardinality(context);
				auto stats = RelationStatisticsHelper::ExtractGetStats(get, context, cardinality);
				scans.emplace(get.table_index, ScanStats {cardinality, std::move(stats)});
			}
		}
		for (auto &child : op.children) {
			pending.push_back(*child);
		}
	}
}

optional_ptr<const RelationStats> JoinOrderSourceStats::FindScanStats(const LogicalGet &get) const {
	auto entry = scans.find(get.table_index);
	return entry == scans.end() ? nullptr : optional_ptr<const RelationStats>(entry->second.stats);
}

static idx_t SourceChildCount(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return op.children.size();
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		return op.Cast<LogicalSecureView>().propagate_statistics ? op.children.size() : 0;
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		switch (op.Cast<LogicalComparisonJoin>().join_type) {
		case JoinType::INNER:
			return 2;
		case JoinType::LEFT:
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::MARK:
			return 1;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

join_column_sources_t JoinOrderSourceStats::Resolve(LogicalOperator &root) const {
	struct Frame {
		explicit Frame(LogicalOperator &op) : op(op) {
		}

		reference<LogicalOperator> op;
		idx_t next_child = 0;
		join_column_sources_t inputs;
	};
	vector<Frame> stack;
	stack.emplace_back(root);
	while (true) {
		auto &frame = stack.back();
		auto &op = frame.op.get();
		if (frame.next_child < SourceChildCount(op)) {
			auto &child = *op.children[frame.next_child++];
			stack.emplace_back(child);
			continue;
		}
		join_column_sources_t output;
		if (op.type == LogicalOperatorType::LOGICAL_GET && op.children.empty()) {
			auto &get = op.Cast<LogicalGet>();
			auto entry = scans.find(get.table_index);
			if (entry != scans.end() && !get.extra_info.sample_options) {
				auto &scan = entry->second;
				for (auto &column : scan.stats.columns) {
					output.emplace(column.binding, JoinColumnSource {column.binding, scan.cardinality,
					                                                 scan.stats.cardinality, column.total_domain});
				}
			}
		} else if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto bindings = op.GetColumnBindings();
			for (idx_t i = 0; i < op.expressions.size(); i++) {
				auto &expr = *op.expressions[i];
				if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
					continue;
				}
				auto &column = expr.Cast<BoundColumnRefExpression>();
				auto entry = frame.inputs.find(column.Binding());
				if (column.Depth() == 0 && entry != frame.inputs.end()) {
					output.emplace(bindings[i], entry->second);
				}
			}
		} else {
			for (auto &binding : op.GetColumnBindings()) {
				auto entry = frame.inputs.find(binding);
				if (entry != frame.inputs.end()) {
					output.emplace(binding, entry->second);
				}
			}
		}
		stack.pop_back();
		if (stack.empty()) {
			return output;
		}
		for (auto &entry : output) {
			stack.back().inputs.emplace(entry.first, std::move(entry.second));
		}
	}
}

} // namespace duckdb
