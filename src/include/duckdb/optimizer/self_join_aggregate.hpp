//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/self_join_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer.hpp"

namespace duckdb {

//! Rewrites a self-join of one table on equal keys and exactly one inequality, read only through its keys by a
//! parent that ignores duplicates, into GROUP BY keys HAVING min(x) <> max(x) over a single scan.
//! See self_join_aggregate.cpp for the conditions and the argument that the rewrite is exact.
class SelfJoinAggregate {
public:
	explicit SelfJoinAggregate(Optimizer &optimizer);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	Optimizer &optimizer;
};

} // namespace duckdb
