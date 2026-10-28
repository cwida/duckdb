#include "duckdb/main/client_context.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/active_query_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/statement_iterator.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/relation.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/passthrough_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/relation_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/statement_preprocessor.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/result_set_manager.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/main/prepared_statement.hpp"

#ifdef __APPLE__
#include <sys/sysctl.h>

// code adapted from Apple's example
// https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment#Determine-Whether-Your-App-Is-Running-as-a-Translated-Binary
static bool OsxRosettaIsActive() {
	int ret = 0;
	size_t size = sizeof(ret);
	if (sysctlbyname("sysctl.proc_translated", &ret, &size, NULL, 0)) {
		return false;
	}
	return ret == 1;
}

#endif

namespace duckdb {

#ifdef DEBUG
struct DebugClientContextState : public ClientContextState {
	~DebugClientContextState() override {
		if (Exception::UncaughtException()) {
			return;
		}
		D_ASSERT(!active_transaction);
		D_ASSERT(!active_query);
	}

	bool active_transaction = false;
	bool active_query = false;

	void QueryBegin(ClientContext &context) override {
		if (active_query) {
			throw InternalException("DebugClientContextState::QueryBegin called when a query is already active");
		}
		active_query = true;
	}
	void QueryEnd(ClientContext &context) override {
		if (!active_query) {
			throw InternalException("DebugClientContextState::QueryEnd called when no query is active");
		}
		active_query = false;
	}
	void TransactionBegin(MetaTransaction &transaction, ClientContext &context) override {
		if (active_transaction) {
			throw InternalException(
			    "DebugClientContextState::TransactionBegin called when a transaction is already active");
		}
		active_transaction = true;
	}
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		if (!active_transaction) {
			throw InternalException("DebugClientContextState::TransactionCommit called when no transaction is active");
		}
		active_transaction = false;
	}
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		if (!active_transaction) {
			throw InternalException(
			    "DebugClientContextState::TransactionRollback called when no transaction is active");
		}
		active_transaction = false;
	}
#ifdef DUCKDB_DEBUG_REBIND
	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) override {
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
	RebindQueryInfo OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared,
	                                  PreparedStatementMode mode) override {
		if (mode == PreparedStatementMode::PREPARE_AND_EXECUTE) {
			return RebindQueryInfo::ATTEMPT_TO_REBIND;
		}
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                          RebindQueryInfo current_rebind) override {
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
#endif
};
#endif

ClientContext::ClientContext(shared_ptr<DatabaseInstance> database)
    : db(std::move(database)), interrupt_state(ClientInterruptState::NOT_INTERRUPTED), transaction(*this),
      connection_id(DConstants::INVALID_INDEX) {
	registered_state = make_uniq<RegisteredStateManager>();
#ifdef DEBUG
	registered_state->GetOrCreate<DebugClientContextState>("debug_client_context_state");
#endif
	LoggingContext context(LogContextScope::CONNECTION);
	logger = db->GetLogManager().CreateLogger(context, true);
	client_data = make_uniq<ClientData>(*this);

#ifdef __APPLE__
	if (OsxRosettaIsActive()) {
		DUCKDB_LOG_WARNING(*this, "OSX binary translation ('Rosetta') detected. Running DuckDB through Rosetta will "
		                          "cause a significant performance degradation. DuckDB is available natively on Apple "
		                          "silicon, please download an appropriate binary here: https://duckdb.org/install/");
	}
#endif
}

ClientContext::~ClientContext() {
	if (Exception::UncaughtException()) {
		return;
	}
	// destroy the client context and rollback if there is an active transaction
	// but only if we are not destroying this client context as part of an exception stack unwind
	Destroy();
}

unique_ptr<ClientContextLock> ClientContext::LockContext() {
	return make_uniq<ClientContextLock>(context_lock);
}

void ClientContext::ConnectToCatalog(const shared_ptr<AttachedDatabase> &target) {
	D_ASSERT(target);
	// Pre-flight: Supports(RemoteCapability::CONNECT) is the capability declaration; catalogs that
	// return true MUST implement RemoteExecute(string). Validation runs before mutation so a throw
	// leaves the client unbound.
	if (!target->GetCatalog().Supports(RemoteCapability::CONNECT)) {
		throw InvalidInputException("Database %s does not support CONNECT", target->GetName());
	}
	connected_to_database = target;
	is_connected = true;
	ClientConfig::GetConfig(*this).connected_grammar = target->GetConnectedGrammar(*this);
}

void ClientContext::DisconnectFromCatalog() {
	connected_to_database.reset();
	is_connected = false;
	ClientConfig::GetConfig(*this).connected_grammar.reset();
}

shared_ptr<AttachedDatabase> ClientContext::TryGetConnectedCatalog() const {
	if (!is_connected) {
		return nullptr;
	}
	return AttachedDatabase::TryGetReference(connected_to_database);
}

//! True if `type` is a CONNECT control statement that must execute against LOCAL even while a
//! CONNECT binding is active (i.e. the chokepoint must let it fall through, not rewrite it).
static bool IsConnectControlStatement(StatementType type) {
	return type == StatementType::CONNECT_STATEMENT || type == StatementType::DISCONNECT_STATEMENT;
}

//! Wrap a TableRef returned from Catalog::RemoteExecute into `SELECT * FROM <ref>` for the chokepoint.
static unique_ptr<SQLStatement> WrapAsSelect(unique_ptr<TableRef> from_ref) {
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(from_ref);

	auto select_stmt = make_uniq<SelectStatement>();
	select_stmt->node = std::move(select_node);
	return std::move(select_stmt);
}

void ClientContext::Destroy() {
	auto lock = LockContext();
	if (transaction.HasActiveTransaction()) {
		transaction.ResetActiveQuery();
		if (!transaction.IsAutoCommit()) {
			// a destructor cannot throw: once the database is invalidated, the rollback can fail as well
			try {
				transaction.Rollback(nullptr);
			} catch (...) { // NOLINT
			}
		}
	}
	CleanupInternal(*lock);
}

void ClientContext::ProcessError(ErrorData &error, const string &query) const {
	error.FinalizeError();
	if (Settings::Get<ErrorsAsJSONSetting>(*this)) {
		error.ConvertErrorToJSON();
	} else {
		error.AddErrorLocation(query);
	}
}

void ClientContext::ApplyInvalidationPolicy(const ErrorData &error) {
	bool invalidates_transaction = true;
	if (!ErrorInvalidatesTransaction(error.Type())) {
		// standard exceptions don't invalidate the transaction
		invalidates_transaction = false;
	} else if (Exception::InvalidatesDatabase(error.Type())) {
		auto &db_instance = DatabaseInstance::GetDatabase(*this);
		ValidChecker::Invalidate(db_instance, error.RawMessage());
	}
	// a statement issued while CONNECT-ed that did not run locally is not part of the local transaction
	const bool outside_local_transaction = active_query ? active_query->outside_local_transaction : is_connected;
	if (invalidates_transaction && HasActiveTransaction() && !outside_local_transaction) {
		ValidChecker::Invalidate(ActiveTransaction(), error.RawMessage());
	}
}

template <class T>
unique_ptr<T> ClientContext::ErrorResult(ErrorData error, const string &query) {
	ApplyInvalidationPolicy(error);
	ProcessError(error, query);
	return make_uniq<T>(std::move(error));
}

bool ClientContext::OpenResultHoldsConnection(ClientContextLock &) {
	// A query no result holds (one whose begin failed), or whose result a collector built so that nothing can end
	// it, is abandoned instead
	return active_query && active_query->HasOpenResult() && !active_query->collector_built_result;
}

