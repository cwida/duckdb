#include "duckdb/optimizer/window_fusion.hpp"

#include "duckdb/optimizer/topn_window_elimination.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/execution/operator/aggregate/physical_streaming_window.hpp"

namespace duckdb {

WindowFusion::WindowFusion(ClientContext &client, optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats)
    : client(client), stats(stats) {
	D_ASSERT(stats);
}

optional_idx WindowFusion::CanOptimize(LogicalOperator &op) {
	const optional_idx cant_optimize;
	if (op.type != LogicalOperatorType::LOGICAL_FILTER) {
		return cant_optimize;
	}

	const auto &filter = op.Cast<LogicalFilter>();
	if (filter.expressions.size() != 1) {
		return cant_optimize;
	}

	const auto comparison = filter.expressions[0]->GetExpressionType();
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_EQUAL:
		break;
	default:
		return cant_optimize;
	}

	auto &filter_comparison = filter.expressions[0]->Cast<BoundFunctionExpression>();
	auto &right = BoundComparisonExpression::Right(filter_comparison);
	if (right.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return cant_optimize;
	}
	auto &filter_value = right.Cast<BoundConstantExpression>();
	if (filter_value.GetValue().type() != LogicalType::BIGINT) {
		return cant_optimize;
	}
	if (filter_value.GetValue().IsNull()) {
		return cant_optimize;
	}

	const auto bigint_value = filter_value.GetValue().GetValue<int64_t>();
	optional_idx can_optimize(0);
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (bigint_value < 1) {
			break;
		}
		can_optimize = UnsafeNumericCast<idx_t>(bigint_value);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		if (bigint_value < 2) {
			break;
		}
		can_optimize = UnsafeNumericCast<idx_t>(bigint_value - 1);
		break;
	case ExpressionType::COMPARE_EQUAL:
		//	TODO: Handle other values
		if (bigint_value < 1) {
			break;
		}
		if (bigint_value != 1) {
			return cant_optimize;
		}
		can_optimize = UnsafeNumericCast<idx_t>(bigint_value);
		break;
	default:
		return cant_optimize;
	}

	auto &left = BoundComparisonExpression::LeftMutable(filter_comparison);
	if (left->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return cant_optimize;
	}
	auto column_references = ColumnReferenceExtractor::GetReferences(&left);

	reference<LogicalOperator> child = *filter.children[0];
	while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = child.get().Cast<LogicalProjection>();
		if (column_references.size() != 1) {
			return cant_optimize;
		}

		const auto current_column_ref = column_references.begin()->first;
		D_ASSERT(current_column_ref.table_index == projection.table_index);
		// The filter has to name the window column itself. An expression that only mentions it, such as
		// row_number + 10, is a different predicate, and following the inner reference would drop that expression.
		auto &proj_expr = *projection.expressions[current_column_ref.column_index];
		if (proj_expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return cant_optimize;
		}
		column_references =
		    ColumnReferenceExtractor::GetReferences(&projection.expressions[current_column_ref.column_index]);

		child = *child.get().children[0];
	}

	if (column_references.size() != 1) {
		return cant_optimize;
	}
	const auto filter_col_idx = column_references.begin()->first.table_index;

	if (child.get().type != LogicalOperatorType::LOGICAL_WINDOW) {
		return cant_optimize;
	}
	const auto &window = child.get().Cast<LogicalWindow>();
	if (window.window_index != filter_col_idx) {
		return cant_optimize;
	}
	const auto &first_window_expr = window.expressions[0]->Cast<BoundWindowExpression>();
	for (auto &partition : first_window_expr.Partitions()) {
		if (partition->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return cant_optimize;
		}
	}
	if (window.expressions.size() != 1) {
		for (idx_t i = 1; i < window.expressions.size(); ++i) {
			if (!window.expressions[i]->Equals(first_window_expr)) {
				return cant_optimize;
			}
		}
	}
	if (window.expressions[0]->GetExpressionType() != ExpressionType::WINDOW_ROW_NUMBER) {
		return cant_optimize;
	}
	auto &window_expr = window.expressions[0]->Cast<BoundWindowExpression>();

	if (!window_expr.ArgOrders().empty()) {
		return cant_optimize;
	}

	return can_optimize;
}

unique_ptr<LogicalOperator> WindowFusion::OptimizeInternal(unique_ptr<LogicalOperator> op) {
	//	We are not changing any bindings, just fusing a particular filter into an existing window.
	//	So we can just visit children and return the current operator (or its child.)
	for (auto &child : op->children) {
		child = OptimizeInternal(std::move(child));
	}

	const auto partition_limit = WindowFusion::CanOptimize(*op);
	if (!partition_limit.IsValid()) {
		return op;
	}

	//	Remove the Filter
	D_ASSERT(op->children.size() == 1);
	auto &filter = op->Cast<LogicalFilter>();

	//	Find the child Window
	reference<LogicalOperator> child = *filter.children[0];
	while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		child = *child.get().children[0];
	}
	D_ASSERT(child.get().type == LogicalOperatorType::LOGICAL_WINDOW);
	auto &window = child.get().Cast<LogicalWindow>();

	D_ASSERT(window.expressions.size() == 1);
	auto &wexpr = window.expressions[0]->Cast<BoundWindowExpression>();

	// Restrict to implemented cases
	if (!PhysicalStreamingWindow::IsStreamingFunction(client, wexpr)) {
		return op;
	}

	//	Add the limit to the child Window
	window.partition_limit = partition_limit;

	//	Return the Filter itself (to handle BETWEEN and other situations
	//	where the window operators cannot (completely) apply the filter.)
	return op;
}

unique_ptr<LogicalOperator> WindowFusion::Optimize(unique_ptr<LogicalOperator> op) {
	auto &extension_manager = client.db->GetExtensionManager();
	if (!extension_manager.ExtensionIsLoaded("core_functions")) {
		return op;
	}

	return OptimizeInternal(std::move(op));
}

} // namespace duckdb
