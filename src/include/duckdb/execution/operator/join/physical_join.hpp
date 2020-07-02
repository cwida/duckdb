//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/physical_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

//! PhysicalJoin represents the base class of the join operators
class PhysicalJoin : public PhysicalSink {
public:
	PhysicalJoin(LogicalOperator &op, PhysicalOperatorType type, JoinType join_type);

	JoinType join_type;

public:
	template <bool MATCH>
	static void ConstructSemiOrAntiJoinResult(DataChunk &left, DataChunk &result, bool found_match[]);
	static void ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &left, DataChunk &result, bool found_match[],
	                                    bool has_null);
    static void ConstructLeftJoinResult(DataChunk &left, DataChunk &result, bool found_match[]);
};

} // namespace duckdb