template <class T>
unique_ptr<T> ClientContext::InitialCleanup(ClientContextLock &lock) {
	if (OpenResultHoldsConnection(lock)) {
		// The statement never ran, so no invalidation policy applies to the open result's transaction
		auto error = ErrorData(ResourceInUseException(OPEN_RESULT_ERROR));
		ProcessError(error, string());
		return make_uniq<T>(std::move(error));
	}
	AbandonActiveQuery(lock);
	return nullptr;
}

void ClientContext::VerifyNoOpenResult() {
	auto lock = LockContext();
	if (OpenResultHoldsConnection(*lock)) {
		throw ResourceInUseException(OPEN_RESULT_ERROR);
	}
}

Logger &ClientContext::GetLogger() const {
	return *logger;
}

connection_t ClientContext::GetConnectionId() const {
	return connection_id;
}

static bool IsExplainAnalyze(ClientContext &context, SQLStatement *statement) {
	if (!statement) {
		return false;
	}
	if (statement->type == StatementType::EXECUTE_STATEMENT) {
		// the profiler state set up when preparing is gone by the time the statement is executed
		auto &prepared_statements = ClientData::Get(context).prepared_statements;
		auto entry = prepared_statements.find(statement->Cast<ExecuteStatement>().name);
		if (entry == prepared_statements.end()) {
			return false;
		}
		return IsExplainAnalyze(context, entry->second->unbound_statement.get());
	}
	if (statement->type != StatementType::EXPLAIN_STATEMENT) {
		return false;
	}
	auto &explain = statement->Cast<ExplainStatement>();
	return explain.explain_type == ExplainType::EXPLAIN_ANALYZE;
}

shared_ptr<PreparedStatementData> ClientContext::CreatePreparedStatementInternal(ClientContextLock &lock,
                                                                                 unique_ptr<SQLStatement> statement,
                                                                                 const QueryParameters &parameters) {
	StatementType statement_type = statement->type;
	auto result = make_shared_ptr<PreparedStatementData>(statement_type);

	auto &profiler = QueryProfiler::Get(*this);
	profiler.StartQuery(statement->query, IsExplainAnalyze(*this, statement.get()));
	Planner logical_planner(*this);
	if (parameters.statement_args) {
		auto &parameter_values = *parameters.statement_args;
		for (auto &value : parameter_values) {
			logical_planner.parameter_data.emplace(value.first, BoundParameterData(value.second));
		}
	}

	{
		auto planner_timer = profiler.StartTimer<MetricPlannerTotalTime>();
		logical_planner.CreatePlan(std::move(statement));
		D_ASSERT(logical_planner.plan || !logical_planner.properties.bound_all_parameters);
	}

	if (logical_planner.properties.bound_all_parameters) {
		logical_planner.Optimize();
	}
	auto logical_plan = std::move(logical_planner.plan);
	// extract the result column names from the plan
	result->properties = logical_planner.properties;
	result->names = logical_planner.names;
	result->types = logical_planner.types;
	result->value_map = std::move(logical_planner.value_map);
	if (!logical_planner.properties.bound_all_parameters) {
		// not all parameters were bound - return
		return result;
	}

	// Convert the logical query plan into a physical query plan.
	{
		auto physical_timer = profiler.StartTimer<MetricPhysicalPlannerTotalTime>();
		PhysicalPlanGenerator physical_planner(*this);
		result->physical_plan = physical_planner.Plan(std::move(logical_plan));
	}
	D_ASSERT(result->physical_plan);
	return result;
}

shared_ptr<PreparedStatementData> ClientContext::CreatePreparedStatement(ClientContextLock &lock,
                                                                         unique_ptr<SQLStatement> statement,
                                                                         const QueryParameters &parameters) {
	// check if any client context state could request a rebind
	bool can_request_rebind = false;
	for (auto &state : registered_state->States()) {
		if (state->CanRequestRebind()) {
			can_request_rebind = true;
		}
	}
	if (can_request_rebind) {
		bool rebind = false;
		// if any registered state can request a rebind we do the binding on a copy first
		shared_ptr<PreparedStatementData> result;
		try {
			result = CreatePreparedStatementInternal(lock, statement->Copy(), parameters);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			// check if any registered client context state wants to try a rebind
			for (auto &state : registered_state->States()) {
				auto info = state->OnPlanningError(*this, *statement, error);
				if (info == RebindQueryInfo::ATTEMPT_TO_REBIND) {
					rebind = true;
				}
			}
			if (!rebind) {
				throw;
			}
		}
		if (result) {
			D_ASSERT(!rebind);
			for (auto &state : registered_state->States()) {
				auto info = state->OnFinalizePrepare(*this, *result, PreparedStatementMode::PREPARE_AND_EXECUTE);
				if (info == RebindQueryInfo::ATTEMPT_TO_REBIND) {
					rebind = true;
				}
			}
		}
		if (!rebind) {
			return result;
		}
		// an extension wants to do a rebind - do it once
	}

	return CreatePreparedStatementInternal(lock, std::move(statement), parameters);
}

QueryProgress ClientContext::GetQueryProgress() {
	return query_progress;
}

profiler_metrics_t ClientContext::GetLiveQueryMetrics() {
	return QueryProfiler::Get(*this).GetLiveMetrics();
}

void ClientContext::CheckIfPreparedStatementIsExecutable(PreparedStatementData &statement) {
	if (ValidChecker::IsInvalidated(ActiveTransaction()) && statement.properties.requires_valid_transaction) {
		throw ErrorManager::InvalidatedTransaction(*this);
	}

	auto &meta_transaction = MetaTransaction::Get(*this);
	auto &manager = DatabaseManager::Get(*this);
	for (auto &it : statement.properties.modified_databases) {
		auto &modified_database = it.first;
		auto entry = manager.GetDatabase(*this, modified_database);
		if (!entry) {
			// database has been detached
			throw InvalidInputException("Database %s not found", modified_database);
		}
		if (entry->IsReadOnly()) {
			throw InvalidInputException(StringUtil::Format(
			    "Cannot execute statement of type \"%s\" on database %s which is attached in read-only mode!",
			    StatementTypeToString(statement.statement_type), modified_database));
		}
		meta_transaction.ModifyDatabase(*entry, it.second.modifications);
	}
}

bool ClientContext::ErrorInvalidatesQueryTransaction(ExceptionType type) {
	// A statement that expands into several and fails after one of its statements wrote has applied part of its
	// effect, which only ending the transaction undoes
	if (active_query && active_query->ExpandsIntoSeveral() && active_query->Wrote()) {
		return true;
	}
	return ErrorInvalidatesTransaction(type);
}

bool ClientContext::ErrorInvalidatesTransaction(ExceptionType type) {
	switch (transaction.GetInvalidationPolicy()) {
	case TransactionInvalidationPolicy::STANDARD_POLICY:
	case TransactionInvalidationPolicy::ALL_ERRORS_INVALIDATE_TRANSACTION:
		return true;
	default:
		return Exception::InvalidatesTransaction(type);
	}
}

StatementIterator ClientContext::IterateStatements(const string &query, bool apply_invalidation_policy) {
	return StatementIterator(ParseIterator(*this, query, apply_invalidation_policy));
}

vector<unique_ptr<SQLStatement>> ClientContext::ParseStatementsInternal(ClientContextLock &lock, const string &query,
                                                                        bool profile) {
	try {
		if (profile) {
			QueryProfiler::Get(*this).StartQuery(query);
		}

		// Drain the lazy iterator into a vector for callers that want the eager shape. The callers that run the
		// statements apply the invalidation policy through ErrorResult.
		StatementIterator iterator {ParseIterator(*this, query, false, lock)};
		vector<unique_ptr<SQLStatement>> result;
		while (iterator.Peek()) {
			result.push_back(profile ? iterator.GetStatementForExecution() : iterator.GetStatement());
		}
		return result;
	} catch (std::exception &ex) {
		auto error = ErrorData(ex);
		ProcessError(error, query);
		error.Throw();
	}
}

