#include "duckdb/optimizer/window_fusion.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

WindowFusion::WindowFusion(ClientContext &context, Optimizer &optimizer,
                           optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats)
    : context(context), optimizer(optimizer), stats(stats) {
	D_ASSERT(stats);
}

unique_ptr<LogicalOperator> WindowFusion::Optimize(unique_ptr<LogicalOperator> op) {
	auto &extension_manager = context.db->GetExtensionManager();
	if (!extension_manager.ExtensionIsLoaded("core_functions")) {
		return op;
	}

	return op;
}

} // namespace duckdb
