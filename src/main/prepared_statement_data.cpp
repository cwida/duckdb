#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

namespace duckdb {

PreparedStatementData::PreparedStatementData(StatementType type) : statement_type(type) {
}

PreparedStatementData::~PreparedStatementData() {
}

void PreparedStatementData::CheckParameterCount(idx_t parameter_count) {
	const auto required = properties.parameter_count;
	if (parameter_count != required) {
		throw BinderException("Parameter/argument count mismatch for prepared statement. Expected %llu, got %llu",
		                      required, parameter_count);
	}
}

bool CheckCatalogIdentity(ClientContext &context, const Identifier &catalog_name,
                          const StatementProperties::CatalogIdentity catalog_identity) {
	// some catalogs don't support catalog version, we can't check identity in that case
	if (!catalog_identity.catalog_version.IsValid()) {
		return false;
	}
	auto database = DatabaseManager::Get(context).GetDatabase(context, catalog_name);
	if (!database) {
		throw BinderException("Prepared statement requires database %s but it was not attached", catalog_name);
	}
	Transaction::Get(context, *database);
	auto current_catalog_oid = database->GetCatalog().GetOid();
	auto current_catalog_version = database->GetCatalog().GetCatalogVersion(context);

	return StatementProperties::CatalogIdentity {current_catalog_oid, current_catalog_version} == catalog_identity;
}

static BoundParameterData GetParameterValue(ClientContext &context, const identifier_map_t<BoundParameterData> &values,
                                            const Identifier &identifier, bool allow_user_variables) {
	auto lookup = values.find(identifier);
	if (lookup != values.end()) {
		return lookup->second;
	}
	Value variable_value;
	if (allow_user_variables && ClientConfig::GetConfig(context).GetUserVariable(identifier, variable_value)) {
		return BoundParameterData(std::move(variable_value));
	}
	throw BinderException("Could not find parameter with identifier %s", identifier);
}

static identifier_map_t<idx_t> GetExpectedParameters(const bound_parameter_map_t &value_map) {
	identifier_map_t<idx_t> result;
	for (auto &entry : value_map) {
		result[entry.first] = result.size();
	}
	return result;
}

static bool HasNamedParameters(const PreparedStatementData &data) {
	return data.unbound_statement && !data.unbound_statement->named_param_map.empty();
}

static identifier_map_t<idx_t> GetExpectedParameters(const PreparedStatementData &data) {
	if (HasNamedParameters(data)) {
		return data.unbound_statement->named_param_map;
	}
	return GetExpectedParameters(data.value_map);
}

void PreparedStatementData::PopulateMissingParameterValues(ClientContext &context,
                                                           identifier_map_t<BoundParameterData> &values) const {
	const auto expected_parameters = GetExpectedParameters(*this);
	const bool allow_user_variables = HasNamedParameters(*this);
	auto verification_context = allow_user_variables ? &context : nullptr;
	PreparedStatement::VerifyParameters(values, expected_parameters, verification_context);
	for (auto &entry : expected_parameters) {
		if (values.count(entry.first)) {
			continue;
		}
		Value variable_value;
		const bool can_read_user_variable =
		    allow_user_variables && PreparedStatement::AllowsUserVariableFallback(entry.first);
		if (can_read_user_variable && ClientConfig::GetConfig(context).GetUserVariable(entry.first, variable_value)) {
			values[entry.first] = BoundParameterData(std::move(variable_value));
		}
	}
}

bool PreparedStatementData::RequireRebind(ClientContext &context,
                                          optional_ptr<identifier_map_t<BoundParameterData>> values) {
	identifier_map_t<BoundParameterData> empty_values;
	auto &parameter_values = values ? *values : empty_values;
	if (!unbound_statement) {
		throw InternalException("Prepared statement without unbound statement");
	}
	const auto expected_parameters = GetExpectedParameters(*this);
	const bool allow_user_variables = HasNamedParameters(*this);
	auto verification_context = allow_user_variables ? &context : nullptr;
	PreparedStatement::VerifyParameters(parameter_values, expected_parameters, verification_context);
	if (properties.always_require_rebind) {
		// this statement must always be re-bound
		return true;
	}
	if (!properties.bound_all_parameters) {
		// parameters not yet bound: query always requires a rebind
		return true;
	}
	for (auto &it : value_map) {
		auto &identifier = it.first;
		const bool can_read_user_variable =
		    allow_user_variables && PreparedStatement::AllowsUserVariableFallback(identifier);
		auto parameter_value = GetParameterValue(context, parameter_values, identifier, can_read_user_variable);
		if (parameter_value.GetValue().type() != it.second->return_type) {
			return true;
		}
	}
	// Check the catalog versions to ensure all catalog entries we rely on are current
	for (auto &it : properties.read_databases) {
		if (!CheckCatalogIdentity(context, it.first, it.second)) {
			return true;
		}
	}
	for (auto &it : properties.modified_databases) {
		if (!CheckCatalogIdentity(context, it.first, it.second.identity)) {
			return true;
		}
	}
	return false;
}

void PreparedStatementData::Bind(ClientContext &context, const identifier_map_t<BoundParameterData> &values) {
	// set parameters
	D_ASSERT(!unbound_statement || unbound_statement->named_param_map.size() == properties.parameter_count);
	if (unbound_statement || !value_map.empty()) {
		const auto expected_parameters = GetExpectedParameters(*this);
		const bool allow_user_variables = HasNamedParameters(*this);
		auto verification_context = allow_user_variables ? &context : nullptr;
		PreparedStatement::VerifyParameters(values, expected_parameters, verification_context);
	} else if (!values.empty()) {
		CheckParameterCount(values.size());
	}

	// bind the required values
	const bool allow_user_variables = HasNamedParameters(*this);
	for (auto &it : value_map) {
		const string &identifier = it.first.GetIdentifierName();
		const bool can_read_user_variable =
		    allow_user_variables && PreparedStatement::AllowsUserVariableFallback(it.first);
		auto parameter_value = GetParameterValue(context, values, it.first, can_read_user_variable);
		D_ASSERT(it.second);
		const auto &value = parameter_value.GetValue();
		auto cast_value = value.TryCastAs(context, it.second->return_type);
		if (!cast_value) {
			throw BinderException(
			    "Type mismatch for binding parameter with identifier %s, expected type %s but got type %s", identifier,
			    it.second->return_type.ToString().c_str(), value.type().ToString().c_str());
		}
		it.second->SetValue(std::move(*cast_value));
	}
}

bool PreparedStatementData::TryGetType(const Identifier &identifier, LogicalType &result) {
	auto it = value_map.find(identifier);
	if (it == value_map.end()) {
		return false;
	}
	if (it->second->return_type.id() != LogicalTypeId::INVALID) {
		result = it->second->return_type;
	} else {
		result = it->second->GetValue().type();
	}
	return true;
}

LogicalType PreparedStatementData::GetType(const Identifier &identifier) {
	LogicalType result;
	if (!TryGetType(identifier, result)) {
		throw BinderException("Could not find parameter identified with: %s", identifier);
	}
	return result;
}

} // namespace duckdb
