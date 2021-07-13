#include "duckdb/common/enums/statement_type.hpp"

namespace duckdb {

// LCOV_EXCL_START
string StatementTypeToString(StatementType type) {
	switch (type) {
	case StatementType::SELECT_STATEMENT:
		return "SELECT";
	case StatementType::INSERT_STATEMENT:
		return "INSERT";
	case StatementType::UPDATE_STATEMENT:
		return "UPDATE";
	case StatementType::DELETE_STATEMENT:
		return "DELETE";
	case StatementType::PREPARE_STATEMENT:
		return "PREPARE";
	case StatementType::EXECUTE_STATEMENT:
		return "EXECUTE";
	case StatementType::ALTER_STATEMENT:
		return "ALTER";
	case StatementType::TRANSACTION_STATEMENT:
		return "TRANSACTION";
	case StatementType::COPY_STATEMENT:
		return "COPY";
	case StatementType::ANALYZE_STATEMENT:
		return "ANALYZE";
	case StatementType::VARIABLE_SET_STATEMENT:
		return "VARIABLE_SET";
	case StatementType::CREATE_FUNC_STATEMENT:
		return "CREATE_FUNC";
	case StatementType::EXPLAIN_STATEMENT:
		return "EXPLAIN";
	case StatementType::CREATE_STATEMENT:
		return "CREATE";
	case StatementType::DROP_STATEMENT:
		return "DROP";
	case StatementType::PRAGMA_STATEMENT:
		return "PRAGMA";
	case StatementType::SHOW_STATEMENT:
		return "SHOW";
	case StatementType::VACUUM_STATEMENT:
		return "VACUUM";
	case StatementType::RELATION_STATEMENT:
		return "RELATION";
	case StatementType::EXPORT_STATEMENT:
		return "EXPORT";
	case StatementType::CALL_STATEMENT:
		return "CALL";
	case StatementType::SET_STATEMENT:
		return "SET";
	case StatementType::LOAD_STATEMENT:
		return "LOAD";
	case StatementType::INVALID_STATEMENT:
		break;
	}
	return "INVALID";
}
// LCOV_EXCL_STOP

bool StatementTypeReturnChanges(StatementType type) {
	switch (type) {
	case StatementType::INSERT_STATEMENT:
	case StatementType::UPDATE_STATEMENT:
	case StatementType::DELETE_STATEMENT:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
