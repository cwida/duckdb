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
//! LIST(x ORDER BY x) -> LIST_SORT(LIST(x))
//! F(CASE WHEN c THEN x END) -> F(x) FILTER (WHERE c), as a separate pass (RewriteCaseToFilter)
class AggregateFunctionRewriter {
public:
	explicit AggregateFunctionRewriter(Optimizer &optimizer);
	~AggregateFunctionRewriter();

	void Optimize(unique_ptr<LogicalOperator> &op);
	//! Rewrites f(CASE WHEN c THEN x END) -> f(x) FILTER (WHERE c)
	void RewriteCaseToFilter(unique_ptr<LogicalOperator> &op);

private:
	Optimizer &optimizer;
};

} // namespace duckdb
