//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/connection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <type_traits>

#include "duckdb/main/profiler/profiler_print_format.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class ColumnDataCollection;
class ClientContext;

class DatabaseInstance;
class DuckDB;
class LogicalOperator;
class ResultFormat;
class SelectStatement;
struct CSVReaderOptions;
struct TableFunctionInfo;

typedef void (*warning_callback_t)(std::string);

//! A connection to a database. This represents a (client) connection that can
//! be used to query the database.
class Connection {
public:
	DUCKDB_API explicit Connection(DuckDB &database);
	DUCKDB_API explicit Connection(DatabaseInstance &database);
	DUCKDB_API static Connection CreateInternal(DatabaseInstance &database);
	//! Creates an internal connection that runs on behalf of `caller`: it follows the interrupts and the query
	//! deadline of caller's base client
	DUCKDB_API static Connection CreateInternal(ClientContext &caller);
	// disable copy constructors
	Connection(const Connection &other) = delete;
	Connection &operator=(const Connection &) = delete;
	//! enable move constructors
	DUCKDB_API Connection(Connection &&other) noexcept;
	DUCKDB_API Connection &operator=(Connection &&) noexcept;
	DUCKDB_API ~Connection();

	shared_ptr<ClientContext> context;

private:
	Connection(DatabaseInstance &database, ConnectionType connection_type);

public:
	//! Returns query profiling information for the current query, formatted according to the given ProfilerPrintFormat
	//! (e.g. ProfilerPrintFormat::JSON()). ProfilerPrintFormat::Default() uses the configured default profiler format.
	DUCKDB_API string GetProfilingInformation(const ProfilerPrintFormat &format = ProfilerPrintFormat::Default());

	//! Interrupts the current query. Async-signal-safe
	DUCKDB_API void Interrupt();

	//! Get query progress of current query
	DUCKDB_API double GetQueryProgress();

	//! Enable query profiling
	DUCKDB_API void EnableProfiling();
	//! Disable query profiling
	DUCKDB_API void DisableProfiling();

	[[deprecated("Query verification has been replaced by individual verification routines - this method does nothing "
	             "anymore")]] DUCKDB_API void
	EnableQueryVerification();
	[[deprecated("Query verification has been replaced by individual verification routines - this method does nothing "
	             "anymore")]] DUCKDB_API void
	DisableQueryVerification();
	//! Force parallel execution, even for smaller tables. Should only be used in testing.
	DUCKDB_API void ForceParallelism();

	//! Blocking. Runs the query to completion and returns its handle. The result is retained: it can be read
	//! repeatedly and at random.
	DUCKDB_API unique_ptr<QueryResult> Query(const string &query);
	DUCKDB_API unique_ptr<QueryResult> Query(unique_ptr<SQLStatement> statement,
	                                         shared_ptr<ResultFormat> format = nullptr);
	//! As above, in the given result format
	DUCKDB_API unique_ptr<QueryResult> Query(const string &query, shared_ptr<ResultFormat> format);
	// prepared statements
	template <typename... ARGS>
	unique_ptr<QueryResult> Query(const string &query, ARGS... args) {
		vector<Value> values;
		return QueryParamsRecursive(query, values, args...);
	}
	//! Overload resolution otherwise prefers the prepared-statement template above for a derived format pointer
	template <class FORMAT, typename std::enable_if<std::is_base_of<ResultFormat, FORMAT>::value, int>::type = 0>
	unique_ptr<QueryResult> Query(const string &query, shared_ptr<FORMAT> format) {
		return Query(query, shared_ptr<ResultFormat>(std::move(format)));
	}

