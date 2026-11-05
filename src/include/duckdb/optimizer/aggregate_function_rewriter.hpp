//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/aggregate_function_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {

class Optimizer;

//! Rewrites aggregate functions. Currently registered rules:
//! AVG(x) -> SUM(x) / COUNT(x)
//! SUM(x + C) -> SUM(x) + C * COUNT(x)
class AggregateFunctionRewriter {
public:
	explicit AggregateFunctionRewriter(Optimizer &optimizer);
	~AggregateFunctionRewriter();

	void Optimize(unique_ptr<LogicalOperator> &op);

private:
	Optimizer &optimizer;
};

} // namespace duckdb
