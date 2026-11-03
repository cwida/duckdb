#include "duckdb/main/connection.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/relation/query_relation.hpp"
#include "duckdb/main/relation/read_csv_relation.hpp"
#include "duckdb/main/relation/table_function_relation.hpp"
#include "duckdb/main/relation/table_relation.hpp"
#include "duckdb/main/relation/value_relation.hpp"
#include "duckdb/main/relation/view_relation.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/statement_iterator.hpp"

namespace duckdb {

Connection::Connection(DatabaseInstance &database) : Connection(database, ConnectionType::USER) {
}

Connection::Connection(DatabaseInstance &database, ConnectionType connection_type)
    : context(make_shared_ptr<ClientContext>(database.shared_from_this())) {
	context->connection_type = connection_type;
	auto &connection_manager = ConnectionManager::Get(database);
	connection_manager.AssignConnectionId(*this);
	connection_manager.AddConnection(*context);
}

Connection::Connection(DuckDB &database) : Connection(*database.instance) {
}

Connection Connection::CreateInternal(DatabaseInstance &database) {
	return Connection(database, ConnectionType::INTERNAL);
}

Connection Connection::CreateInternal(ClientContext &caller) {
	auto con = CreateInternal(*caller.db);
	auto &context = *con.context;
	context.base_context = caller.base_context ? caller.base_context : caller.shared_from_this();
	context.internal_depth = caller.internal_depth + 1;
	return con;
}

Connection::Connection(Connection &&other) noexcept {
	std::swap(context, other.context);
	std::swap(connection_id, other.connection_id);
}

Connection &Connection::operator=(Connection &&other) noexcept {
	std::swap(context, other.context);
	std::swap(connection_id, other.connection_id);
	return *this;
}

Connection::~Connection() {
	if (!context) {
		return;
	}
	ConnectionManager::Get(*context->db).RemoveConnection(*context);
}

string Connection::GetProfilingInformation(const ProfilerPrintFormat &format) {
	return QueryProfiler::Get(*context).ToString(format);
}

void Connection::Interrupt() {
	context->Interrupt();
}

double Connection::GetQueryProgress() {
	return context->GetQueryProgress().GetPercentage();
}

void Connection::EnableProfiling() {
	context->EnableProfiling();
}

void Connection::DisableProfiling() {
	context->DisableProfiling();
}

void Connection::EnableQueryVerification() {
}

void Connection::DisableQueryVerification() {
}

void Connection::ForceParallelism() {
	ClientConfig::GetConfig(*context).verify_parallelism = true;
}

unique_ptr<QueryResult> Connection::Query(const string &query) {
	return context->Query(query, QueryParameters());
}

unique_ptr<QueryResult> Connection::Query(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format) {
	return context->Query(std::move(statement), std::move(format));
}

unique_ptr<QueryResult> Connection::Query(const string &query, shared_ptr<ResultFormat> format) {
	return context->Query(query, std::move(format));
}

unique_ptr<QueryResult> Connection::Submit(const string &query, const QueryParameters &query_parameters) {
	return context->Submit(query, query_parameters);
}

unique_ptr<QueryResult> Connection::Submit(unique_ptr<SQLStatement> statement,
                                           const QueryParameters &query_parameters) {
	return context->Submit(std::move(statement), query_parameters);
}

unique_ptr<QueryResult> Connection::Submit(const string &query, shared_ptr<ResultFormat> format) {
	return context->Submit(query, std::move(format));
}

unique_ptr<QueryResult> Connection::Submit(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format) {
	return context->Submit(std::move(statement), std::move(format));
}

unique_ptr<QueryResult> Connection::Submit(const string &query, identifier_map_t<BoundParameterData> &named_values,
                                           const QueryParameters &query_parameters) {
	return context->Submit(query, named_values, query_parameters);
}

unique_ptr<QueryResult> Connection::Submit(unique_ptr<SQLStatement> statement,
                                           identifier_map_t<BoundParameterData> &named_values,
                                           const QueryParameters &query_parameters) {
	return context->Submit(std::move(statement), named_values, query_parameters);
}

static identifier_map_t<BoundParameterData> ConvertParamListToMap(vector<Value> &param_list) {
	identifier_map_t<BoundParameterData> named_values;
	for (idx_t i = 0; i < param_list.size(); i++) {
		auto &val = param_list[i];
		named_values[Identifier(std::to_string(i + 1))] = BoundParameterData(val);
	}
	return named_values;
}

unique_ptr<QueryResult> Connection::Submit(const string &query, vector<Value> &values,
                                           const QueryParameters &query_parameters) {
	auto named_params = ConvertParamListToMap(values);
	return context->Submit(query, named_params, query_parameters);
}

unique_ptr<QueryResult> Connection::Submit(unique_ptr<SQLStatement> statement, vector<Value> &values,
                                           const QueryParameters &query_parameters) {
	auto named_params = ConvertParamListToMap(values);
	return context->Submit(std::move(statement), named_params, query_parameters);
}

unique_ptr<PreparedStatement> Connection::Prepare(const string &query) {
	return context->Prepare(query);
}

unique_ptr<PreparedStatement> Connection::Prepare(unique_ptr<SQLStatement> statement) {
	return context->Prepare(std::move(statement));
}

unique_ptr<QueryResult> Connection::QueryParamsRecursive(const string &query, vector<Value> &values) {
	auto named_params = ConvertParamListToMap(values);
	QueryParameters parameters;
	parameters.statement_args = named_params;
	return context->Query(query, parameters);
}

unique_ptr<TableDescription> Connection::TableInfo(const Identifier &database_name, const Identifier &schema_name,
                                                   const Identifier &table_name) {
	return context->TableInfo(database_name, schema_name, table_name);
}

unique_ptr<TableDescription> Connection::TableInfo(const Identifier &schema_name, const Identifier &table_name) {
	return TableInfo(Identifier::InvalidCatalog(), schema_name, table_name);
}

unique_ptr<TableDescription> Connection::TableInfo(const Identifier &table_name) {
	return TableInfo(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), table_name);
}

