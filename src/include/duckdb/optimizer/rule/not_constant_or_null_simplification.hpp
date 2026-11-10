//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/not_constant_or_null_simplification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

// Rewrites NOT(constant_or_null(v, e)) into constant_or_null(!v, e):
// the per-row NULL check on e is preserved, only the constant is negated
class NotConstantOrNullSimplificationRule : public Rule {
public:
	explicit NotConstantOrNullSimplificationRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
