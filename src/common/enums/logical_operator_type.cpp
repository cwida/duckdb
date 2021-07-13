#include "duckdb/common/enums/logical_operator_type.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Value <--> String Utilities
//===--------------------------------------------------------------------===//
// LCOV_EXCL_START
string LogicalOperatorToString(LogicalOperatorType type) {
	switch (type) {
	case LogicalOperatorType::LOGICAL_GET:
		return "GET";
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
		return "CHUNK_GET";
	case LogicalOperatorType::LOGICAL_DELIM_GET:
		return "DELIM_GET";
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return "EMPTY_RESULT";
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		return "EXPRESSION_GET";
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
		return "ANY_JOIN";
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		return "COMPARISON_JOIN";
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		return "DELIM_JOIN";
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return "PROJECTION";
	case LogicalOperatorType::LOGICAL_FILTER:
		return "FILTER";
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		return "AGGREGATE";
	case LogicalOperatorType::LOGICAL_WINDOW:
		return "WINDOW";
	case LogicalOperatorType::LOGICAL_UNNEST:
		return "UNNEST";
	case LogicalOperatorType::LOGICAL_LIMIT:
		return "LIMIT";
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		return "ORDER_BY";
	case LogicalOperatorType::LOGICAL_TOP_N:
		return "TOP_N";
	case LogicalOperatorType::LOGICAL_SAMPLE:
		return "SAMPLE";
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		return "COPY_TO_FILE";
	case LogicalOperatorType::LOGICAL_JOIN:
		return "JOIN";
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return "CROSS_PRODUCT";
	case LogicalOperatorType::LOGICAL_UNION:
		return "UNION";
	case LogicalOperatorType::LOGICAL_EXCEPT:
		return "EXCEPT";
	case LogicalOperatorType::LOGICAL_INTERSECT:
		return "INTERSECT";
	case LogicalOperatorType::LOGICAL_INSERT:
		return "INSERT";
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return "DISTINCT";
	case LogicalOperatorType::LOGICAL_DELETE:
		return "DELETE";
	case LogicalOperatorType::LOGICAL_UPDATE:
		return "UPDATE";
	case LogicalOperatorType::LOGICAL_PREPARE:
		return "PREPARE";
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		return "DUMMY_SCAN";
	case LogicalOperatorType::LOGICAL_CREATE_INDEX:
		return "CREATE_INDEX";
	case LogicalOperatorType::LOGICAL_CREATE_TABLE:
		return "CREATE_TABLE";
	case LogicalOperatorType::LOGICAL_CREATE_MACRO:
		return "CREATE_MACRO";
	case LogicalOperatorType::LOGICAL_EXPLAIN:
		return "EXPLAIN";
	case LogicalOperatorType::LOGICAL_EXECUTE:
		return "EXECUTE";
	case LogicalOperatorType::LOGICAL_VACUUM:
		return "VACUUM";
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
		return "REC_CTE";
	case LogicalOperatorType::LOGICAL_CTE_REF:
		return "CTE_SCAN";
	case LogicalOperatorType::LOGICAL_SHOW:
		return "SHOW";
	case LogicalOperatorType::LOGICAL_ALTER:
		return "ALTER";
	case LogicalOperatorType::LOGICAL_CREATE_SEQUENCE:
		return "CREATE_SEQUENCE";
	case LogicalOperatorType::LOGICAL_CREATE_VIEW:
		return "CREATE_VIEW";
	case LogicalOperatorType::LOGICAL_CREATE_SCHEMA:
		return "CREATE_SCHEMA";
	case LogicalOperatorType::LOGICAL_DROP:
		return "DROP";
	case LogicalOperatorType::LOGICAL_PRAGMA:
		return "PRAGMA";
	case LogicalOperatorType::LOGICAL_TRANSACTION:
		return "TRANSACTION";
	case LogicalOperatorType::LOGICAL_EXPORT:
		return "EXPORT";
	case LogicalOperatorType::LOGICAL_SET:
		return "SET";
	case LogicalOperatorType::LOGICAL_LOAD:
		return "LOAD";
	case LogicalOperatorType::LOGICAL_INVALID:
		break;
	}
	return "INVALID";
}
// LCOV_EXCL_STOP

} // namespace duckdb
