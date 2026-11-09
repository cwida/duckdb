//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_fusion.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

class ClientContext;

class WindowFusion : public LogicalOperatorVisitor {
public:
	WindowFusion(ClientContext &client, optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op);

	ClientContext &client;
	optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats;
};

} // namespace duckdb
