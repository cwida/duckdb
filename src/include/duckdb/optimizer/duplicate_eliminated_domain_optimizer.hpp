//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/duplicate_eliminated_domain_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {
class LogicalOperator;
class Optimizer;

//! Restrict local grouped aggregates to their consumer's fixed demand keys.
class DuplicateEliminatedDomainOptimizer {
public:
	static void Optimize(Optimizer &optimizer, LogicalOperator &plan);
};
} // namespace duckdb
