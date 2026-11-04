//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/duplicate_eliminated_domain_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"

namespace duckdb {
class Optimizer;
class LogicalAggregate;
class LogicalMaterializedCTE;

//! Restrict local grouped aggregates to their consumer's demand keys.
class DuplicateEliminatedDomainOptimizer : public LogicalOperatorVisitor {
public:
	explicit DuplicateEliminatedDomainOptimizer(Optimizer &optimizer);

	void Optimize(unique_ptr<LogicalOperator> &op);

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op) override;
	bool TryOptimizeDomain(unique_ptr<LogicalOperator> &op, idx_t aggregate_side);
	bool TryOptimizeRuntimeDomain(unique_ptr<LogicalOperator> &op, LogicalAggregate &aggregate, idx_t aggregate_side,
	                              idx_t demand_index);

	Optimizer &optimizer;
	BindingReplacementGraph replacements;
	vector<optional_ptr<LogicalMaterializedCTE>> cte_scopes;
};
} // namespace duckdb
