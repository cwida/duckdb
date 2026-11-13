//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/tableref_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Table Reference Types
//===--------------------------------------------------------------------===//
enum class TableReferenceType : uint8_t {
	INVALID = 0,          // invalid table reference type
	BASE_TABLE = 1,       // base table reference
	SUBQUERY = 2,         // output of a subquery
	JOIN = 3,             // output of join
	TABLE_FUNCTION = 5,   // table producing function
	EXPRESSION_LIST = 6,  // expression list
	CTE = 7,              // Recursive CTE
	EMPTY_FROM = 8,       // placeholder for empty FROM
	PIVOT = 9,            // pivot statement
	SHOW_REF = 10,        // SHOW statement
	COLUMN_DATA = 11,     // column data collection
	DELIM_GET = 12,       // Delim get ref
	BOUND_TABLE_REF = 13, // Bound table ref
	MATCH_RECOGNIZE = 14  // MATCH_RECOGNIZE
};

} // namespace duckdb