static void CheckStatement(ClientContext &context, const SQLStatement &statement, StatementCheckType type) {
	try {
		for (auto &extension : ExtensionCallback::Iterate(context)) {
			extension->OnStatementCheck(context, statement, type);
		}
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		if (Exception::InvalidatesDatabase(error.Type())) {
			ValidChecker::Invalidate(DatabaseInstance::GetDatabase(context), error.RawMessage());
		}
		throw;
	}
}

unique_ptr<LogicalOperator> ClientContext::ExtractPlan(const string &query) {
	auto lock = LockContext();

	auto statements = ParseStatementsInternal(*lock, query, false);
	if (statements.size() != 1) {
		throw InvalidInputException("ExtractPlan can only prepare a single statement");
	}
	CheckStatement(*this, *statements[0], StatementCheckType::INSPECTION);
	statements[0] = ExpandSingleStatementInternal(*lock, std::move(statements[0]),
	                                              "ExtractPlan can only prepare a single statement");
	CheckStatement(*this, *statements[0], StatementCheckType::INSPECTION);

	unique_ptr<LogicalOperator> plan;
	RunFunctionInTransactionInternal(*lock, [&]() {
		Planner planner(*this);
		planner.CreatePlan(std::move(statements[0]));
		D_ASSERT(planner.plan);

		plan = std::move(planner.plan);

		Optimizer optimizer(*planner.binder, *this);
		plan = optimizer.Optimize(std::move(plan));

		LogicalPlanVerifier::Verify(*this, *plan);
		// the resolver checks the column references against the operator types, which the optimizer leaves stale
		plan->ResolveOperatorTypes();
		ColumnBindingResolver resolver;
		resolver.VisitOperator(*plan);
	});
	return plan;
}

//! Snapshot the metadata that the PreparedStatement handle exposes to the user
static PreparedStatementInfo GetPreparedStatementInfo(PreparedStatementData &data) {
	PreparedStatementInfo info;
	info.names = data.names;
	info.types = data.types;
	info.statement_type = data.statement_type;
	info.properties = data.properties;
	if (data.unbound_statement) {
		info.named_param_map = data.unbound_statement->named_param_map;
	}
	for (auto &entry : data.value_map) {
		LogicalType parameter_type;
		if (data.TryGetType(entry.first, parameter_type)) {
			info.parameter_types.emplace(entry.first, std::move(parameter_type));
		}
	}
	return info;
}

unique_ptr<PreparedStatement> ClientContext::PrepareInternal(ClientContextLock &lock,
                                                             unique_ptr<SQLStatement> statement) {
	CheckStatement(*this, *statement, StatementCheckType::PREPARATION);
	if (is_connected) {
		// the connected database runs the statement as written, at each execution
		if (!statement->named_param_map.empty()) {
			throw InvalidInputException(CONNECTED_PARAMETERS_ERROR);
		}
		if (statement->type != StatementType::DISCONNECT_STATEMENT) {
			// it runs on the connected database only, also when parsed before CONNECT
			auto passthrough = make_uniq<PassthroughStatement>();
			passthrough->query = std::move(statement->query);
			passthrough->stmt_location = statement->stmt_location;
			statement = std::move(passthrough);
		}
		return make_uniq<PreparedStatement>(shared_from_this(), std::move(statement));
	}
	// Only a pragma or a multi-statement can expand, and only one that does not expand into exactly one statement
	// keeps the statement as written, to expand it again at each execution
	unique_ptr<SQLStatement> written_statement;
	if (statement->type == StatementType::PRAGMA_STATEMENT || statement->type == StatementType::MULTI_STATEMENT) {
		written_statement = statement->Copy();
	}
	auto expanded = PreprocessStatementInternal(lock, std::move(statement));
	if (expanded.statements.size() != 1) {
		D_ASSERT(written_statement);
		if (!written_statement->named_param_map.empty()) {
			throw InvalidInputException(EXPANDING_PARAMETERS_ERROR);
		}
		return make_uniq<PreparedStatement>(shared_from_this(), std::move(written_statement));
	}
	statement = std::move(expanded.statements[0]);

	auto statement_query = statement->query;
	// prepare the statement under a generated name - the returned PreparedStatement only refers to that name
	auto name = "duckdb_prepare_internal_" + UUID::ToString(UUID::GenerateRandomUUID());
	auto prepare = make_uniq<PrepareStatement>();
	prepare->name = Identifier(name);
	prepare->query = statement_query;
	prepare->stmt_location = statement->stmt_location;
	prepare->statement = std::move(statement);

	QueryParameters parameters;
	auto result = RunStatementInternal(lock, std::move(prepare), parameters, false);
	result->ThrowIfError();
	auto entry = client_data->prepared_statements.find(Identifier(name));
	if (entry == client_data->prepared_statements.end()) {
		throw InternalException("PREPARE succeeded but the prepared statement was not registered");
	}
	return make_uniq<PreparedStatement>(shared_from_this(), std::move(name), std::move(statement_query),
	                                    GetPreparedStatementInfo(*entry->second));
}

void ClientContext::RemovePreparedStatement(const string &name) {
	auto lock = LockContext();
	client_data->prepared_statements.erase(Identifier(name));
}

unique_ptr<PreparedStatement> ClientContext::Prepare(unique_ptr<SQLStatement> statement) {
	auto lock = LockContext();
	// Store the query in case of an error.
	auto query = statement->query;

	// Try to prepare.
	try {
		if (auto refusal = InitialCleanup<PreparedStatement>(*lock)) {
			return refusal;
		}
		return PrepareInternal(*lock, std::move(statement));
	} catch (std::exception &ex) {
		return ErrorResult<PreparedStatement>(ErrorData(ex), query);
	}
}

StatementSignature ClientContext::BindStatement(unique_ptr<SQLStatement> statement) {
	auto lock = LockContext();
	StatementSignature signature;
	ErrorData bind_error;
	RunFunctionInTransactionInternal(
	    *lock,
	    [&]() {
		    try {
			    // Expanded inside this transaction, so an expansion error leaves the caller's transaction as a bind
			    // error does
			    CheckStatement(*this, *statement, StatementCheckType::INSPECTION);
			    const bool has_parameters = !statement->named_param_map.empty();
			    auto expanded = StatementPreprocessor(*this).ExpandStatement(std::move(statement));
			    if (expanded.statements.empty()) {
				    signature.properties.return_type = StatementReturnType::NOTHING;
				    return;
			    }
			    if (expanded.statements.size() > 1) {
				    if (has_parameters) {
					    throw InvalidInputException(EXPANDING_PARAMETERS_ERROR);
				    }
				    // The placeholder the planner reports for a result known only at execution
				    signature.names = {"unknown"};
				    signature.types = {LogicalTypeId::UNKNOWN};
				    signature.metadata_deferred = true;
				    return;
			    }
			    auto bound = std::move(expanded.statements[0]);
			    CheckStatement(*this, *bound, StatementCheckType::INSPECTION);
			    auto named_param_map = bound->named_param_map;
			    Planner planner(*this);
			    planner.CreatePlan(std::move(bound));
			    signature.names = planner.names;
			    signature.types = planner.types;
			    signature.properties = std::move(planner.properties);
			    // Parameter types from the bound parameter map (as in PreparedStatementData::TryGetType).
			    // An un-anchored parameter (e.g. SELECT $1) gets no value_map entry, so its
			    // type stays UNKNOWN; do not assume every parameter is present.
			    for (auto &entry : named_param_map) {
				    LogicalType type(LogicalTypeId::UNKNOWN);
				    auto it = planner.value_map.find(entry.first);
				    if (it != planner.value_map.end()) {
					    type = it->second->return_type.id() != LogicalTypeId::INVALID ? it->second->return_type
					                                                                  : it->second->GetValue().type();
				    }
				    signature.parameters.push_back({entry.first, entry.second, std::move(type)});
			    }
		    } catch (const std::exception &ex) {
			    ErrorData error(ex);
			    // Binding is read-only: a recoverable bind error changed nothing, so leave
			    // the caller's transaction intact (rethrow after the wrapper). A database-
			    // fatal error still propagates so invalidation runs as usual.
			    if (Exception::InvalidatesDatabase(error.Type())) {
				    throw;
			    }
			    bind_error = std::move(error);
		    }
	    },
	    false);
	if (bind_error.HasError()) {
		bind_error.Throw();
	}
	return signature;
}