	//! Non-blocking. Submits the query and returns its handle. The engine runs it iff threads - external_threads > 0,
	//! but produces no data until the caller either calls a materializing method on the handle or opens a
	//! QueryResultStream on it. The query may only contain a single statement.
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, const QueryParameters &query_parameters = {});
	//! Non-blocking. As above, for a parsed statement and for bound parameter values
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement,
	                                          const QueryParameters &query_parameters = {});
	//! Non-blocking. As above, in the given result format
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, shared_ptr<ResultFormat> format);
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format);
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement,
	                                          identifier_map_t<BoundParameterData> &named_values,
	                                          const QueryParameters &query_parameters = {});
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, identifier_map_t<BoundParameterData> &named_values,
	                                          const QueryParameters &query_parameters = {});
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, vector<Value> &values,
	                                          const QueryParameters &query_parameters = {});
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement, vector<Value> &values,
	                                          const QueryParameters &query_parameters = {});

	//! Prepare the specified query, returning a prepared statement object
	DUCKDB_API unique_ptr<PreparedStatement> Prepare(const string &query);
	//! Prepare the specified statement, returning a prepared statement object
	DUCKDB_API unique_ptr<PreparedStatement> Prepare(unique_ptr<SQLStatement> statement);

	//! Get the table info of a specific table, or nullptr if it cannot be found.
	DUCKDB_API unique_ptr<TableDescription> TableInfo(const Identifier &database_name, const Identifier &schema_name,
	                                                  const Identifier &table_name);
	//! Get the table info of a specific table, or nullptr if it cannot be found. Uses INVALID_CATALOG.
	DUCKDB_API unique_ptr<TableDescription> TableInfo(const Identifier &schema_name, const Identifier &table_name);
	//! Get the table info of a specific table, or nullptr if it cannot be found. Uses INVALID_CATALOG and
	//! DEFAULT_SCHEMA.
	DUCKDB_API unique_ptr<TableDescription> TableInfo(const Identifier &table_name);

	//! Extract the user statements of a script as parsed, without expanding any of them. apply_invalidation_policy
	//! works as for ClientContext::IterateStatements.
	DUCKDB_API vector<unique_ptr<SQLStatement>> ExtractStatements(const string &query,
	                                                              bool apply_invalidation_policy = true);
	//! Extract the logical plan that corresponds to a query
	DUCKDB_API unique_ptr<LogicalOperator> ExtractPlan(const string &query);

	//! Appends a ColumnDataCollection to the described table.
	DUCKDB_API void Append(TableDescription &description, ColumnDataCollection &collection);

	//! Returns a relation that produces a table from this connection
	DUCKDB_API shared_ptr<Relation> Table(const Identifier &tname);
	DUCKDB_API shared_ptr<Relation> Table(const Identifier &schema_name, const Identifier &table_name);
	DUCKDB_API shared_ptr<Relation> Table(const Identifier &catalog_name, const Identifier &schema_name,
	                                      const Identifier &table_name);
	//! Returns a relation that produces a view from this connection
	DUCKDB_API shared_ptr<Relation> View(const Identifier &tname);
	DUCKDB_API shared_ptr<Relation> View(const Identifier &schema_name, const Identifier &table_name);
	//! Returns a relation that calls a specified table function
	DUCKDB_API shared_ptr<Relation> TableFunction(const string &tname);
	DUCKDB_API shared_ptr<Relation> TableFunction(const string &tname, const vector<Value> &values,
	                                              const named_parameter_map_t &named_parameters);
	DUCKDB_API shared_ptr<Relation> TableFunction(const string &tname, const vector<Value> &values);
	//! Calls a table function with process-local input for this invocation
	DUCKDB_API shared_ptr<Relation> TableFunction(const string &tname, const vector<Value> &values,
	                                              const named_parameter_map_t &named_parameters,
	                                              shared_ptr<TableFunctionInfo> bind_info);
	//! Returns a relation that produces values
	DUCKDB_API shared_ptr<Relation> Values(const vector<vector<Value>> &values);
	DUCKDB_API shared_ptr<Relation> Values(vector<vector<unique_ptr<ParsedExpression>>> &&values);
	DUCKDB_API shared_ptr<Relation> Values(const vector<vector<Value>> &values, const vector<string> &column_names,
	                                       const string &alias = "values");
	DUCKDB_API shared_ptr<Relation> Values(const string &values);
	DUCKDB_API shared_ptr<Relation> Values(const string &values, const vector<string> &column_names,
	                                       const string &alias = "values");

	//! Reads CSV file
	DUCKDB_API shared_ptr<Relation> ReadCSV(const string &csv_file);
	DUCKDB_API shared_ptr<Relation> ReadCSV(const vector<string> &csv_input, named_parameter_map_t &&options);
	DUCKDB_API shared_ptr<Relation> ReadCSV(const string &csv_input, named_parameter_map_t &&options);
	DUCKDB_API shared_ptr<Relation> ReadCSV(const string &csv_file, const vector<string> &columns);

	//! Reads Parquet file
	DUCKDB_API shared_ptr<Relation> ReadParquet(const string &parquet_file, bool binary_as_string);
	//! Returns a relation from a query
	DUCKDB_API shared_ptr<Relation> RelationFromQuery(const string &query, const string &alias = "queryrelation",
	                                                  const string &error = "Expected a single SELECT statement");
	DUCKDB_API shared_ptr<Relation> RelationFromQuery(unique_ptr<SelectStatement> select_stmt,
	                                                  const string &alias = "queryrelation", const string &query = "");

	DUCKDB_API void BeginTransaction();
	DUCKDB_API void Commit();
	DUCKDB_API void Rollback();
	DUCKDB_API void SetAutoCommit(bool auto_commit);
	DUCKDB_API bool IsAutoCommit();
	DUCKDB_API bool HasActiveTransaction();

	//! Fetch the set of tables names of the query.
	//! Returns the fully qualified, escaped table names, if qualified is set to true,
	//! else returns the not qualified, not escaped table names.
	DUCKDB_API unordered_set<string> GetTableNames(const string &query, const bool qualified = false);

protected:
	//! Identified used to uniquely identify connections to the database.
	connection_t connection_id;

private:
	unique_ptr<QueryResult> QueryParamsRecursive(const string &query, vector<Value> &values);

	template <typename T, typename... ARGS>
	unique_ptr<QueryResult> QueryParamsRecursive(const string &query, vector<Value> &values, T value, ARGS... args) {
		values.push_back(Value::CreateValue<T>(value));
		return QueryParamsRecursive(query, values, args...);
	}
};

} // namespace duckdb
