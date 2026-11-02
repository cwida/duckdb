//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/projection_placement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

class BaseStatistics;
class LogicalOperator;
class Optimizer;

class ProjectionPlacementOptimizer {
public:
	ProjectionPlacementOptimizer(Optimizer &optimizer,
	                             column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map);

	void Optimize(unique_ptr<LogicalOperator> &plan);

private:
	Optimizer &optimizer;
	column_binding_map_t<unique_ptr<BaseStatistics>> &statistics_map;
};

} // namespace duckdb