unique_ptr<PreparedStatement> ClientContext::Prepare(const string &query) {
	auto lock = LockContext();
	// prepare the query
	try {
		if (auto refusal = InitialCleanup<PreparedStatement>(*lock)) {
			return refusal;
		}

		// first parse the query
		auto statements = ParseStatementsInternal(*lock, query);
		if (statements.empty()) {
			throw InvalidInputException("No statement to prepare!");
		}
		if (statements.size() > 1) {
			throw InvalidInputException("Cannot prepare multiple statements at once!");
		}
		return PrepareInternal(*lock, std::move(statements[0]));
	} catch (std::exception &ex) {
		ErrorData error(ex);
		// a parse error has started the profiler without running a statement
		EndQueryProfiling(error);
		return ErrorResult<PreparedStatement>(std::move(error), query);
	}
}

unique_ptr<QueryResult> ClientContext::SubmitStatementInternal(ClientContextLock &lock,
                                                               unique_ptr<SQLStatement> statement,
                                                               const QueryParameters &parameters) {
	CheckStatement(*this, *statement, StatementCheckType::SUBMISSION);
	// prepare the query for execution
	if (!statement->named_param_map.empty() && parameters.statement_args) {
		PreparedStatement::VerifyParameters(*parameters.statement_args, statement->named_param_map, this);
	} else if (!statement->named_param_map.empty()) {
		identifier_map_t<BoundParameterData> empty_parameters;
		PreparedStatement::VerifyParameters(empty_parameters, statement->named_param_map, this);
	}

	auto prepared = CreatePreparedStatement(lock, std::move(statement), parameters);

	if (!prepared->properties.bound_all_parameters) {
		return ErrorResult<QueryResult>(InvalidInputException("Not all parameters were bound"));
	}
	// execute the prepared statement
	CheckIfPreparedStatementIsExecutable(*prepared);
	return SubmitPreparedStatementInternal(lock, std::move(prepared), parameters);
}

unique_ptr<SQLStatement> ClientContext::ExpandSingleStatementInternal(ClientContextLock &lock,
                                                                      unique_ptr<SQLStatement> statement,
                                                                      const char *refusal) {
	auto expanded = PreprocessStatementInternal(lock, std::move(statement));
	if (expanded.statements.size() != 1) {
		throw InvalidInputException(refusal);
	}
	return std::move(expanded.statements[0]);
}

PreprocessedStatement ClientContext::PreprocessStatementInternal(ClientContextLock &lock,
                                                                 unique_ptr<SQLStatement> statement) {
	StatementPreprocessor preprocessor(*this);
	return preprocessor.PreprocessStatement(lock, std::move(statement));
}

unique_ptr<QueryResult> ClientContext::RunStatementInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
                                                            const QueryParameters &parameters, bool verify) {
	auto result = SubmitInternal(lock, std::move(statement), parameters, verify, Retention::RETAINED_AT_SUBMISSION);
	if (result->HasError()) {
		return result;
	}
	return CompleteInternal(lock, std::move(result));
}

//! Whether this statement executes a prepared statement with parameter values supplied through the C/C++ API
static bool HasBoundParameterValues(const SQLStatement &statement) {
	if (statement.type != StatementType::EXECUTE_STATEMENT) {
		return false;
	}
	return !statement.Cast<ExecuteStatement>().bound_values.empty();
}

