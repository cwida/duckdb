#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"

namespace duckdb {

PreparedStatement::PreparedStatement(const shared_ptr<ClientContext> &context_p, string name_p, string query_p,
                                     PreparedStatementInfo info_p)
    : context(context_p), name(std::move(name_p)), query(std::move(query_p)), success(true), info(std::move(info_p)) {
	D_ASSERT(!context.expired());
}

PreparedStatement::PreparedStatement(const shared_ptr<ClientContext> &context_p, unique_ptr<SQLStatement> statement)
    : context(context_p), query(statement->query), success(true), written_statement(std::move(statement)) {
	D_ASSERT(!context.expired());
	// The placeholder the planner reports for a result known only at execution. The properties describe nothing
	info.names = {Identifier("unknown")};
	info.types = {LogicalTypeId::UNKNOWN};
	info.statement_type = written_statement->type;
	info.properties.always_require_rebind = true;
}

PreparedStatement::PreparedStatement(ErrorData error) : success(false), error(std::move(error)) {
}

PreparedStatement::~PreparedStatement() {
	if (written_statement) {
		// nothing was prepared in the client context
		return;
	}
	auto client_context = context.lock();
	if (!client_context) {
		// the client context is gone - the prepared statement has been deallocated together with it
		return;
	}
	client_context->RemovePreparedStatement(name);
}

const string &PreparedStatement::GetError() {
	D_ASSERT(HasError());
	return error.Message();
}

ErrorData &PreparedStatement::GetErrorObject() {
	return error;
}

bool PreparedStatement::HasError() const {
	return !success;
}

shared_ptr<ClientContext> PreparedStatement::TryGetContext() const {
	return context.lock();
}

const string &PreparedStatement::GetName() const {
	return name;
}

const string &PreparedStatement::GetQuery() const {
	return query;
}

idx_t PreparedStatement::ColumnCount() const {
	return info.types.size();
}

StatementType PreparedStatement::GetStatementType() const {
	return info.statement_type;
}

const StatementProperties &PreparedStatement::GetStatementProperties() const {
	return info.properties;
}

const vector<LogicalType> &PreparedStatement::GetTypes() const {
	return info.types;
}

const vector<Identifier> &PreparedStatement::GetNames() const {
	return info.names;
}

const identifier_map_t<idx_t> &PreparedStatement::GetNamedParameterMap() const {
	return info.named_param_map;
}

idx_t PreparedStatement::GetParameterCount() const {
	return info.named_param_map.size();
}

bool PreparedStatement::TryGetParameterType(const Identifier &identifier, LogicalType &result) const {
	auto entry = info.parameter_types.find(identifier);
	if (entry == info.parameter_types.end()) {
		return false;
	}
	result = entry->second;
	return true;
}

case_insensitive_map_t<LogicalType> PreparedStatement::GetExpectedParameterTypes() const {
	case_insensitive_map_t<LogicalType> expected_types(info.parameter_types.size());
	for (auto &entry : info.parameter_types) {
		expected_types[entry.first.GetIdentifierName()] = entry.second;
	}
	return expected_types;
}

bool PreparedStatement::MetadataAvailable() const {
	return !written_statement && info.properties.bound_all_parameters;
}

unique_ptr<SQLStatement> PreparedStatement::CreateStatement(identifier_map_t<BoundParameterData> &named_values,
                                                            QueryParameters &parameters) const {
	if (written_statement) {
		if (!named_values.empty()) {
			parameters.statement_args = named_values;
		}
		return written_statement->Copy();
	}
	auto execute = make_uniq<ExecuteStatement>();
	execute->name = Identifier(name);
	// report the query that was prepared - not the generated EXECUTE - in errors and profiling output
	execute->query = query;
	execute->stmt_location = QueryLocation(0, query.size());
	// the values are already typed - pass them in pre-bound instead of as SQL literals
	execute->bound_values = named_values;
	return std::move(execute);
}

unique_ptr<QueryResult> PreparedStatement::Execute(identifier_map_t<BoundParameterData> &named_values,
                                                   const QueryParameters &query_parameters) {
	if (!success) {
		return make_uniq<QueryResult>(
		    ErrorData(InvalidInputException("Attempting to execute an unsuccessfully prepared statement!")));
	}
	auto client_context = context.lock();
	if (!client_context) {
		return make_uniq<QueryResult>(ErrorData(
		    InvalidInputException("Attempting to execute a prepared statement after its connection was closed!")));
	}
	auto parameters = query_parameters;
	auto statement = CreateStatement(named_values, parameters);
	return client_context->RunInternalStatement(std::move(statement), parameters);
}

unique_ptr<QueryResult> PreparedStatement::Execute(vector<Value> &values, const QueryParameters &query_parameters) {
	identifier_map_t<BoundParameterData> named_values;
	for (idx_t i = 0; i < values.size(); i++) {
		named_values[Identifier(std::to_string(i + 1))] = BoundParameterData(values[i]);
	}
	return Execute(named_values, query_parameters);
}

unique_ptr<QueryResult> PreparedStatement::Submit(vector<Value> &values, const QueryParameters &query_parameters) {
	identifier_map_t<BoundParameterData> named_values;
	for (idx_t i = 0; i < values.size(); i++) {
		auto &val = values[i];
		named_values[Identifier(std::to_string(i + 1))] = BoundParameterData(val);
	}
	return Submit(named_values, query_parameters);
}

unique_ptr<QueryResult> PreparedStatement::Submit(identifier_map_t<BoundParameterData> &named_values,
                                                  const QueryParameters &query_parameters) {
	if (!success) {
		auto exception = InvalidInputException("Attempting to execute an unsuccessfully prepared statement!");
		return make_uniq<QueryResult>(ErrorData(exception));
	}
	auto client_context = context.lock();
	if (!client_context) {
		auto exception =
		    InvalidInputException("Attempting to execute a prepared statement after its connection was closed!");
		return make_uniq<QueryResult>(ErrorData(exception));
	}
	auto parameters = query_parameters;
	auto statement = CreateStatement(named_values, parameters);
	return client_context->SubmitInternalStatement(std::move(statement), parameters);
}

} // namespace duckdb
