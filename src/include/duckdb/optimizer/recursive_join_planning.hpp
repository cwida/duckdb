//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/recursive_join_planning.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/join_build_properties.hpp"

namespace duckdb {

struct RecursiveJoinProperties {
	JoinBuildProperties build;
	bool depends_on_recursive_input = false;

	static RecursiveJoinProperties Extract(LogicalOperator &op, const unordered_set<TableIndex> &active_ctes);
};

bool CanPlaceRecursiveProbe(JoinType type, const RecursiveJoinProperties &probe_side,
                            const RecursiveJoinProperties &build_side);

} // namespace duckdb