unique_ptr<QueryResult> ClientContext::SubmitStatement(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
                                                       const QueryParameters &parameters, bool verify,
                                                       Retention retention) {
	try {
		CheckStatement(*this, *statement, StatementCheckType::SUBMISSION);
	} catch (const std::exception &ex) {
		return ErrorResult<QueryResult>(ErrorData(ex), statement->query);
	}
	// CONNECT chokepoint: when connected, non-control SQL is rewritten in place and falls through to
	// the normal pipeline. No recursion — the rewrite goes through SubmitStatementInternal, not back here.
	bool outside_local_transaction = false;
	if (is_connected) {
		if (!IsConnectControlStatement(statement->type)) {
			// Parameterized prepared statements would need parameter substitution we don't do in v0 — reject.
			// No-param prepared statements have a fully-resolved `query` already, route them.
			if (HasBoundParameterValues(*statement) ||
			    (parameters.statement_args && !parameters.statement_args->empty())) {
				return ErrorResult<QueryResult>(ErrorData(InvalidInputException(CONNECTED_PARAMETERS_ERROR)),
				                                statement->query);
			}
			auto live = TryGetConnectedCatalog();
			if (!live) {
				// Target was detached elsewhere; user must explicitly DISCONNECT to clear is_connected.
				return ErrorResult<QueryResult>(
				    ErrorData(InvalidInputException(
				        "The connected database has been detached out from under this connection. Issue "
				        "DISCONNECT to clear the connection before running further SQL.")),
				    statement->query);
			}
			// Dispatch via the catalog — Supports(CONNECT) was validated at CONNECT time, so RemoteExecute
			// is contracted to be implemented. Wrap the returned TableRef into a SelectStatement.
			unique_ptr<TableRef> remote_ref;
			try {
				remote_ref = live->GetCatalog().RemoteExecute(*this, statement->query);
			} catch (std::exception &ex) {
				AttachedDatabase::InvokeCloseIfLastReference(live, *this);
				return ErrorResult<QueryResult>(ErrorData(ex), statement->query);
			}
			auto rewritten = WrapAsSelect(std::move(remote_ref));
			// the rewrite is invisible to the user - keep reporting the SQL they issued
			rewritten->query = std::move(statement->query);
			statement = std::move(rewritten);
			// it shares the transaction the local one holds on the connected database, if there is one
			outside_local_transaction =
			    !transaction.HasActiveTransaction() || !MetaTransaction::Get(*this).TryGetTransaction(*live);
			AttachedDatabase::InvokeCloseIfLastReference(live, *this);
			// statement is now SELECT * FROM <remote-ref>; fall through.
		}
	}
	// the statement is moved into the query - keep the source text for error reporting
	auto query = statement->query;
	const auto statement_type = statement->type;
	const bool has_parameters =
	    !statement->named_param_map.empty() || (parameters.statement_args && !parameters.statement_args->empty());
	PreprocessedStatement preprocessed;
	try {
		preprocessed = PreprocessStatementInternal(lock, std::move(statement));
		auto &statements = preprocessed.statements;
		if (statements.size() > 1 && has_parameters) {
			throw InvalidInputException(EXPANDING_PARAMETERS_ERROR);
		}
		if (verify) {
			// A later statement binds only once the earlier ones ran, so a statement that expands is verified without
			// running copies of it
			for (auto &internal_statement : statements) {
				if (statements.size() == 1) {
					CheckStatement(*this, *internal_statement, StatementCheckType::SUBMISSION);
				}
				StatementVerification(lock, internal_statement, parameters, statements.size() == 1);
			}
		}
	} catch (std::exception &ex) {
		// preserve extra error data (like query location)
		return ErrorResult<QueryResult>(ErrorData(ex), query);
	}
	if (preprocessed.statements.empty()) {
		StatementProperties properties;
		properties.return_type = StatementReturnType::NOTHING;
		return EmptyResultInternal(statement_type, std::move(properties), {}, {}, parameters.format);
	}
	const bool expands = preprocessed.statements.size() > 1;
	try {
		BeginQueryInternal(lock, std::move(preprocessed), parameters, retention);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (Exception::InvalidatesDatabase(error.Type())) {
			// fatal exceptions invalidate the entire database
			auto &db_instance = DatabaseInstance::GetDatabase(*this);
			ValidChecker::Invalidate(db_instance, error.RawMessage());
		}
		return ErrorResult<QueryResult>(std::move(error), query);
	}
	active_query->outside_local_transaction = outside_local_transaction;

	unique_ptr<QueryResult> handle;
	unique_ptr<QueryResult> result;
	bool invalidate_query = true;
	try {
		if (expands) {
			handle =
			    make_uniq<QueryResult>(shared_from_this(), statement_type, parameters.format, GetClientProperties());
			active_query->handle = handle.get();
			active_query->SetOpenResult(*handle);
		}
		result = SubmitStatementInternal(lock, active_query->TakeNextStatement(), parameters);
		if (!result && ClientConfig::GetConfig(*this).get_result_collector) {
			result = RunToResultStatementInternal(lock, *handle);
		}
		if (!result) {
			result = std::move(handle);
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (!ErrorInvalidatesQueryTransaction(error.Type())) {
			// standard exceptions do not invalidate the current transaction
			invalidate_query = false;
		} else if (Exception::InvalidatesDatabase(error.Type())) {
			// fatal exceptions invalidate the entire database
			auto &db_instance = DatabaseInstance::GetDatabase(*this);
			ValidChecker::Invalidate(db_instance, error.RawMessage());
		}
		// other types of exceptions do invalidate the current transaction
		result = ErrorResult<QueryResult>(std::move(error), query);
	}
	if (handle) { // NOLINT(bugprone-use-after-move): handle is empty once it was handed out as the result
		// A handle that is not handed out is released without the context lock its Close would take
		handle->context.reset();
	}
	if (result->HasError()) {
		// query failed: abort now, unless a delegated collector's execution already ended it
		if (active_query) {
			EndQueryInternal(lock, false, invalidate_query, result->GetErrorObject());
		}
		return result;
	}
	// A collector that builds its own result object finishes the query inside the submission, so
	// the query it belonged to may already be gone
	D_ASSERT(!active_query || active_query->IsOpenResult(*result));
	return result;
}

void ClientContext::LogQueryInternal(ClientContextLock &, const string &query) {
	if (!client_data->log_query_writer) {
#ifdef DUCKDB_FORCE_QUERY_LOG
		try {
			string log_path(DUCKDB_FORCE_QUERY_LOG);
			client_data->log_query_writer = make_uniq<BufferedFileWriter>(FileSystem::GetFileSystem(*this), log_path,
			                                                              BufferedFileWriter::DEFAULT_OPEN_FLAGS);
		} catch (...) {
			return;
		}
#else
		return;
#endif
	}
	// log query path is set: log the query
	client_data->log_query_writer->WriteData(const_data_ptr_cast(query.c_str()), query.size());
	client_data->log_query_writer->WriteData(const_data_ptr_cast("\n"), 1);
	client_data->log_query_writer->Flush();
	client_data->log_query_writer->Sync();
}

unique_ptr<QueryResult> ClientContext::Query(unique_ptr<SQLStatement> statement, const QueryParameters &parameters) {
	auto lock = LockContext();
	try {
		if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
			return refusal;
		}
	} catch (std::exception &ex) {
		return ErrorResult<QueryResult>(ErrorData(ex), statement->query);
	}
	auto result = SubmitInternal(*lock, std::move(statement), parameters, true, Retention::RETAINED_AT_SUBMISSION);
	if (result->HasError()) {
		if (transaction.HasActiveTransaction() && transaction.GetAutoRollback()) {
			transaction.Rollback(result->GetErrorObject());
		}
		return result;
	}
	return CompleteInternal(*lock, std::move(result));
}

unique_ptr<QueryResult> ClientContext::Query(const string &query, QueryParameters query_parameters) {
	auto lock = LockContext();
	auto result = QueryInternal(*lock, query, std::move(query_parameters));
	// a query that fails before any statement runs, e.g. with a parser error, has started the profiler
	ErrorData error;
	EndQueryProfiling(error);
	if (error.HasError() && !result->HasError()) {
		return ErrorResult<QueryResult>(std::move(error), query);
	}
	return result;
}

unique_ptr<QueryResult> ClientContext::QueryInternal(ClientContextLock &lock, const string &query,
                                                     QueryParameters query_parameters) {
	// The lazy path drives its own parse iterator, so it clears leftover query state (interrupt flag, etc.) itself
	if (auto refusal = InitialCleanup<QueryResult>(lock)) {
		return refusal;
	}
	auto &profiler = QueryProfiler::Get(*this);
	profiler.StartQuery(query);
	// ParseIterator's constructor runs UTF-8 validation / Unicode-space strip and can throw — route
	// through ErrorResult so the source location attaches the same way Peek failures do. ErrorResult also applies
	// the invalidation policy.
	optional_ptr<StatementIterator> iterator_ptr;
	unique_ptr<StatementIterator> iterator_storage;
	try {
		iterator_storage = make_uniq<StatementIterator>(ParseIterator(*this, query, false, lock));
		iterator_ptr = *iterator_storage;
	} catch (const std::exception &ex) {
		return ErrorResult<QueryResult>(ErrorData(ex), query);
	}
	auto &iterator = *iterator_ptr;

	auto peek_or_error = [&](bool &has_now) -> unique_ptr<QueryResult> {
		try {
			has_now = iterator.Peek();
			return nullptr;
		} catch (const std::exception &ex) {
			return ErrorResult<QueryResult>(ErrorData(ex), query);
		}
	};

	bool has_current = false;
	if (auto error = peek_or_error(has_current)) {
		return error;
	}
	if (!has_current) {
		// no statements, return empty successful result
		StatementProperties properties;
		vector<Identifier> names;
		auto collection = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator());
		return make_uniq<QueryResult>(StatementType::INVALID_STATEMENT, properties, std::move(names),
		                              std::move(collection), GetClientProperties());
	}

	unique_ptr<QueryResult> result;
	optional_ptr<QueryResult> last_result;
	bool last_had_result = false;
	while (has_current) {
		// The next user statement. Its submission preprocesses it, seeing the transaction state the previous
		// statement left, and runs what it expands into as one query
		unique_ptr<SQLStatement> statement;
		try {
			statement = iterator.GetStatementForExecution();
		} catch (const std::exception &ex) {
			return ErrorResult<QueryResult>(ErrorData(ex), query);
		}

		// Look ahead WITHOUT parsing: HasMore() only walks the token cursor, so it never parses (and
		// never throws) the next statement here. The next statement is parsed later, in this loop's
		// next GetStatementForExecution, after the current statement has executed. This lets a statement
		// register grammar (e.g. LOAD an extension) that a following statement then uses.
		bool has_next = iterator.HasMore();

		if (has_next && query_parameters.statement_args && !query_parameters.statement_args->empty()) {
			return ErrorResult<QueryResult>(
			    ErrorData(InvalidInputException("Cannot prepare multiple statements at once!")), query);
		}
		if (statement) {
			// Every statement completes before the call returns, so none of them leaves a producer parked for the
			// consumer's choice
			auto current_result =
			    SubmitInternal(lock, std::move(statement), query_parameters, true, Retention::RETAINED_AT_SUBMISSION);
			if (!current_result->HasError()) {
				current_result = CompleteInternal(lock, std::move(current_result));
			}
			if (current_result->HasError()) {
				if (transaction.HasActiveTransaction() && transaction.GetAutoRollback()) {
					transaction.Rollback(current_result->GetErrorObject());
				}
				// Reset the interrupted flag, this was set by the task that found the error
				// Next statements should not be bothered by that interruption
				interrupt_state = ClientInterruptState::NOT_INTERRUPTED;
				return current_result;
			}
			// now append the result to the list of results; a completed result knows its metadata
			auto has_result = current_result->GetStatementProperties().return_type == StatementReturnType::QUERY_RESULT;
			if (!last_result || !last_had_result) {
				// first result of the query
				result = std::move(current_result);
				last_result = result.get();
				last_had_result = has_result;
			} else if (has_result) {
				// later results; attach to the result chain, but only if there is a result
				last_result->next = std::move(current_result);
				last_result = last_result->next.get();
			}
			D_ASSERT(last_result);
		}

		has_current = has_next;
	}
	return result;
}

