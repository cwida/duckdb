//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_fusion.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

class ClientContext;

class WindowFusion : public LogicalOperatorVisitor {
public:
	WindowFusion(ClientContext &client, optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

	static optional_idx CanOptimize(LogicalOperator &op);

private:
	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op);

	ClientContext &client;
	optional_ptr<column_binding_map_t<unique_ptr<BaseStatistics>>> stats;
};

} // namespace duckdb
