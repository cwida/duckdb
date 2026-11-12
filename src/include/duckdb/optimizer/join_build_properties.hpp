//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_build_properties.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

struct JoinBuildProperties {
	bool correlation_sensitive = false;
	//! Safe to treat the subtree as repeatable for unchanged inputs.
	bool repeatable = true;

	static JoinBuildProperties Extract(LogicalOperator &op);
};

} // namespace duckdb
