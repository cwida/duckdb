//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/enums/query_result_state.hpp"
#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/enums/prepared_statement_mode.hpp"
#include "duckdb/common/enums/connection_type.hpp"
#include "duckdb/common/enums/result_lifetime.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/main/external_dependencies.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/main/query_parameters.hpp"

namespace duckdb {
class Logger;

class Appender;
class AttachedDatabase;
class Catalog;
class CatalogSearchPath;
class ColumnDataCollection;
class DatabaseInstance;
class FileOpener;
class LogicalOperator;
class PreparedStatement;
class PreparedStatementData;
class StatementIterator;
struct PreprocessedStatement;
class Relation;
class BufferedFileWriter;
class QueryProfiler;
class ClientContextLock;
struct CreateScalarFunctionInfo;
class ScalarFunctionCatalogEntry;
struct ActiveQueryContext;
struct ParserOptions;
class SimpleBufferedData;
class BufferedData;
struct ClientData;
class ClientContextState;
class Connection;
class PhysicalTransaction;
class RegisteredStateManager;
struct TransactionInfo;

//! A statement parameter: identifier ($1 -> "1"), binding index, and inferred type (UNKNOWN if not inferred).
struct StatementParameter {
	Identifier identifier;
	idx_t index;
	LogicalType type;
};

//! A bound statement's signature: result schema (names/types) and parameter schema, without a
//! PreparedStatement. names/types are never empty; parameters are unordered (sort by index for positional use).
struct StatementSignature {
	vector<Identifier> names;
	vector<LogicalType> types;
	vector<StatementParameter> parameters;
	StatementProperties properties;
	//! The statement expands into several; its result is known only once execution binds the statement that
	//! produces it
	bool metadata_deferred = false;
};

//! Interrupt state for the client context
enum class ClientInterruptState : uint8_t { NOT_INTERRUPTED, INTERRUPTED, INTERRUPTS_SUPPRESSED };

//! The ClientContext holds information relevant to the current client session
//! during execution
class ClientContext : public enable_shared_from_this<ClientContext> {
	friend class QueryResult;         // LockContext
	friend class BufferedData;        // ExecuteTaskInternal, PollInternal
	friend class SimpleBufferedData;  // ExecuteTaskInternal, PollInternal
	friend class BatchedBufferedData; // ExecuteTaskInternal, PollInternal
	friend class ResultStreamBase;    // drives a statement that expands before its result buffer exists
	friend class ConnectionManager;
	friend class Connection;
	friend class PhysicalTransaction;

public:
	//! The error of a statement submitted while a result that no call ended holds the connection
	static constexpr const char *OPEN_RESULT_ERROR =
	    "connection has an open result; drain or destroy it before starting a new query";
	//! The error of a query that ran past max_execution_time
	static constexpr const char *QUERY_DEADLINE_ERROR = "Query exceeded maximum execution time";
	//! The error of a statement that expands into several, given parameters
	static constexpr const char *EXPANDING_PARAMETERS_ERROR =
	    "parameters are not supported for a statement that expands into several";
	//! The error of a prepared statement with parameters, while CONNECT-ed
	static constexpr const char *CONNECTED_PARAMETERS_ERROR =
	    "Parameterized prepared statements cannot be executed while CONNECT-ed; DISCONNECT first, or run the SQL as a "
	    "fresh statement to route through the CONNECT binding";

public:
	DUCKDB_API explicit ClientContext(shared_ptr<DatabaseInstance> db);
	DUCKDB_API ~ClientContext();

