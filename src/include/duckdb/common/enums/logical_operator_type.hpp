//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/logical_operator_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Logical Operator Types
//===--------------------------------------------------------------------===//
enum class LogicalOperatorType : uint8_t {
	LOGICAL_INVALID = 0,
	LOGICAL_PROJECTION = 1,
	LOGICAL_FILTER = 2,
	LOGICAL_AGGREGATE_AND_GROUP_BY = 3,
	LOGICAL_WINDOW = 4,
	LOGICAL_UNNEST = 5,
	LOGICAL_LIMIT = 6,
	LOGICAL_ORDER_BY = 7,
	LOGICAL_TOP_N = 8,
	LOGICAL_COPY_TO_FILE = 10,
	LOGICAL_DISTINCT = 11,
	LOGICAL_SAMPLE = 12,
	LOGICAL_PIVOT = 14,
	LOGICAL_COPY_DATABASE = 15,
	LOGICAL_SECURE_VIEW = 16,

	// -----------------------------
	// Data sources
	// -----------------------------
	LOGICAL_GET = 25,
	LOGICAL_CHUNK_GET = 26,
	LOGICAL_DELIM_GET = 27,
	LOGICAL_EXPRESSION_GET = 28,
	LOGICAL_DUMMY_SCAN = 29,
	LOGICAL_EMPTY_RESULT = 30,
	LOGICAL_CTE_REF = 31,
	// -----------------------------
	// Joins
	// -----------------------------
	LOGICAL_JOIN = 50,
	LOGICAL_DELIM_JOIN = 51,
	LOGICAL_COMPARISON_JOIN = 52,
	LOGICAL_ANY_JOIN = 53,
	LOGICAL_CROSS_PRODUCT = 54,
	LOGICAL_POSITIONAL_JOIN = 55,
	LOGICAL_ASOF_JOIN = 56,
	LOGICAL_DEPENDENT_JOIN = 57,
	// -----------------------------
	// SetOps
	// -----------------------------
	LOGICAL_UNION = 75,
	LOGICAL_EXCEPT = 76,
	LOGICAL_INTERSECT = 77,
	LOGICAL_RECURSIVE_CTE = 78,
	LOGICAL_MATERIALIZED_CTE = 79,

	// -----------------------------
	// Updates
	// -----------------------------
	LOGICAL_INSERT = 100,
	LOGICAL_DELETE = 101,
	LOGICAL_UPDATE = 102,
	LOGICAL_MERGE_INTO = 103,
	LOGICAL_TRIGGER = 104,

	// -----------------------------
	// Schema
	// -----------------------------
	LOGICAL_ALTER = 125,
	LOGICAL_CREATE_TABLE = 126,
	LOGICAL_CREATE_INDEX = 127,
	LOGICAL_CREATE_SEQUENCE = 128,
	LOGICAL_CREATE_VIEW = 129,
	LOGICAL_CREATE_SCHEMA = 130,
	LOGICAL_CREATE_MACRO = 131,
	LOGICAL_DROP = 132,
	LOGICAL_PRAGMA = 133,
	LOGICAL_TRANSACTION = 134,
	LOGICAL_CREATE_TYPE = 135,
	LOGICAL_ATTACH = 136,
	LOGICAL_DETACH = 137,
	LOGICAL_CREATE_TRIGGER = 138,

	// -----------------------------
	// Explain
	// -----------------------------
	LOGICAL_EXPLAIN = 150,

	// -----------------------------
	// Helpers
	// -----------------------------
	LOGICAL_PREPARE = 175,
	LOGICAL_EXPORT = 177,
	LOGICAL_VACUUM = 178,
	LOGICAL_SET = 179,
	LOGICAL_LOAD = 180,
	LOGICAL_RESET = 181,
	LOGICAL_UPDATE_EXTENSIONS = 182,
	LOGICAL_CONNECT = 183,
	LOGICAL_DISCONNECT = 184,
	LOGICAL_EXTERNAL_RESOURCE = 185,

	// -----------------------------
	// Secrets
	// -----------------------------
	LOGICAL_CREATE_SECRET = 190,

	LOGICAL_EXTENSION_OPERATOR = 255
};

DUCKDB_API string LogicalOperatorToString(LogicalOperatorType type);

} // namespace duckdb
