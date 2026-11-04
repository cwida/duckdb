//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/remove_redundant_order.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! Removes ORDER BY operators whose output order is discarded by the operators above them, e.g. a sorted subquery
//! below a GROUP BY, a DISTINCT, a join, a set operation or another ORDER BY.
class RemoveRedundantOrder {
public:
	void Optimize(unique_ptr<LogicalOperator> &op);

private:
	//! Visit op; order_discarded is true when no operator above op reads the order of its output
	void Optimize(unique_ptr<LogicalOperator> &op, bool order_discarded);
};

} // namespace duckdb