unique_ptr<QueryResult> ClientContext::Query(const string &query, shared_ptr<ResultFormat> format) {
	return Query(query, QueryParameters(std::move(format)));
}

unique_ptr<QueryResult> ClientContext::Query(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format) {
	return Query(std::move(statement), QueryParameters(std::move(format)));
}

unique_ptr<QueryResult> ClientContext::Submit(const string &query, const QueryParameters &parameters) {
	auto lock = LockContext();
	try {
		if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
			return refusal;
		}

		// The submission preprocesses the statement, which may expand into several that run as one query
		auto statements = ParseStatementsInternal(*lock, query);
		if (statements.empty()) {
			throw InvalidInputException("No statement to prepare!");
		}
		if (statements.size() > 1) {
			throw InvalidInputException("Cannot prepare multiple statements at once!");
		}

		return SubmitInternal(*lock, std::move(statements[0]), parameters, true);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		// a parse error has started the profiler without running a statement
		EndQueryProfiling(error);
		return ErrorResult<QueryResult>(std::move(error), query);
	}
}

unique_ptr<QueryResult> ClientContext::Submit(unique_ptr<SQLStatement> statement, const QueryParameters &parameters) {
	auto lock = LockContext();
	try {
		if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
			return refusal;
		}

		return SubmitInternal(*lock, std::move(statement), parameters, true);
	} catch (std::exception &ex) {
		return make_uniq<QueryResult>(ErrorData(ex));
	}
}

unique_ptr<QueryResult> ClientContext::Submit(const string &query, shared_ptr<ResultFormat> format) {
	return Submit(query, QueryParameters(std::move(format)));
}

unique_ptr<QueryResult> ClientContext::Submit(unique_ptr<SQLStatement> statement, shared_ptr<ResultFormat> format) {
	return Submit(std::move(statement), QueryParameters(std::move(format)));
}

unique_ptr<QueryResult> ClientContext::Submit(const string &query, identifier_map_t<BoundParameterData> &values,
                                              QueryParameters parameters) {
	parameters.statement_args = values;
	return Submit(query, parameters);
}

unique_ptr<QueryResult> ClientContext::Submit(unique_ptr<SQLStatement> statement,
                                              identifier_map_t<BoundParameterData> &values,
                                              QueryParameters parameters) {
	parameters.statement_args = values;
	return Submit(std::move(statement), parameters);
}

unique_ptr<QueryResult> ClientContext::RunInternalStatement(unique_ptr<SQLStatement> statement,
                                                            const QueryParameters &parameters) {
	auto lock = LockContext();
	try {
		if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
			return refusal;
		}
	} catch (std::exception &ex) {
		return ErrorResult<QueryResult>(ErrorData(ex), statement->query);
	}
	return RunStatementInternal(*lock, std::move(statement), parameters, false);
}

unique_ptr<QueryResult> ClientContext::SubmitInternalStatement(unique_ptr<SQLStatement> statement,
                                                               const QueryParameters &parameters) {
	auto lock = LockContext();
	try {
		if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
			return refusal;
		}
	} catch (std::exception &ex) {
		return ErrorResult<QueryResult>(ErrorData(ex), statement->query);
	}
	return SubmitInternal(*lock, std::move(statement), parameters, false);
}

unique_ptr<QueryResult> ClientContext::SubmitInternal(ClientContextLock &lock, unique_ptr<SQLStatement> statement,
                                                      const QueryParameters &parameters, bool verify,
                                                      Retention retention) {
	// before anything that can fail, so a failed statement never reports the previous one's total
	QueryProfiler::Get(*this).ResetBytesScanned();
	return SubmitStatement(lock, std::move(statement), parameters, verify, retention);
}

void ClientContext::Interrupt() {
	ClientInterruptState expected = ClientInterruptState::NOT_INTERRUPTED;
	interrupt_state.compare_exchange_strong(expected, ClientInterruptState::INTERRUPTED);
}

bool ClientContext::IsInterrupted() const {
	auto state = interrupt_state.load(std::memory_order_relaxed);
	if (state == ClientInterruptState::INTERRUPTS_SUPPRESSED) {
		return false;
	}
	// an internal connection is also interrupted when its base client is
	return state == ClientInterruptState::INTERRUPTED || (base_context && base_context->IsInterrupted());
}

void ClientContext::ClearInterrupt() {
	interrupt_state = ClientInterruptState::NOT_INTERRUPTED;
}

void ClientContext::SuppressInterrupts() {
	interrupt_state = ClientInterruptState::INTERRUPTS_SUPPRESSED;
}

void ClientContext::InterruptCheck() const {
	// Counter for throttling timeout checks - only check every N iterations
	static constexpr uint32_t TIMEOUT_CHECK_INTERVAL = 256;
	thread_local uint32_t timeout_check_counter = 0;

	if (IsInterrupted()) {
		throw InterruptException();
	}
	// Only check timeout every N calls to avoid expensive steady_clock::now() syscall
	if (HasQueryDeadline() && ++timeout_check_counter % TIMEOUT_CHECK_INTERVAL == 0) {
		BlockingInterruptCheck();
	}
}

void ClientContext::BlockingInterruptCheck() const {
	if (IsInterrupted()) {
		throw InterruptException();
	}
	if (QueryDeadlinePassed()) {
		throw InterruptException(QUERY_DEADLINE_ERROR);
	}
}

bool ClientContext::HasQueryDeadline() const {
	return query_deadline.IsValid() || (base_context && base_context->HasQueryDeadline());
}

bool ClientContext::QueryDeadlinePassed() const {
	if (query_deadline.IsValid()) {
		auto now = NumericCast<idx_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		if (now >= query_deadline.GetIndex()) {
			return true;
		}
	}
	// an internal connection is also out of time when the query it runs on behalf of is
	return base_context && base_context->QueryDeadlinePassed();
}

void ClientContext::CancelTransaction() {
	auto lock = LockContext();
	AbandonActiveQuery(*lock);
}

void ClientContext::EnableProfiling() {
	auto lock = LockContext();
	auto &client_config = ClientConfig::GetConfig(*this);
	client_config.enable_profiler = true;
}

void ClientContext::DisableProfiling() {
	auto lock = LockContext();
	auto &client_config = ClientConfig::GetConfig(*this);
	client_config.enable_profiler = false;
}

