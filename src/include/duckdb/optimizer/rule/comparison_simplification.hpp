//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/comparison_simplification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

// The Comparison Simplification rule rewrites comparisons with a constant NULL (i.e. [x = NULL] => [NULL])
class ComparisonSimplificationRule : public Rule {
public:
	explicit ComparisonSimplificationRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

//! Rewrites row comparisons (`=` and `IS NOT DISTINCT FROM`), with a row constructor or a folded row constant on
//! either side, into per-field comparisons using IS NOT DISTINCT FROM - row comparisons are defined that way.
class RowComparisonSimplificationRule : public Rule {
public:
	explicit RowComparisonSimplificationRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
