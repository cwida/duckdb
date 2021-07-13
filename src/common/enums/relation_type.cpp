#include "duckdb/common/enums/relation_type.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

// LCOV_EXCL_START
string RelationTypeToString(RelationType type) {
	switch (type) {
	case RelationType::TABLE_RELATION:
		return "TABLE_RELATION";
	case RelationType::PROJECTION_RELATION:
		return "PROJECTION_RELATION";
	case RelationType::FILTER_RELATION:
		return "FILTER_RELATION";
	case RelationType::EXPLAIN_RELATION:
		return "EXPLAIN_RELATION";
	case RelationType::CROSS_PRODUCT_RELATION:
		return "CROSS_PRODUCT_RELATION";
	case RelationType::JOIN_RELATION:
		return "JOIN_RELATION";
	case RelationType::AGGREGATE_RELATION:
		return "AGGREGATE_RELATION";
	case RelationType::SET_OPERATION_RELATION:
		return "SET_OPERATION_RELATION";
	case RelationType::DISTINCT_RELATION:
		return "DISTINCT_RELATION";
	case RelationType::LIMIT_RELATION:
		return "LIMIT_RELATION";
	case RelationType::ORDER_RELATION:
		return "ORDER_RELATION";
	case RelationType::CREATE_VIEW_RELATION:
		return "CREATE_VIEW_RELATION";
	case RelationType::CREATE_TABLE_RELATION:
		return "CREATE_TABLE_RELATION";
	case RelationType::INSERT_RELATION:
		return "INSERT_RELATION";
	case RelationType::VALUE_LIST_RELATION:
		return "VALUE_LIST_RELATION";
	case RelationType::DELETE_RELATION:
		return "DELETE_RELATION";
	case RelationType::UPDATE_RELATION:
		return "UPDATE_RELATION";
	case RelationType::WRITE_CSV_RELATION:
		return "WRITE_CSV_RELATION";
	case RelationType::READ_CSV_RELATION:
		return "READ_CSV_RELATION";
	case RelationType::SUBQUERY_RELATION:
		return "SUBQUERY_RELATION";
	case RelationType::TABLE_FUNCTION_RELATION:
		return "TABLE_FUNCTION_RELATION";
	case RelationType::VIEW_RELATION:
		return "VIEW_RELATION";
	case RelationType::QUERY_RELATION:
		return "QUERY_RELATION";
	case RelationType::INVALID_RELATION:
		break;
	}
	return "INVALID_RELATION";
}
// LCOV_EXCL_STOP

} // namespace duckdb