vector<unique_ptr<SQLStatement>> Connection::ExtractStatements(const string &query, bool apply_invalidation_policy) {
	auto &client_context = *context;
	vector<unique_ptr<SQLStatement>> result;
	try {
		auto iterator = client_context.IterateStatements(query, apply_invalidation_policy);
		while (iterator.Peek()) {
			result.push_back(iterator.GetStatement());
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		client_context.ProcessError(error, query);
		error.Throw();
	}
	return result;
}

unique_ptr<LogicalOperator> Connection::ExtractPlan(const string &query) {
	return context->ExtractPlan(query);
}

void Connection::Append(TableDescription &description, ColumnDataCollection &collection) {
	context->Append(description, collection);
}

shared_ptr<Relation> Connection::Table(const Identifier &table_name) {
	return Table(Identifier::DefaultSchema(), table_name);
}

shared_ptr<Relation> Connection::Table(const Identifier &schema_name, const Identifier &table_name) {
	auto table_info = TableInfo(Identifier::InvalidCatalog(), schema_name, table_name);
	if (!table_info) {
		throw CatalogException("Table %s does not exist!",
		                       QualifiedName(Identifier(), schema_name, table_name)
		                           .ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA));
	}
	return make_shared_ptr<TableRelation>(context, std::move(table_info));
}

shared_ptr<Relation> Connection::Table(const Identifier &catalog_name, const Identifier &schema_name,
                                       const Identifier &table_name) {
	unique_ptr<TableDescription> table_info;
	do {
		table_info = TableInfo(catalog_name, schema_name, table_name);
		if (table_info) {
			break;
		}

		if (catalog_name.empty() && !schema_name.empty()) {
			table_info = TableInfo(schema_name, Identifier::DefaultSchema(), table_name);
		}
	} while (false);

	if (!table_info) {
		throw CatalogException("Table %s does not exist!",
		                       QualifiedName(catalog_name, schema_name, table_name)
		                           .ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA));
	}
	return make_shared_ptr<TableRelation>(context, std::move(table_info));
}

shared_ptr<Relation> Connection::View(const Identifier &tname) {
	return View(Identifier::DefaultSchema(), tname);
}

shared_ptr<Relation> Connection::View(const Identifier &schema_name, const Identifier &table_name) {
	return make_shared_ptr<ViewRelation>(context, schema_name, table_name);
}

shared_ptr<Relation> Connection::TableFunction(const string &fname) {
	vector<Value> values;
	named_parameter_map_t named_parameters;
	return TableFunction(fname, values, named_parameters);
}

shared_ptr<Relation> Connection::TableFunction(const string &fname, const vector<Value> &values,
                                               const named_parameter_map_t &named_parameters) {
	return make_shared_ptr<TableFunctionRelation>(context, fname, values, named_parameters);
}

