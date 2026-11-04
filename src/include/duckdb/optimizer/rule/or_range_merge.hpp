//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/or_range_merge.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

//! Merges range predicates on the same expression that are joined by OR into a single range:
//! (x BETWEEN 1 AND 20) OR (x BETWEEN 21 AND 40) => x BETWEEN 1 AND 40
//! Ranges merge when they overlap or touch, and for integral types also when they are adjacent (20 | 21).
class OrRangeMergeRule : public Rule {
public:
	explicit OrRangeMergeRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
