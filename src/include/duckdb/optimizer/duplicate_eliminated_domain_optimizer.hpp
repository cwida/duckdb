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

public:
	void Optimize(unique_ptr<LogicalOperator> &op);
	//! Whether domain changes preserve this statement's statistics-dependent aggregate bounds.
	static bool CanOptimize(const LogicalOperator &op);

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op) override;
	bool TryOptimizeDomain(unique_ptr<LogicalOperator> &op, idx_t aggregate_side);
	bool TryOptimizeRuntimeDomain(unique_ptr<LogicalOperator> &op, LogicalAggregate &aggregate, idx_t aggregate_side,
	                              idx_t demand_index);
	void ResolveDeferredDomain(unique_ptr<LogicalOperator> &op);

private:
	Optimizer &optimizer;
	BindingReplacementGraph replacements;
	vector<reference<LogicalMaterializedCTE>> cte_scopes;
	vector<pair<reference<LogicalOperator>, idx_t>> parent_path;
};
} // namespace duckdb
