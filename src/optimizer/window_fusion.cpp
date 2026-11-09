#include "duckdb/optimizer/window_fusion.hpp"

#include "duckdb/optimizer/topn_window_elimination.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/execution/operator/aggregate/physical_streaming_window.hpp"

namespace duckdb {

WindowFusion::WindowFusion(ClientContext &client, optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats)
    : client(client), stats(stats) {
	D_ASSERT(stats);
}

unique_ptr<LogicalOperator> WindowFusion::OptimizeInternal(unique_ptr<LogicalOperator> op) {
	//	We are not changing any bindings, just fusing a particular filter into an existing window
	//	and removing the filter. So we can just visit children and return the current operator (or its child.)
	for (auto &child : op->children) {
		child = OptimizeInternal(std::move(child));
	}

	const auto partition_limit = TopNWindowElimination::CanOptimize(*op, true);
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

	//	Return the Filter's child.
	return std::move(filter.children[0]);
}

unique_ptr<LogicalOperator> WindowFusion::Optimize(unique_ptr<LogicalOperator> op) {
	auto &extension_manager = client.db->GetExtensionManager();
	if (!extension_manager.ExtensionIsLoaded("core_functions")) {
		return op;
	}

	return OptimizeInternal(std::move(op));
}

} // namespace duckdb