	//! The database that this client is connected to
	shared_ptr<DatabaseInstance> db;
	//! Interrupt state for the current query
	atomic<ClientInterruptState> interrupt_state {ClientInterruptState::NOT_INTERRUPTED};
	//! The deadline for the current query (milliseconds since epoch)
	optional_idx query_deadline;
	//! For an internal connection created on behalf of another client: the outermost client, whose interrupts and
	//! query deadline also apply. An internal connection runs within a query of its base client.
	shared_ptr<ClientContext> base_context;
	//! The number of internal connections between this client and its base client (0 for a base client)
	idx_t internal_depth = 0;
	//! Set of optional states (e.g. Caches) that can be held by the ClientContext
	unique_ptr<RegisteredStateManager> registered_state;
	//! The logger to be used by this ClientContext
	shared_ptr<Logger> logger;
	//! The client configuration
	ClientConfig config;
	//! The set of client-specific data
	unique_ptr<ClientData> client_data;
	//! Data for the currently running transaction
	TransactionContext transaction;

public:
	//! Connect this client to a remote-style AttachedDatabase. Subsequent non-control SQL routes via
	//! Catalog::GetConnectFunctionName. Use DisconnectFromCatalog() to revert to LOCAL.
	DUCKDB_API void ConnectToCatalog(const shared_ptr<AttachedDatabase> &target);
	//! Clear any active CONNECT; subsequent SQL goes through the normal DuckDB pipeline.
	DUCKDB_API void DisconnectFromCatalog();
	//! True iff a CONNECT is currently active (even if the target was detached out from under us).
	DUCKDB_API bool IsConnected() const {
		return is_connected;
	}
	//! Resolve the currently-connected AttachedDatabase. Returns nullptr if not connected or if the
	//! target has been detached out from under us (in that case IsConnected() is still true — call it
	//! directly to disambiguate "never connected" from "was connected, target was detached elsewhere").
	DUCKDB_API shared_ptr<AttachedDatabase> TryGetConnectedCatalog() const;

	bool HasActiveTransaction() const {
		return transaction.HasActiveTransaction();
	}
	MetaTransaction &ActiveTransaction() {
		return transaction.ActiveTransaction();
	}

	//! Interrupts the running query. Async-signal-safe: sets a flag and nothing else
	DUCKDB_API void Interrupt();
	DUCKDB_API bool IsInterrupted() const;
	DUCKDB_API void ClearInterrupt();
	//! Suppress all further interrupts for the current query (called after irreversible operations like COMMIT)
	DUCKDB_API void SuppressInterrupts();
	DUCKDB_API void CancelTransaction();

	//! Check for interrupt or timeout, throws InterruptException if triggered
	DUCKDB_API void InterruptCheck() const;
	//! InterruptCheck that reads the clock on every call, for waits that block between checks
	DUCKDB_API void BlockingInterruptCheck() const;

	//! Enable query profiling
	DUCKDB_API void EnableProfiling();
	//! Disable query profiling
	DUCKDB_API void DisableProfiling();

	//! Blocking. Runs the query to completion and returns its handle. The result is retained
	DUCKDB_API unique_ptr<QueryResult> Query(const string &query, QueryParameters query_parameters);
	DUCKDB_API unique_ptr<QueryResult> Query(unique_ptr<SQLStatement> statement,
	                                         const QueryParameters &query_parameters);
	//! As above, in the given result format
	DUCKDB_API unique_ptr<QueryResult> Query(const string &query, shared_ptr<ResultFormat> format);
	DUCKDB_API unique_ptr<QueryResult> Query(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format);

