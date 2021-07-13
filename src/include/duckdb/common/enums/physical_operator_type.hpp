//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/physical_operator_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Physical Operator Types
//===--------------------------------------------------------------------===//
enum class PhysicalOperatorType : uint8_t {
	INVALID,
	ORDER_BY,
	LIMIT,
	TOP_N,
	WINDOW,
	UNNEST,
	SIMPLE_AGGREGATE,
	HASH_GROUP_BY,
	PERFECT_HASH_GROUP_BY,
	FILTER,
	PROJECTION,
	COPY_TO_FILE,
	RESERVOIR_SAMPLE,
	STREAMING_SAMPLE,
	// -----------------------------
	// Scans
	// -----------------------------
	TABLE_SCAN,
	DUMMY_SCAN,
	CHUNK_SCAN,
	RECURSIVE_CTE_SCAN,
	DELIM_SCAN,
	EXPRESSION_SCAN,
	// -----------------------------
	// Joins
	// -----------------------------
	BLOCKWISE_NL_JOIN,
	NESTED_LOOP_JOIN,
	HASH_JOIN,
	CROSS_PRODUCT,
	PIECEWISE_MERGE_JOIN,
	DELIM_JOIN,
	INDEX_JOIN,
	// -----------------------------
	// SetOps
	// -----------------------------
	UNION,
	RECURSIVE_CTE,

	// -----------------------------
	// Updates
	// -----------------------------
	INSERT,
	DELETE_OPERATOR,
	UPDATE,

	// -----------------------------
	// Schema
	// -----------------------------
	CREATE_TABLE,
	CREATE_TABLE_AS,
	CREATE_INDEX,
	ALTER,
	CREATE_SEQUENCE,
	CREATE_VIEW,
	CREATE_SCHEMA,
	CREATE_MACRO,
	DROP,
	PRAGMA,
	TRANSACTION,

	// -----------------------------
	// Helpers
	// -----------------------------
	EXPLAIN,
	EMPTY_RESULT,
	EXECUTE,
	PREPARE,
	VACUUM,
	EXPORT,
	SET,
	LOAD,
	INOUT_FUNCTION
};

string PhysicalOperatorToString(PhysicalOperatorType type);

} // namespace duckdb
