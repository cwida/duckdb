//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_join_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class WindowJoinPushdown {
public:
	static bool CanOptimize(const LogicalOperator &op);

public:
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op);
};

} // namespace duckdb