	//! Non-blocking. Submits the query and returns its handle. The engine runs it iff threads - external_threads > 0,
	//! but produces no data until the caller either calls a materializing method on the handle or opens a
	//! QueryResultStream on it. The query may only contain a single statement.
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, const QueryParameters &query_parameters);
	//! Non-blocking. As above, for a parsed statement
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement,
	                                          const QueryParameters &query_parameters);
	//! Non-blocking. As above, in the given result format
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, shared_ptr<ResultFormat> format);
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format);

	//! Non-blocking. As above, for bound parameter values
	DUCKDB_API unique_ptr<QueryResult> Submit(unique_ptr<SQLStatement> statement,
	                                          identifier_map_t<BoundParameterData> &values,
	                                          QueryParameters query_parameters);
	DUCKDB_API unique_ptr<QueryResult> Submit(const string &query, identifier_map_t<BoundParameterData> &values,
	                                          QueryParameters query_parameters);

	//! Blocking. Runs a statement that was generated internally rather than parsed from user SQL. Statement
	//! verification is skipped, and the client context lock is held for the entire duration of the query.
	DUCKDB_API unique_ptr<QueryResult> RunInternalStatement(unique_ptr<SQLStatement> statement,
	                                                        const QueryParameters &parameters);
	//! Non-blocking. Same as RunInternalStatement, but returns the handle of the submitted query, which the caller
	//! drives
	DUCKDB_API unique_ptr<QueryResult> SubmitInternalStatement(unique_ptr<SQLStatement> statement,
	                                                           const QueryParameters &parameters);

	//! Destroy the client context
	DUCKDB_API void Destroy();

	//! Get the table info of a specific table, or nullptr if it cannot be found.
	DUCKDB_API unique_ptr<TableDescription> TableInfo(const Identifier &database_name, const Identifier &schema_name,
	                                                  const Identifier &table_name);
	//! Get the table info of a specific table, or nullptr if it cannot be found. Uses INVALID_CATALOG.
	DUCKDB_API unique_ptr<TableDescription> TableInfo(const Identifier &schema_name, const Identifier &table_name);
	//! Executes a query with the given collection "attached" to the query using a CTE.
	DUCKDB_API void Append(unique_ptr<SQLStatement> stmt);
	//! Appends a ColumnDataCollection to the described table.
	DUCKDB_API void Append(TableDescription &description, ColumnDataCollection &collection);

	//! Try to bind a relation in the current client context; either throws an exception or fills the result_columns
	//! list with the set of returned columns
	DUCKDB_API void TryBindRelation(Relation &relation, vector<ColumnDefinition> &result_columns);

	//! Internal function for try bind relation. It does not require a client-context lock.
	DUCKDB_API void InternalTryBindRelation(Relation &relation, vector<ColumnDefinition> &result_columns);

	//! Non-blocking. Submits a relation and returns its handle
	DUCKDB_API unique_ptr<QueryResult> Submit(const shared_ptr<Relation> &relation,
	                                          const QueryParameters &query_parameters);
	//! Blocking. Runs a relation to completion and returns its handle
	DUCKDB_API unique_ptr<QueryResult> Execute(const shared_ptr<Relation> &relation);

	//! Prepare a query. A statement that expands into several is kept as written and expanded at each execution, so
	//! its metadata is known only from the result
	DUCKDB_API unique_ptr<PreparedStatement> Prepare(const string &query);
	//! Directly prepare a SQL statement, as Prepare(query) does
	DUCKDB_API unique_ptr<PreparedStatement> Prepare(unique_ptr<SQLStatement> statement);
	//! Deallocate the prepared statement with the given name - does nothing if it does not exist
	DUCKDB_API void RemovePreparedStatement(const string &name);
	//! Bind a statement and return its signature, without building a PreparedStatement, optimizing, or
	//! executing. Read-only: binding touches no in-flight query state, so a live result survives. Throws on error.
	//! A statement that expands into several reports metadata_deferred with a placeholder column
	DUCKDB_API StatementSignature BindStatement(unique_ptr<SQLStatement> statement);
	//! Throws the error a new statement gets while a result that no call ended holds the connection
	DUCKDB_API void VerifyNoOpenResult();

	//! Gets current percentage of the query's progress, returns 0 in case the progress bar is disabled.
	DUCKDB_API QueryProgress GetQueryProgress();
	//! Snapshot the query-level metrics of the running query, keyed by metric name (e.g. "io.total_bytes_read").
	//! Safe to call from another thread while the query runs, and regardless of whether profiling is enabled.
	DUCKDB_API profiler_metrics_t GetLiveQueryMetrics();

	//! Register function in the temporary schema
	DUCKDB_API void RegisterFunction(CreateFunctionInfo &info);

	//! Iterate the user statements of a script as parsed. The submission of each one expands it, so a statement that
	//! the engine rewrites into several is still one statement here. With apply_invalidation_policy a parse error
	//! invalidates the active transaction according to the transaction invalidation policy, as fits statements that are
	//! parsed to be run; without it the transaction stays usable, as fits statements that are only inspected.
	DUCKDB_API StatementIterator IterateStatements(const string &query, bool apply_invalidation_policy = true);

	//! Extract the logical plan of a query
	DUCKDB_API unique_ptr<LogicalOperator> ExtractPlan(const string &query);

	//! Runs a function with a valid transaction context, potentially starting a transaction if the context is in auto
	//! commit mode.
	DUCKDB_API void RunFunctionInTransaction(const std::function<void(void)> &fun,
	                                         bool requires_valid_transaction = true);
	//! Same as RunFunctionInTransaction, but does not obtain a lock on the client context or check for validation
	DUCKDB_API void RunFunctionInTransactionInternal(ClientContextLock &lock, const std::function<void(void)> &fun,
	                                                 bool requires_valid_transaction = true);

	//! Equivalent to CURRENT_SETTING(key) SQL function.
	DUCKDB_API SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) const;
	//! Typed variant, leaves result untouched if not found
	template <class TYPE>
	SettingLookupResult TryGetCurrentSetting(const Identifier &key, TYPE &result) const {
		Value output;
		auto lookup_result = TryGetCurrentSetting(key, output);
		if (lookup_result) {
			result = output.GetValue<TYPE>();
		}
		return lookup_result;
	}
	//! Returns the value of the current setting set by the user - if the user has set it.
	DUCKDB_API SettingLookupResult TryGetCurrentUserSetting(idx_t setting_index, Value &result) const;

	//! Whether or not the given result object is the connection's open result
	DUCKDB_API bool IsActiveResult(ClientContextLock &lock, BaseQueryResult &result);

	//! Returns the current executor
	Executor &GetExecutor();

	//! Return the current logger
	Logger &GetLogger() const;

	//! Returns the current query string (if any)
	const string &GetCurrentQuery();

	connection_t GetConnectionId() const;

	ConnectionType GetConnectionType() const {
		return connection_type;
	}

	//! Fetch the set of tables names of the query.
	//! Returns the fully qualified, escaped table names, if qualified is set to true,
	//! else returns the not qualified, not escaped table names.
	DUCKDB_API unordered_set<string> GetTableNames(const string &query, const bool qualified = false);

	DUCKDB_API ClientProperties GetClientProperties();

	//! Returns true if execution of the current query is finished
	DUCKDB_API bool ExecutionIsFinished();

	//! Process an error for display to the user
	DUCKDB_API void ProcessError(ErrorData &error, const string &query) const;
	//! Invalidates the active transaction (or for fatal errors the database) when the current invalidation policy says
	//! this error type should
	DUCKDB_API void ApplyInvalidationPolicy(const ErrorData &error);

	DUCKDB_API LogicalType ParseLogicalType(const string &type);