shared_ptr<Relation> Connection::TableFunction(const string &fname, const vector<Value> &values,
                                               const named_parameter_map_t &named_parameters,
                                               shared_ptr<TableFunctionInfo> bind_info) {
	return make_shared_ptr<TableFunctionRelation>(context, fname, values, named_parameters, nullptr, true,
	                                              std::move(bind_info));
}

shared_ptr<Relation> Connection::TableFunction(const string &fname, const vector<Value> &values) {
	return make_shared_ptr<TableFunctionRelation>(context, fname, values);
}

shared_ptr<Relation> Connection::Values(const vector<vector<Value>> &values) {
	vector<string> column_names;
	return Values(values, column_names);
}

shared_ptr<Relation> Connection::Values(vector<vector<unique_ptr<ParsedExpression>>> &&expressions) {
	vector<string> column_names;
	return make_shared_ptr<ValueRelation>(context, std::move(expressions), column_names);
}

shared_ptr<Relation> Connection::Values(const vector<vector<Value>> &values, const vector<string> &column_names,
                                        const string &alias) {
	return make_shared_ptr<ValueRelation>(context, values, column_names, alias);
}

shared_ptr<Relation> Connection::Values(const string &values) {
	vector<string> column_names;
	return Values(values, column_names);
}

shared_ptr<Relation> Connection::Values(const string &values, const vector<string> &column_names, const string &alias) {
	return make_shared_ptr<ValueRelation>(context, values, column_names, alias);
}

shared_ptr<Relation> Connection::ReadCSV(const string &csv_file) {
	named_parameter_map_t options;
	return ReadCSV(csv_file, std::move(options));
}

shared_ptr<Relation> Connection::ReadCSV(const vector<string> &csv_input, named_parameter_map_t &&options) {
	return make_shared_ptr<ReadCSVRelation>(context, csv_input, std::move(options));
}

shared_ptr<Relation> Connection::ReadCSV(const string &csv_input, named_parameter_map_t &&options) {
	vector<string> csv_files = {csv_input};
	return ReadCSV(csv_files, std::move(options));
}

shared_ptr<Relation> Connection::ReadCSV(const string &csv_file, const vector<string> &columns) {
	// parse columns
	named_parameter_map_t options;
	child_list_t<Value> column_list;
	for (auto &column : columns) {
		auto col_list = Parser(*context).ParseColumnList(column);
		if (col_list.LogicalColumnCount() != 1) {
			throw ParserException("Expected a single column definition");
		}
		auto &col_def = col_list.GetColumnMutable(LogicalIndex(0));
		column_list.emplace_back(col_def.GetName().GetIdentifierName(), col_def.GetType().ToString());
	}
	vector<string> files {csv_file};
	return make_shared_ptr<ReadCSVRelation>(context, files, std::move(options));
}

shared_ptr<Relation> Connection::ReadParquet(const string &parquet_file, bool binary_as_string) {
	vector<Value> params;
	params.emplace_back(parquet_file);
	named_parameter_map_t named_parameters({{"binary_as_string", Value::BOOLEAN(binary_as_string)}});
	return TableFunction("parquet_scan", params, named_parameters)->Alias(parquet_file);
}

unordered_set<string> Connection::GetTableNames(const string &query, const bool qualified) {
	return context->GetTableNames(query, qualified);
}

shared_ptr<Relation> Connection::RelationFromQuery(const string &query, const string &alias, const string &error) {
	return RelationFromQuery(QueryRelation::ParseStatement(*context, query, error), alias);
}

shared_ptr<Relation> Connection::RelationFromQuery(unique_ptr<SelectStatement> select_stmt, const string &alias,
                                                   const string &query_p) {
	return make_shared_ptr<QueryRelation>(context, std::move(select_stmt), alias, query_p);
}

void Connection::BeginTransaction() {
	TransactionInfo info(TransactionType::BEGIN_TRANSACTION);
	context->RunTransactionStatement(info);
}

void Connection::Commit() {
	TransactionInfo info(TransactionType::COMMIT);
	context->RunTransactionStatement(info);
}

void Connection::Rollback() {
	TransactionInfo info(TransactionType::ROLLBACK);
	context->RunTransactionStatement(info);
}

void Connection::SetAutoCommit(bool auto_commit) {
	context->transaction.SetAutoCommit(auto_commit);
}

bool Connection::IsAutoCommit() {
	return context->transaction.IsAutoCommit();
}
bool Connection::HasActiveTransaction() {
	return context->transaction.HasActiveTransaction();
}

} // namespace duckdb
