//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/regex_range_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class Optimizer;

class RegexRangeFilter {
public:
	explicit RegexRangeFilter(ClientContext &context) : context(context) {
	}
	//! Perform filter pushdown
	unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);

private:
	ClientContext &context;
};

} // namespace duckdb