void ClientContext::RegisterFunction(CreateFunctionInfo &info) {
	RunFunctionInTransaction([&]() {
		auto existing_function = Catalog::GetEntry<ScalarFunctionCatalogEntry>(
		    *this,
		    QualifiedName(Identifier::InvalidCatalog(), info.GetQualifiedName().Schema(), info.GetFunctionName()),
		    OnEntryNotFound::RETURN_NULL);
		if (existing_function) {
			auto &new_info = info.Cast<CreateScalarFunctionInfo>();
			if (new_info.functions.MergeFunctionSet(existing_function->functions)) {
				// function info was updated from catalog entry, rewrite is needed
				info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
			}
		}
		// create function
		auto &catalog = Catalog::GetSystemCatalog(*this);
		catalog.CreateFunction(*this, info);
	});
}

void ClientContext::RunTransactionStatementInternal(const TransactionInfo &info) {
	auto type = info.type;
	if (type == TransactionType::COMMIT && transaction.HasActiveTransaction() &&
	    ValidChecker::IsInvalidated(ActiveTransaction())) {
		// transaction is invalidated - turn COMMIT into ROLLBACK
		type = TransactionType::ROLLBACK;
	}
	switch (type) {
	case TransactionType::BEGIN_TRANSACTION: {
		if (!transaction.IsAutoCommit()) {
			throw TransactionException("cannot start a transaction within a transaction");
		}
		transaction.SetAutoCommit(false);
		if (info.modifier == TransactionModifierType::TRANSACTION_READ_ONLY) {
			transaction.SetReadOnly();
		}
		transaction.SetInvalidationPolicy(info.invalidation_policy);
		transaction.SetAutoRollback(info.auto_rollback);
		if (Settings::Get<ImmediateTransactionModeSetting>(*this)) {
			auto databases = DatabaseManager::Get(*this).GetDatabases(*this);
			for (auto &attached : databases) {
				if (ValidChecker::IsInvalidated(*attached)) {
					continue;
				}
				transaction.ActiveTransaction().GetTransaction(*attached);
			}
		}
		break;
	}
	case TransactionType::COMMIT:
		if (transaction.IsAutoCommit()) {
			throw TransactionException("cannot commit - no transaction is active");
		}
		transaction.Commit();
		// The commit is irreversible, so ignore interrupts until the next query.
		SuppressInterrupts();
		break;
	case TransactionType::ROLLBACK: {
		if (transaction.IsAutoCommit()) {
			throw TransactionException("cannot rollback - no transaction is active");
		}
		auto &valid_checker = ValidChecker::Get(transaction.ActiveTransaction());
		if (valid_checker.IsInvalidated()) {
			ErrorData error(ExceptionType::TRANSACTION, valid_checker.InvalidatedMessage());
			transaction.Rollback(error);
		} else {
			transaction.Rollback(nullptr);
		}
		break;
	}
	default:
		throw NotImplementedException("Unrecognized transaction type!");
	}
}

void ClientContext::RunTransactionStatement(const TransactionInfo &info) {
	auto lock = LockContext();
	if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
		refusal->ThrowError();
	}
	if (is_connected) {
		auto statement = make_uniq<TransactionStatement>(info.Copy());
		statement->query = info.ToString();
		QueryParameters parameters;
		auto result = RunStatementInternal(*lock, std::move(statement), parameters);
		if (result->HasError()) {
			if (transaction.HasActiveTransaction() && transaction.GetAutoRollback()) {
				transaction.Rollback(result->GetErrorObject());
			}
			ClearInterrupt();
			result->ThrowError();
		}
		return;
	}
	auto &db_instance = DatabaseInstance::GetDatabase(*this);
	if (ValidChecker::IsInvalidated(db_instance)) {
		throw ErrorManager::InvalidatedDatabase(*this, ValidChecker::InvalidatedMessage(db_instance));
	}
	RunTransactionStatementInternal(info);
}

void ClientContext::RunFunctionInTransactionInternal(ClientContextLock &lock, const std::function<void(void)> &fun,
                                                     bool requires_valid_transaction) {
	if (requires_valid_transaction && transaction.HasActiveTransaction() &&
	    ValidChecker::IsInvalidated(ActiveTransaction())) {
		throw TransactionException(ErrorManager::FormatException(*this, ErrorType::INVALIDATED_TRANSACTION));
	}

	// check if we are on AutoCommit. In this case we should start a transaction
	bool require_new_transaction = transaction.IsAutoCommit() && !transaction.HasActiveTransaction();
	if (require_new_transaction) {
		D_ASSERT(!active_query);
		transaction.BeginTransaction();
		interrupt_state = ClientInterruptState::NOT_INTERRUPTED;
	}
	try {
		fun();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		bool invalidates_transaction = true;
		if (!ErrorInvalidatesTransaction(error.Type())) {
			// standard exceptions don't invalidate the transaction
			invalidates_transaction = false;
		} else if (Exception::InvalidatesDatabase(error.Type())) {
			auto &db_instance = DatabaseInstance::GetDatabase(*this);
			ValidChecker::Invalidate(db_instance, error.RawMessage());
		}
		if (require_new_transaction) {
			transaction.Rollback(error);
		} else if (invalidates_transaction) {
			ValidChecker::Invalidate(ActiveTransaction(), error.RawMessage());
		}
		throw;
	}
	if (require_new_transaction) {
		transaction.Commit();
	}
}

void ClientContext::RunFunctionInTransaction(const std::function<void(void)> &fun, bool requires_valid_transaction) {
	auto lock = LockContext();
	RunFunctionInTransactionInternal(*lock, fun, requires_valid_transaction);
}

unique_ptr<TableDescription> ClientContext::TableInfo(const Identifier &database_name, const Identifier &schema_name,
                                                      const Identifier &table_name) {
	unique_ptr<TableDescription> result;
	RunFunctionInTransaction([&]() {
		// Obtain the table from the catalog.
		auto table = Catalog::GetEntry<TableCatalogEntry>(*this, QualifiedName(database_name, schema_name, table_name),
		                                                  OnEntryNotFound::RETURN_NULL);
		if (!table) {
			return;
		}
		// Describe the table at its resolved location, not the input identifiers.
		auto &catalog = table->ParentCatalog();
		result = make_uniq<TableDescription>(QualifiedName(catalog.GetName(), table->ParentSchema().name, table->name));
		result->readonly = catalog.GetAttached().IsReadOnly();
		for (auto &column : table->GetColumns().Logical()) {
			result->columns.emplace_back(column.Copy());
		}
	});
	return result;
}

unique_ptr<TableDescription> ClientContext::TableInfo(const Identifier &schema_name, const Identifier &table_name) {
	return TableInfo(Identifier::InvalidCatalog(), schema_name, table_name);
}

void ClientContext::Append(unique_ptr<SQLStatement> stmt) {
	auto result = Query(std::move(stmt), QueryParameters());
	result->ThrowIfError("Failed to append: ");
}

void ClientContext::Append(TableDescription &description, ColumnDataCollection &collection) {
	Identifier table_name("__duckdb_internal_appended_data");
	vector<Identifier> expected_names;
	auto query = Appender::ConstructQuery(description, table_name, expected_names);
	auto table_ref = BaseAppender::GetColumnDataTableRef(collection, table_name, expected_names);
	auto stmt = BaseAppender::ParseStatement(*this, std::move(table_ref), query, table_name.GetIdentifierName());
	Append(std::move(stmt));
}

void ClientContext::InternalTryBindRelation(Relation &relation, vector<ColumnDefinition> &result_columns) {
	// bind the expressions
	auto binder = Binder::CreateBinder(*this);
	auto result = relation.Bind(*binder);
	D_ASSERT(result.names.size() == result.types.size());

	result_columns.reserve(result_columns.size() + result.names.size());
	for (idx_t i = 0; i < result.names.size(); i++) {
		result_columns.emplace_back(result.names[i], result.types[i]);
	}
}

