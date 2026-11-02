//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/remove_redundant_order_keys.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class RemoveRedundantOrderKeys {
public:
	void Optimize(LogicalOperator &op);
};

} // namespace duckdb