private:
	friend class Parser;
	friend class ParseIterator;
	ParserOptions GetParserOptions();

	//! Runs a transaction statement without going through the local query processing pipeline.
	void RunTransactionStatement(const TransactionInfo &info);
	//! Same as RunTransactionStatement, but does not obtain a lock or route CONNECT statements.
	void RunTransactionStatementInternal(const TransactionInfo &info);

	//! Whether a submission settles its result as retained before the first task is scheduled. A caller that
	//! completes the query itself settles it, so no producer parks for a decision it has already made
	enum class Retention : uint8_t { UNDECIDED, RETAINED_AT_SUBMISSION };

	//! Submits a query to the database and returns its handle
	unique_ptr<QueryResult> SubmitInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
	                                       const QueryParameters &parameters, bool verify = true,
	                                       Retention retention = Retention::UNDECIDED);
	//! Drives a submitted query to completion and retains its result
	unique_ptr<QueryResult> CompleteInternal(ClientContextLock &lock, unique_ptr<QueryResult> result);
	//! Drives a query whose collector builds its own result object, and hands that object out. Null
	//! when the query failed: the error is on the handle
	unique_ptr<QueryResult> CompleteDelegatedInternal(ClientContextLock &lock, QueryResult &result);

	//! The user statements of a query, as parsed: a submission expands each one. profile is false for calls that
	//! do not run the query
	vector<unique_ptr<SQLStatement>> ParseStatementsInternal(ClientContextLock &lock, const string &query,
	                                                         bool profile = true);
	//! Expands a user statement for a caller that binds it without running it; a statement that expands into
	//! several is refused with the given message
	unique_ptr<SQLStatement> ExpandSingleStatementInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
	                                                       const char *refusal);
	//! A verification that runs a statement of its own, like a prepared or an explained copy, is skipped when
	//! allow_nested_statements is false
	void StatementVerification(ClientContextLock &lock, unique_ptr<SQLStatement> &statement,
	                           QueryParameters query_parameters, bool allow_nested_statements = true);

	//! Whether a result that no call ended holds the connection, so that a new statement is refused
	bool OpenResultHoldsConnection(ClientContextLock &lock);
	//! The refusal of a statement submitted while a result holds the connection; otherwise null, after abandoning
	//! what is left of the previous query
	template <class T>
	[[nodiscard]] unique_ptr<T> InitialCleanup(ClientContextLock &lock);
	//! Abandons the query of an open result that no call ended, as Close would, and resets the interrupted flag
	void AbandonActiveQuery(ClientContextLock &lock);
	//! Internal clean up, does not lock. Caller must hold the context_lock.
	void CleanupInternal(ClientContextLock &lock, BaseQueryResult *result = nullptr,
	                     bool invalidate_transaction = false);
	//! Ends the active query as abandoned: nothing it did is committed
	void AbortInternal(ClientContextLock &lock);
	unique_ptr<QueryResult> SubmitStatement(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
	                                        const QueryParameters &parameters, bool verify, Retention retention);
	//! Starts a bound statement of the active query. Returns its handle; for a statement of a query that expands
	//! into several, null, unless a result collector built its own result object
	unique_ptr<QueryResult> SubmitPreparedStatementInternal(ClientContextLock &lock,
	                                                        shared_ptr<PreparedStatementData> statement_data_p,
	                                                        const QueryParameters &parameters);
	void CheckIfPreparedStatementIsExecutable(PreparedStatementData &statement);

	//! Internally prepare a SQL statement. Caller must hold the context_lock.
	shared_ptr<PreparedStatementData> CreatePreparedStatement(ClientContextLock &lock,
	                                                          unique_ptr<SQLStatement> statement,
	                                                          const QueryParameters &parameters);
	unique_ptr<QueryResult> SubmitStatementInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
	                                                const QueryParameters &parameters);
	//! Expands a user statement into the statements it runs as, without the statements of its transaction handling
	PreprocessedStatement PreprocessStatementInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement);
	//! Runs the statements of a query that expands up to the one producing its result, inside the submission, so
	//! that a result collector building its own result object serves that statement. Returns that object, if any
	unique_ptr<QueryResult> RunToResultStatementInternal(ClientContextLock &lock, QueryResult &handle);
	unique_ptr<QueryResult> RunStatementInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
	                                             const QueryParameters &parameters, bool verify = true);
	unique_ptr<PreparedStatement> PrepareInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement);
	void LogQueryInternal(ClientContextLock &lock, const string &query);
	//! Run the statements of a query string one after the other
	unique_ptr<QueryResult> QueryInternal(ClientContextLock &lock, const string &query,
	                                      QueryParameters query_parameters);

	unique_ptr<ClientContextLock> LockContext();

	//! Begins the query of a user statement, whose first statement runs next
	void BeginQueryInternal(ClientContextLock &lock, PreprocessedStatement statements,
	                        const QueryParameters &parameters, Retention retention);
	//! Begins one statement of the active query: its query number, its text and its query log line
	void BeginStatementInternal(ClientContextLock &lock, const string &query);
	//! Refreshes the logger for the statement that begins
	void RefreshLoggerInternal();
	//! Ends the statement of a query that expands into several once it finished, and starts the next one
	QueryResultState StartNextStatementInternal(ClientContextLock &lock);
	//! Ends the statement that finished and starts the next one. Returns the result object a collector built for it,
	//! which only a statement started inside the submission may have
	unique_ptr<QueryResult> AdvanceStatementInternal(ClientContextLock &lock);
	//! Whether this query or, for an internal connection, the base client's query has a deadline
	bool HasQueryDeadline() const;
	bool QueryDeadlinePassed() const;
	//! Ends the statement of the active query that finished, when another one follows
	void EndStatementInternal(ClientContextLock &lock);
	ErrorData EndQueryInternal(ClientContextLock &lock, bool success, bool invalidate_transaction,
	                           optional_ptr<ErrorData> previous_error);
	//! End the query in the profiler; a failure to emit the profile becomes the error unless there is one already
	void EndQueryProfiling(ErrorData &error);
	//! The result of the finished query. For a statement that expands into several and returns no rows, that is an
	//! empty result in the query's format, the way a statement that returns nothing reports
	unique_ptr<QueryResult> TakeFinishedResultInternal(ClientContextLock &lock);
	//! A finished result without rows in `format`, the chunk format when null
	unique_ptr<QueryResult> EmptyResultInternal(StatementType statement_type, StatementProperties properties,
	                                            vector<LogicalType> types, vector<Identifier> names,
	                                            shared_ptr<ResultFormat> format);
	//! Records the consumer's retention decision for a result statement that has not started. The first decision
	//! stands; returns it
	ResultLifetime DecideResultLifetimeInternal(ClientContextLock &lock, ResultLifetime decision);

	//! Wait until a task is available to execute
	void WaitForTask(ClientContextLock &lock, BaseQueryResult &result);
	//! Run one partial task slice of the open result on the calling thread
	QueryResultState ExecuteTaskInternal(ClientContextLock &lock, BaseQueryResult &result);
	//! Report the execution state of the open result without running any task
	QueryResultState PollInternal(ClientContextLock &lock, BaseQueryResult &result);
	//! Record the error on the result, end the query and report EXECUTION_ERROR
	QueryResultState FailQueryInternal(ClientContextLock &lock, BaseQueryResult &result, ErrorData error);
	void UpdateProgressInternal(QueryResultState state);

	unique_ptr<QueryResult> SubmitInternal(ClientContextLock &, const shared_ptr<Relation> &relation,
	                                       const QueryParameters &query_parameters);

	template <class T>
	unique_ptr<T> ErrorResult(ErrorData error, const string &query = string());

	shared_ptr<PreparedStatementData> CreatePreparedStatementInternal(ClientContextLock &lock,
	                                                                  unique_ptr<SQLStatement> statement,
	                                                                  const QueryParameters &parameters);

	bool ErrorInvalidatesTransaction(ExceptionType type);
	//! Whether an error ends the active query by invalidating the transaction it runs in
	bool ErrorInvalidatesQueryTransaction(ExceptionType type);

private:
	//! Lock on using the ClientContext in parallel
	mutex context_lock;
	//! The currently active query context
	unique_ptr<ActiveQueryContext> active_query;
	//! The current query progress
	QueryProgress query_progress;
	//! The connection corresponding to this client context
	connection_t connection_id;
	//! Type of connection (USER or INTERNAL)
	ConnectionType connection_type = ConnectionType::USER;
	//! Routing target for SQL execution while CONNECT-ed (CONNECT/DISCONNECT). When is_connected is
	//! true and connected_to_database can be locked, the chokepoint dispatches non-control SQL via
	//! `Catalog::RemoteExecute(string)` and wraps the returned TableRef into a SelectStatement.
	weak_ptr<AttachedDatabase> connected_to_database;
	bool is_connected = false;
};

class ClientContextLock {
public:
	explicit ClientContextLock(mutex &context_lock) : client_guard(context_lock) {
	}

	~ClientContextLock() {
	}

private:
	lock_guard<mutex> client_guard;
};

} // namespace duckdb