void ClientContext::TryBindRelation(Relation &relation, vector<ColumnDefinition> &result_columns) {
#ifdef DEBUG
	D_ASSERT(!relation.GetAlias().empty());
	D_ASSERT(!relation.ToString().empty());
#endif
	RunFunctionInTransaction([&]() { InternalTryBindRelation(relation, result_columns); });
}

unordered_set<string> ClientContext::GetTableNames(const string &query, const bool qualified) {
	auto lock = LockContext();

	// Preprocess before binding so PRAGMA reparse / macro expansion happens up front — GetTableNames
	// extracts names from the *underlying* query (e.g. `PRAGMA tpch(1)` -> the TPC-H SELECT, whose
	// tables are what the caller wants). A raw, un-preprocessed PRAGMA would never surface them.
	auto statements = ParseStatementsInternal(*lock, query, false);
	if (statements.size() != 1) {
		throw InvalidInputException("Expected a single statement");
	}
	CheckStatement(*this, *statements[0], StatementCheckType::INSPECTION);
	statements[0] = ExpandSingleStatementInternal(*lock, std::move(statements[0]), "Expected a single statement");
	CheckStatement(*this, *statements[0], StatementCheckType::INSPECTION);

	unordered_set<string> result;
	RunFunctionInTransactionInternal(*lock, [&]() {
		// bind the expressions
		auto binder = Binder::CreateBinder(*this);
		auto mode = qualified ? BindingMode::EXTRACT_QUALIFIED_NAMES : BindingMode::EXTRACT_NAMES;
		binder->SetBindingMode(mode);
		binder->Bind(*statements[0]);
		result = binder->GetTableNames();
	});
	return result;
}

unique_ptr<QueryResult> ClientContext::SubmitInternal(ClientContextLock &lock, const shared_ptr<Relation> &relation,
                                                      const QueryParameters &query_parameters) {
	if (auto refusal = InitialCleanup<QueryResult>(lock)) {
		return refusal;
	}

#ifdef DEBUG
	// run the ToString method of any relation we run, mostly to ensure it doesn't crash
	relation->ToString();
	relation->GetAlias();
#endif

	auto relation_stmt = make_uniq<RelationStatement>(relation);
	return SubmitInternal(lock, std::move(relation_stmt), query_parameters);
}

unique_ptr<QueryResult> ClientContext::Submit(const shared_ptr<Relation> &relation,
                                              const QueryParameters &query_parameters) {
	auto lock = LockContext();
	return SubmitInternal(*lock, relation, query_parameters);
}

unique_ptr<QueryResult> ClientContext::Execute(const shared_ptr<Relation> &relation) {
	auto lock = LockContext();
	auto &expected_columns = relation->Columns();
	if (auto refusal = InitialCleanup<QueryResult>(*lock)) {
		return refusal;
	}

	auto relation_stmt = make_uniq<RelationStatement>(relation);
	QueryParameters parameters;
	auto result = SubmitInternal(*lock, std::move(relation_stmt), parameters, true, Retention::RETAINED_AT_SUBMISSION);
	if (result->HasError()) {
		return result;
	}
	result = CompleteInternal(*lock, std::move(result));
	if (result->HasError()) {
		return result;
	}
	// verify that the result types and result names of the query match the expected result types/names
	if (result->GetTypes().size() == expected_columns.size()) {
		bool mismatch = false;
		for (idx_t i = 0; i < result->GetTypes().size(); i++) {
			if (result->GetTypes()[i] != expected_columns[i].Type() ||
			    result->ColumnName(i) != expected_columns[i].Name()) {
				mismatch = true;
				break;
			}
		}
		if (!mismatch) {
			// all is as expected: return the result
			return result;
		}
	}
	// result mismatch
	string err_str = "Result mismatch in query!\nExpected the following columns: [";
	for (idx_t i = 0; i < expected_columns.size(); i++) {
		if (i > 0) {
			err_str += ", ";
		}
		err_str += expected_columns[i].Name() + " " + expected_columns[i].Type().ToString();
	}
	err_str += "]\nBut result contained the following: ";
	for (idx_t i = 0; i < result->GetTypes().size(); i++) {
		err_str += i == 0 ? "[" : ", ";
		err_str += result->ColumnName(i) + " " + result->GetTypes()[i].ToString();
	}
	err_str += "]";
	return ErrorResult<QueryResult>(ErrorData(err_str));
}

SettingLookupResult ClientContext::TryGetCurrentSetting(const Identifier &key, Value &result) const {
	optional_ptr<const ConfigurationOption> option;
	// try to get the setting index
	auto &db_config = DBConfig::GetConfig(*this);
	auto setting_index = db_config.TryGetSettingIndex(key, option);
	if (setting_index.IsValid()) {
		// generic setting - try to fetch it
		auto lookup_result =
		    config.user_settings.TryGetSetting(db_config.user_settings, setting_index.GetIndex(), result);
		if (lookup_result) {
			return lookup_result;
		}
	}
	if (option && option->get_setting) {
		// legacy callback
		result = option->get_setting(*this);
		return SettingLookupResult(SettingScope::LOCAL);
	}
	// setting is not set - get the default value
	return DBConfig::TryGetDefaultValue(option, result);
}

SettingLookupResult ClientContext::TryGetCurrentUserSetting(idx_t setting_index, Value &result) const {
	auto &db_config = DBConfig::GetConfig(*this);
	return config.user_settings.TryGetSetting(db_config.user_settings, setting_index, result);
}

ParserOptions ClientContext::GetParserOptions() {
	ParserOptions options;
	options.identifier_case_mode = Settings::Get<PreserveIdentifierCaseSetting>(*this);
	options.integer_division = Settings::Get<IntegerDivisionSetting>(*this);
	options.regex_match_operator_semantics = Settings::Get<RegexMatchOperatorSemanticsSetting>(*this);
	options.max_expression_depth = Settings::Get<MaxExpressionDepthSetting>(*this);
	options.extensions = DBConfig::GetConfig(*this).GetCallbackManager();
	options.parser_override_setting = Settings::Get<AllowParserOverrideExtensionSetting>(*this);
	options.compiled_grammar = CompiledGrammar::Get(*this);
	options.context = *this;
	return options;
}

ClientProperties ClientContext::GetClientProperties() {
	string timezone = "UTC";
	TryGetCurrentSetting("TimeZone", timezone);
	ArrowOffsetSize arrow_offset_size = ArrowOffsetSize::REGULAR;
	if (Settings::Get<ArrowLargeBufferSizeSetting>(*this)) {
		arrow_offset_size = ArrowOffsetSize::LARGE;
	}
	bool arrow_use_list_view = Settings::Get<ArrowOutputListViewSetting>(*this);
	bool arrow_lossless_conversion = Settings::Get<ArrowLosslessConversionSetting>(*this);
	bool arrow_use_string_view = Settings::Get<ProduceArrowStringViewSetting>(*this);
	auto arrow_format_version = Settings::Get<ArrowOutputVersionSetting>(*this);
	return {timezone,
	        arrow_offset_size,
	        arrow_use_list_view,
	        arrow_use_string_view,
	        arrow_lossless_conversion,
	        arrow_format_version,
	        this};
}

LogicalType ClientContext::ParseLogicalType(const string &type) {
	auto lock = LockContext();
	LogicalType logical_type;
	RunFunctionInTransactionInternal(*lock,
	                                 [&]() { logical_type = TypeManager::Get(*db).ParseLogicalType(type, *this); });
	return logical_type;
}

} // namespace duckdb
