#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

void CompressedMaterialization::CompressOrdering(unique_ptr<LogicalOperator> &op, vector<BoundOrderByNode> &orders,
                                                 column_binding_set_t referenced_bindings) {
	// Find all bindings referenced by non-colref expressions in the order nodes
	// These are excluded from compression by projection
	// But we can try to compress the expression directly
	for (idx_t order_node_idx = 0; order_node_idx < orders.size(); order_node_idx++) {
		auto &bound_order = orders[order_node_idx];
		auto &order_expression = *bound_order.expression;
		if (order_expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			continue; // Will be compressed generically
		}

		// Mark the bindings referenced by the non-colref expression so they won't be modified
		GetReferencedBindings(order_expression, referenced_bindings);
	}

	// Create info for compression
	CompressedMaterializationInfo info(*op, {0}, referenced_bindings);

	// Create binding mapping
	const auto bindings = op->GetColumnBindings();
	const auto &types = op->types;
	D_ASSERT(bindings.size() == types.size());
	for (idx_t col_idx = 0; col_idx < bindings.size(); col_idx++) {
		// Ordering does not change bindings, input binding is output binding
		info.binding_map.emplace(bindings[col_idx], CMBindingInfo(bindings[col_idx], types[col_idx]));
	}

	// Now try to compress
	CreateProjections(op, info);
}

void CompressedMaterialization::CompressOrder(unique_ptr<LogicalOperator> &op) {
	CompressOrdering(op, op->Cast<LogicalOrder>().orders, column_binding_set_t());
	UpdateOrderStats(op);
}

void CompressedMaterialization::CompressTopN(unique_ptr<LogicalOperator> &op) {
	auto &topn = op->Cast<LogicalTopN>();
	// top-N is compressed for NUMERIC columns, whose sort key is the canonical encoding (as for VARCHAR) while their
	// values compress to a fixed-size integer; other types keep the plan shape they have without compression, as
	// optimizer extensions (an index scan replacing a top-N over a distance, say) match the operators around a top-N
	bool numeric = false;
	for (auto &type : op->types) {
		numeric = numeric || TypeVisitor::Contains(type, LogicalTypeId::NUMERIC);
	}
	if (!numeric) {
		return;
	}
	column_binding_set_t referenced_bindings;
	if (topn.dynamic_filter) {
		// the boundary of the first order column is pushed into the scan as a filter: it must keep its type
		GetReferencedBindings(*topn.orders[0].expression, referenced_bindings);
	}
	CompressOrdering(op, topn.orders, std::move(referenced_bindings));
	UpdateTopNStats(op);
}

void CompressedMaterialization::UpdateOrderingStats(vector<BoundOrderByNode> &orders) {
	for (idx_t order_node_idx = 0; order_node_idx < orders.size(); order_node_idx++) {
		auto &bound_order = orders[order_node_idx];
		auto &order_expression = *bound_order.expression;
		if (order_expression.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		auto &colref = order_expression.Cast<BoundColumnRefExpression>();
		auto it = statistics_map.find(colref.Binding());
		if (it != statistics_map.end() && it->second) {
			bound_order.stats = it->second->ToUnique();
		}
	}
}

void CompressedMaterialization::UpdateOrderStats(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}
	// Update order stats if compressed
	UpdateOrderingStats(op->children[0]->Cast<LogicalOrder>().orders);
}

void CompressedMaterialization::UpdateTopNStats(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}
	UpdateOrderingStats(op->children[0]->Cast<LogicalTopN>().orders);
}

} // namespace duckdb
