//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/remove_redundant_distinct.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class LogicalOperator;

//! Removes duplicate eliminations whose input is already distinct. A duplicate elimination is an aggregate that only
//! has groups (DISTINCT, GROUP BY without aggregate functions). If its input has at most one row per value of the
//! groups, it is replaced by a projection of the groups.
class RemoveRedundantDistinct {
public:
	void VisitOperator(unique_ptr<LogicalOperator> &op);
};

} // namespace duckdb
