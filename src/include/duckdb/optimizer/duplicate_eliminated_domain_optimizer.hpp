//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/duplicate_eliminated_domain_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {
class Optimizer;

//! Restrict local grouped aggregates to their consumer's fixed demand keys.
class DuplicateEliminatedDomainOptimizer : public LogicalOperatorVisitor {
public:
	explicit DuplicateEliminatedDomainOptimizer(Optimizer &optimizer);

	void Optimize(LogicalOperator &op);

private:
	void VisitOperator(LogicalOperator &op) override;

	Optimizer &optimizer;
};
} // namespace duckdb
