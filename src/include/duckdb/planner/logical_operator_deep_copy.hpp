//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/logical_operator_deep_copy.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/bound_tokens.hpp"
#include "duckdb/planner/logical_tokens.hpp"

#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"

namespace duckdb {

//! While it is possible to copy a logical plan using `Copy()`, both, the original and the copy
//! are—by design—identical. This includes any table_idx values etc. This is bad, when we try
//! to use part of a logical plan multiple times in the same plan.
//! The LogicalOperatorDeepCopy first copies a LogicalOperator, but then traverses the entire plan
//! and replaces all table indexes. We store a map from the original table index to the new index,
//! which we use in the TableBindingReplacer to correct all column accesses.
class LogicalOperatorDeepCopy : public LogicalOperatorVisitor {
public:
	LogicalOperatorDeepCopy(Binder &binder, optional_ptr<bound_parameter_map_t> parameter_data);

	unique_ptr<LogicalOperator> DeepCopy(const unique_ptr<LogicalOperator> &op);

private:
	void VisitOperator(LogicalOperator &op) override;

private:
	// Single-field version
	template <typename T>
	void ReplaceTableIndex(LogicalOperator &op);
	// Multi-field version
	template <typename T>
	void ReplaceTableIndexMulti(LogicalOperator &op);

private:
	Binder &binder;
	std::unordered_map<TableIndex, TableIndex> table_idx_replacements;
	optional_ptr<bound_parameter_map_t> parameter_data;
};

//! The TableBindingReplacer updates table bindings, utility for optimizers
class TableBindingReplacer : LogicalOperatorVisitor {
public:
	TableBindingReplacer(unordered_map<TableIndex, TableIndex> &table_idx_replacements,
	                     optional_ptr<bound_parameter_map_t> parameter_data);

public:
	//! Update each operator of the plan
	void VisitOperator(LogicalOperator &op) override;
	//! Visit an expression and update its column bindings
	void VisitExpression(unique_ptr<Expression> *expression) override;

public:
	//! Contains all bindings that need to be updated
	const unordered_map<TableIndex, TableIndex> &table_idx_replacements;
	optional_ptr<bound_parameter_map_t> parameter_data;
};
} // namespace duckdb
