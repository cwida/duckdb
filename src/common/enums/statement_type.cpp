#include "duckdb/common/enums/statement_type.hpp"

#include "duckdb/catalog/catalog.hpp"

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
	case StatementType::COPY_DATABASE_STATEMENT:
		return "COPY_DATABASE";
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
	case StatementType::EXTENSION_STATEMENT:
		return "EXTENSION";
	case StatementType::LOGICAL_PLAN_STATEMENT:
		return "LOGICAL_PLAN";
	case StatementType::ATTACH_STATEMENT:
		return "ATTACH";
	case StatementType::DETACH_STATEMENT:
		return "DETACH";
	case StatementType::MULTI_STATEMENT:
		return "MULTI";
	case StatementType::UPDATE_EXTENSIONS_STATEMENT:
		return "UPDATE_EXTENSIONS";
	case StatementType::MERGE_INTO_STATEMENT:
		return "MERGE_INTO";
	case StatementType::CONNECT_STATEMENT:
		return "CONNECT";
	case StatementType::DISCONNECT_STATEMENT:
		return "DISCONNECT";
	case StatementType::EXTERNAL_RESOURCE_STATEMENT:
		return "EXTERNAL_RESOURCE";
	case StatementType::PASSTHROUGH_STATEMENT:
		return "PASSTHROUGH";
	case StatementType::INVALID_STATEMENT:
	case StatementType::ENUM_SIZE:
		break;
	}
	return "INVALID";
}

string StatementReturnTypeToString(StatementReturnType type) {
	switch (type) {
	case StatementReturnType::QUERY_RESULT:
		return "QUERY_RESULT";
	case StatementReturnType::CHANGED_ROWS:
		return "CHANGED_ROWS";
	case StatementReturnType::NOTHING:
		return "NOTHING";
	}
	return "INVALID";
}
// LCOV_EXCL_STOP

string StatementProperties::Difference(const StatementProperties &other) const {
	if (read_databases != other.read_databases) {
		return "read databases";
	}
	if (modified_databases.size() != other.modified_databases.size()) {
		return "modified databases";
	}
	for (auto &modified : modified_databases) {
		auto entry = other.modified_databases.find(modified.first);
		if (entry == other.modified_databases.end() || entry->second.identity != modified.second.identity ||
		    !entry->second.modifications.Contains(modified.second.modifications) ||
		    !modified.second.modifications.Contains(entry->second.modifications)) {
			return "modified databases";
		}
	}
	if (requires_valid_transaction != other.requires_valid_transaction) {
		return "requires valid transaction";
	}
	if (bound_all_parameters != other.bound_all_parameters) {
		return "bound all parameters";
	}
	if (return_type != other.return_type) {
		return "return type";
	}
	if (parameter_count != other.parameter_count) {
		return "properties parameter count";
	}
	if (always_require_rebind != other.always_require_rebind) {
		return "always require rebind";
	}
	return string();
}

void StatementProperties::RegisterDBRead(Catalog &catalog, ClientContext &context) {
	auto catalog_identity = CatalogIdentity {catalog.GetOid(), catalog.GetCatalogVersion(context)};
	D_ASSERT(read_databases.count(catalog.GetName()) == 0 || read_databases[catalog.GetName()] == catalog_identity);
	read_databases[catalog.GetName()] = catalog_identity;
}

void StatementProperties::RegisterDBModify(Catalog &catalog, ClientContext &context,
                                           DatabaseModificationType modification) {
	auto catalog_identity = CatalogIdentity {catalog.GetOid(), catalog.GetCatalogVersion(context)};
	auto entry = modified_databases.insert(make_pair(catalog.GetName(), ModificationInfo()));
	if (entry.second) {
		// new entry - set the identity
		entry.first->second.identity = catalog_identity;
	} else {
		// existing entry - verify this has the same identity
		D_ASSERT(entry.first->second.identity == catalog_identity);
	}
	entry.first->second.modifications |= modification;
}

} // namespace duckdb
