#include "duckdb/main/client_context.hpp"
#include "duckdb/common/operator/add.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/helper/physical_result_collector.hpp"
#include "duckdb/execution/operator/helper/physical_result_sink.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/main/active_query_context.hpp"
#include "duckdb/main/buffered_data/batched_buffered_data.hpp"
#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/result_format.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction_context.hpp"

namespace duckdb {

void ClientContext::BeginQueryInternal(ClientContextLock &lock, PreprocessedStatement statements,
                                       const QueryParameters &parameters, Retention retention) {
	// check if we are on AutoCommit. In this case we should start a transaction
	D_ASSERT(!active_query);
	D_ASSERT(!statements.statements.empty());
	// not reset on Rollback, so a statement that fails before StartQuery must not report the previous one's
	QueryProfiler::Get(*this).ResetBytesScanned();
	auto &db_inst = DatabaseInstance::GetDatabase(*this);
	if (ValidChecker::IsInvalidated(db_inst)) {
		throw ErrorManager::InvalidatedDatabase(*this, ValidChecker::InvalidatedMessage(db_inst));
	}
	auto query = statements.statements[0]->query;
	active_query = make_uniq<ActiveQueryContext>(std::move(statements));
	active_query->format = parameters.format;
	if (retention == Retention::RETAINED_AT_SUBMISSION) {
		active_query->retention = ResultLifetime::RETAINED;
	}
	if (transaction.IsAutoCommit() && !transaction.HasActiveTransaction()) {
		transaction.BeginTransaction();
	}
	BeginStatementInternal(lock, query);

	query_progress.Initialize();
	// Set query deadline if max_execution_time is configured
	auto max_execution_time = Settings::Get<MaxExecutionTimeSetting>(*this);
	if (max_execution_time > 0) {
		auto now_ms = NumericCast<idx_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		idx_t deadline;
		if (TryAddOperator::Operation<idx_t, idx_t, idx_t>(now_ms, NumericCast<idx_t>(max_execution_time), deadline)) {
			query_deadline = deadline;
		} else {
			// the deadline is too far in the future to be represented - there is effectively no deadline
			query_deadline.SetInvalid();
		}
	} else {
		query_deadline.SetInvalid();
	}
	// Notify any registered state of query begin
	for (auto &state : context->GetRegisteredState().States()) {
		state->QueryBegin(*this);
	}
	RefreshLoggerInternal();
}

void ClientContext::BeginStatementInternal(ClientContextLock &lock, const string &query) {
	transaction.SetActiveQuery(db->GetDatabaseManager().GetNewQueryNumber());
	LogQueryInternal(lock, query);
	active_query->query = query;
}

void ClientContext::RefreshLoggerInternal() {
	// Flush the old logger.
	context->GetLogger().Flush();

	// Refresh the logger to ensure we are in sync with the global log settings.
	LoggingContext logging_context(LogContextScope::CONNECTION);
	logging_context.connection_id = connection_id;
	logging_context.transaction_id = transaction.ActiveTransaction().global_transaction_id;
	logging_context.query_id = transaction.GetActiveQuery();
	ResetLoggerInternal(logging_context);
	DUCKDB_LOG(*this, QueryLogType, active_query->query);
}

void ClientContext::ResetLoggerInternal(const LoggingContext &logging_context) {
	context->SetLogger(db->GetLogManager().CreateLogger(logging_context, log_config));
}

ErrorData ClientContext::EndQueryInternal(ClientContextLock &lock, bool success, bool invalidate_transaction,
                                          optional_ptr<ErrorData> previous_error) {
	D_ASSERT(active_query.get());
	// whether the statement that finished, or an earlier statement of a query that expands, modifies a database
	const bool statement_wrote =
	    active_query->Wrote() || (active_query->prepared && !active_query->prepared->properties.IsReadOnly());
	auto statement_error = EndStatementInternal(lock, previous_error);
	// a statement that a state failed to end did not succeed, the failure is the query's error unless it has one
	const bool report_statement_error = statement_error.HasError() && !previous_error;
	if (statement_error.HasError()) {
		success = false;
	}
	if (report_statement_error) {
		// what the statement wrote can only be dropped with the whole transaction
		invalidate_transaction =
		    invalidate_transaction || statement_wrote || ErrorInvalidatesQueryTransaction(statement_error.Type());
		ProcessError(statement_error, active_query->query);
		previous_error = &statement_error;
	}
	invalidate_transaction = invalidate_transaction && !active_query->outside_local_transaction;
	active_query.reset();
	query_deadline.SetInvalid();
	query_progress.Initialize();
	ErrorData error;
	try {
		if (transaction.HasActiveTransaction()) {
			transaction.ResetActiveQuery();
			if (transaction.IsAutoCommit()) {
				if (success) {
					transaction.Commit();
				} else {
					transaction.Rollback(previous_error);
				}
			} else if (invalidate_transaction) {
				D_ASSERT(!success);
				if (transaction.GetAutoRollback()) {
					transaction.Rollback(previous_error);
				} else {
					ValidChecker::Invalidate(ActiveTransaction(), "Failed to commit");
				}
			}
		}
	} catch (std::exception &ex) {
		error = ErrorData(ex);
		if (Exception::InvalidatesDatabase(error.Type()) || error.Type() == ExceptionType::INTERNAL) {
			auto &db_inst = DatabaseInstance::GetDatabase(*this);
			ValidChecker::Invalidate(db_inst, error.RawMessage());
		}
	} catch (...) { // LCOV_EXCL_START
		error = ErrorData("Unhandled exception!");
	} // LCOV_EXCL_STOP

	EndQueryProfiling(error);

	// Refresh the logger
	context->GetLogger().Flush();
	LoggingContext logging_context(LogContextScope::CONNECTION);
	logging_context.connection_id = connection_id;
	ResetLoggerInternal(logging_context);

	if (!error.HasError() && report_statement_error) {
		// the failure to end the statement is reported on the result the way a failed commit is
		error = statement_error;
	}

	// Notify any registered state of query end
	for (auto const &s : context->GetRegisteredState().States()) {
		if (error.HasError()) {
			s->QueryEnd(*this, &error);
		} else {
			s->QueryEnd(*this, previous_error);
		}
	}
	return error;
}

void ClientContext::EndQueryProfiling(ErrorData &error) {
	// this also runs while a connection with an open result is destroyed, so it must not throw
	try {
		QueryProfiler::Get(*context).EndQuery();
	} catch (std::exception &ex) {
		if (!error.HasError()) {
			error = ErrorData(ex);
		}
	} catch (...) { // LCOV_EXCL_START
		if (!error.HasError()) {
			error = ErrorData("Unhandled exception!");
		}
	} // LCOV_EXCL_STOP
}

void ClientContext::CleanupInternal(ClientContextLock &lock, BaseQueryResult *result, bool invalidate_transaction) {
	if (!active_query) {
		// no query currently active
		return;
	}
	if (active_query->executor) {
		// Read before CancelTasks clears the slot, and while the profiler is still running
		auto buffer = active_query->executor->GetResultBuffer();
		if (buffer) {
			QueryProfiler::Get(*this).SetStreamingPeakBufferSize(buffer->PeakStreamingBytes());
		}
		active_query->executor->CancelTasks();
	}
	active_query->progress_bar.reset();

	// Relaunch the threads if a SET THREADS command was issued
	auto &scheduler = TaskScheduler::GetScheduler(*this);
	scheduler.RelaunchThreads();

	optional_ptr<ErrorData> passed_error = nullptr;
	if (result && result->HasError()) {
		passed_error = result->GetErrorObject();
	}
	auto error = EndQueryInternal(lock, result ? !result->HasError() : false, invalidate_transaction, passed_error);
	if (result && !result->HasError()) {
		// if an error occurred while ending the statement or committing report it in the result
		result->SetError(error);
	}
	D_ASSERT(!active_query);
}

void ClientContext::AbortInternal(ClientContextLock &lock) {
	D_ASSERT(active_query);
	auto &prepared = active_query->prepared;
	// No savepoint isolates a single statement, so partial writes can only be dropped with the whole transaction. A
	// statement that expands into several and is abandoned with statements left would leave them unapplied, and those
	// are not bound, so they count as writing
	const bool may_write = !prepared || !prepared->properties.IsReadOnly() || active_query->HasNextStatement();
	CleanupInternal(lock, nullptr, may_write);
}

Executor &ClientContext::GetExecutor() {
	D_ASSERT(active_query);
	D_ASSERT(active_query->executor);
	return *active_query->executor;
}

const string &ClientContext::GetCurrentQuery() {
	D_ASSERT(active_query);
	return active_query->query;
}

bool ClientContext::ActiveQueryIsLastStatement() const {
	return !active_query || !active_query->HasNextStatement();
}

void BindPreparedStatementParameters(ClientContext &context, PreparedStatementData &statement,
                                     const QueryParameters &parameters) {
	identifier_map_t<BoundParameterData> owned_values;
	if (parameters.statement_args) {
		auto &params = *parameters.statement_args;
		for (auto &val : params) {
			owned_values.emplace(val);
		}
	}
	statement.Bind(context, owned_values);
}

unique_ptr<QueryResult> ClientContext::SubmitPreparedStatementInternal(
    ClientContextLock &lock, shared_ptr<PreparedStatementData> statement_data_p, const QueryParameters &parameters) {
	D_ASSERT(active_query);
	auto &statement_data = *statement_data_p;
	BindPreparedStatementParameters(*this, statement_data, parameters);
	// The plan must outlive the executor, also when Initialize throws
	active_query->prepared = std::move(statement_data_p);

	// Create the query executor.
	active_query->executor = make_uniq<Executor>(*this);
	auto &executor = *active_query->executor;

	auto &config = ClientConfig::GetConfig(*this);
	if (Settings::Get<EnableProgressBarSetting>(*this)) {
		progress_bar_display_create_func_t display_create_func = nullptr;
		if (Settings::Get<EnableProgressBarPrintSetting>(*this)) {
			// Use either a custom display function, or the default.
			display_create_func =
			    config.display_create_func ? config.display_create_func : ProgressBar::DefaultProgressBarDisplay;
		}
		active_query->progress_bar = make_uniq<ProgressBar>(
		    executor, NumericCast<idx_t>(Settings::Get<ProgressBarTimeSetting>(*this)), display_create_func);
		active_query->progress_bar->Start();
		query_progress.Restart();
	}

	// The result statement of a query that expands into several is the one that returns rows; the others only run
	const bool expands = active_query->ExpandsIntoSeveral();
	const bool result_statement =
	    !expands || statement_data.properties.return_type == StatementReturnType::QUERY_RESULT;
	if (expands && result_statement && active_query->HasNextStatement()) {
		throw NotImplementedException("A statement that expands into a row-returning statement followed by others "
		                              "cannot be served as one result");
	}

	// Decide how to get the result collector.
	get_result_collector_t get_collector = PhysicalResultCollector::GetResultCollector;
	auto &client_config = ClientConfig::GetConfig(*this);
	if (client_config.get_result_collector && result_statement) {
		get_collector = client_config.get_result_collector;
	}

	// Get the result collector and initialize the executor.
	auto collector = get_collector(*this, statement_data);
	D_ASSERT(collector->type == PhysicalOperatorType::RESULT_COLLECTOR);
	// A custom hook can hand back the default sink, which is then served like any other query
	const bool delegating = collector->Cast<PhysicalResultCollector>().BuildsOwnResult();
	if (delegating && expands && !active_query->inside_submission) {
		throw NotImplementedException("A result collector that builds its own result object can only serve a "
		                              "statement that expands into several when it is set before the submission");
	}
	if (delegating && parameters.format) {
		if (!parameters.format->Is<ChunkFormat>()) {
			// The collector builds its own result, which the format would never reach
			throw InvalidInputException("A result format cannot be combined with a custom result collector");
		}
		if (parameters.format->Cast<ChunkFormat>().MemoryType() == QueryResultMemoryType::BUFFER_MANAGED) {
			// The collector chooses its own store, so the request would be silently downgraded
			throw InvalidInputException("A buffer-managed result cannot be combined with a custom result collector");
		}
	}

	// Read before Initialize starts the workers: a SET statement writes the settings from a task
	auto client_properties = GetClientProperties();
	auto types = statement_data.types;

	// The buffer is created here, on the client thread, and handed to the sink, the executor and the
	// handle. It runs the format's InitGlobal before any worker starts, so workers only read the format state
	shared_ptr<BufferedData> buffer;
	if (!delegating) {
		auto &sink = collector->Cast<PhysicalResultSink>();
		ResultFormatContext format_context {statement_data.types, statement_data.names, client_properties,
		                                    sink.ordering};
		// The output of a statement the result does not carry is discarded, so it is kept in chunks
		auto format = result_statement ? parameters.format : nullptr;
		if (sink.ordering == ResultOrdering::BATCH_INDEX_ORDERED) {
			buffer = make_shared_ptr<BatchedBufferedData>(*this, ResultLifetime::UNDECIDED, std::move(format_context),
			                                              std::move(format));
		} else {
			buffer = make_shared_ptr<SimpleBufferedData>(*this, ResultLifetime::UNDECIDED, std::move(format_context),
			                                             std::move(format));
		}
		// Settled before execution starts, so no producer parks for a decision that is already made, or that no
		// consumer makes for a statement whose output is discarded
		if (!result_statement) {
			buffer->Decide(ResultLifetime::RETAINED);
		} else if (active_query->retention != ResultLifetime::UNDECIDED) {
			buffer->Decide(active_query->retention);
		}
		sink.SetResultBuffer(buffer);
	}
	executor.SetResultBuffer(buffer);

	if (expands) {
		// Recorded before Initialize schedules the statement's tasks, which may write before it returns
		active_query->RecordDatabases(statement_data.properties);
	}
	executor.Initialize(std::move(collector));

	D_ASSERT(executor.GetTypes() == statement_data.types);
	if (expands) {
		if (!result_statement) {
			return nullptr;
		}
		active_query->result_statement_started = true;
		auto &handle = *active_query->handle;
		if (!delegating) {
			handle.AdoptResultStatement(active_query->QueryProperties(statement_data.properties), statement_data,
			                            std::move(buffer));
			return nullptr;
		}
		// The handle is released first, so destroying it never takes the context lock held here
		handle.context.reset();
		return CompleteDelegatedInternal(lock, handle);
	}
	D_ASSERT(!active_query->HasOpenResult());

	auto result = make_uniq<QueryResult>(shared_from_this(), statement_data, std::move(types),
	                                     std::move(client_properties), std::move(buffer));
	active_query->SetOpenResult(*result);
	if (delegating) {
		// The collector builds its own result object: run the query and hand that object out. The
		// handle is released first, so destroying it never takes the context lock held here
		result->context.reset();
		auto produced = CompleteDelegatedInternal(lock, *result);
		if (produced) {
			return produced;
		}
	}
	return result;
}

unique_ptr<QueryResult> ClientContext::CompleteDelegatedInternal(ClientContextLock &lock, QueryResult &result) {
	QueryResultState state;
	while (!IsObservable(state = ExecuteTaskInternal(lock, result))) {
		// As in QueryResult::CompleteInternal: wait while other threads run the remaining tasks, never spin
		if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
			WaitForTask(lock, result);
		}
	}
	if (result.HasError()) {
		// The error is on the handle, which the caller hands out instead
		return nullptr;
	}
	auto &executor = GetExecutor();
	auto produced = executor.GetResult();
	if (executor.HasStreamingResultCollector()) {
		active_query->SetOpenResult(*produced);
		active_query->collector_built_result = true;
	} else {
		CleanupInternal(lock, produced.get(), false);
	}
	return produced;
}

void ClientContext::WaitForTask(ClientContextLock &lock, BaseQueryResult &result) {
	auto &executor = *active_query->executor;
	if (executor.HasTaskInProgress()) {
		// This thread is holding a partially processed task, the next step resumes it without waiting.
		return;
	}
	executor.WaitForTask();
}

QueryResultState ClientContext::ExecuteTaskInternal(ClientContextLock &lock, BaseQueryResult &result) {
	D_ASSERT(active_query);
	D_ASSERT(active_query->IsOpenResult(result));
	try {
		// Surface a pending interrupt even when this thread runs no task that reaches InterruptCheck.
		// IsInterrupted() rather than InterruptCheck(): we must not enforce query_deadline here.
		// Skip when the executor already has an error: ExecuteTask rethrows that with its original type.
		if (IsInterrupted() && !active_query->executor->HasError()) {
			throw InterruptException();
		}
		auto state = active_query->executor->ExecuteTask();
		UpdateProgressInternal(state);
		if (state == QueryResultState::FINISHED && active_query->HasNextStatement()) {
			return StartNextStatementInternal(lock);
		}
		return state;
	} catch (std::exception &ex) {
		return FailQueryInternal(lock, result, ErrorData(ex));
	} catch (...) { // LCOV_EXCL_START
		return FailQueryInternal(lock, result, ErrorData("Unhandled exception in ExecuteTaskInternal"));
	} // LCOV_EXCL_STOP
}

QueryResultState ClientContext::PollInternal(ClientContextLock &lock, BaseQueryResult &result) {
	D_ASSERT(active_query);
	D_ASSERT(active_query->IsOpenResult(result));
	try {
		auto state = active_query->executor->Poll();
		UpdateProgressInternal(state);
		if (state == QueryResultState::FINISHED && active_query->HasNextStatement()) {
			return StartNextStatementInternal(lock);
		}
		return state;
	} catch (std::exception &ex) {
		return FailQueryInternal(lock, result, ErrorData(ex));
	} catch (...) { // LCOV_EXCL_START
		return FailQueryInternal(lock, result, ErrorData("Unhandled exception in PollInternal"));
	} // LCOV_EXCL_STOP
}

void ClientContext::UpdateProgressInternal(QueryResultState state) {
	if (!active_query->progress_bar) {
		return;
	}
	// todo: this is not correct for streaming results
	active_query->progress_bar->Update(IsObservable(state));
	query_progress = active_query->progress_bar->GetDetailedQueryProgress();
}

QueryResultState ClientContext::FailQueryInternal(ClientContextLock &lock, BaseQueryResult &result, ErrorData error) {
	bool invalidate_transaction = true;
	if (error.Type() == ExceptionType::INTERRUPT) {
		// No executor when the failure comes between two statements of a query that expands into several
		auto &executor = active_query->executor;
		if (executor && executor->HasError()) {
			// Interrupted by an exception caused in a worker thread
			error = executor->GetError();
			invalidate_transaction = ErrorInvalidatesQueryTransaction(error.Type());
		}
	} else if (!ErrorInvalidatesQueryTransaction(error.Type())) {
		invalidate_transaction = false;
	} else if (Exception::InvalidatesDatabase(error.Type()) || error.Type() == ExceptionType::INTERNAL) {
		// fatal exceptions invalidate the entire database
		auto &db_instance = DatabaseInstance::GetDatabase(*this);
		ValidChecker::Invalidate(db_instance, error.RawMessage());
	}
	ProcessError(error, active_query->query);
	result.SetError(std::move(error));
	EndQueryInternal(lock, false, invalidate_transaction, result.GetErrorObject());
	return QueryResultState::EXECUTION_ERROR;
}

void ClientContext::AbandonActiveQuery(ClientContextLock &lock) {
	if (active_query) {
		AbortInternal(lock);
	}
	interrupt_state = ClientInterruptState::NOT_INTERRUPTED;
	QueryProfiler::Get(*context).ResetQueryStart();
}

unique_ptr<QueryResult> ClientContext::CompleteInternal(ClientContextLock &lock, unique_ptr<QueryResult> result) {
	result->CompleteInternal(lock);
	return result;
}

bool ClientContext::IsActiveResult(ClientContextLock &lock, BaseQueryResult &result) {
	if (!active_query) {
		return false;
	}
	return active_query->IsOpenResult(result);
}

bool ClientContext::ExecutionIsFinished() {
	lock_guard<mutex> guard(context_lock);
	if (!active_query || !active_query->executor || active_query->HasNextStatement()) {
		return false;
	}
	return active_query->executor->ExecutionIsFinished();
}

QueryResultState ClientContext::StartNextStatementInternal(ClientContextLock &lock) {
	QueryResultState state;
	do {
		auto produced = AdvanceStatementInternal(lock);
		// Only a submission meets a collector that builds its own result object
		D_ASSERT(!produced);
		state = active_query->executor->Poll();
	} while (state == QueryResultState::FINISHED && active_query->HasNextStatement());
	return state;
}

unique_ptr<QueryResult> ClientContext::AdvanceStatementInternal(ClientContextLock &lock) {
	// Checked before the finished statement is torn down, so a failure is reported against it
	if (IsInterrupted()) {
		throw InterruptException();
	}
	if (QueryDeadlinePassed()) {
		throw InterruptException(QUERY_DEADLINE_ERROR);
	}
	auto end_error = EndStatementInternal(lock, nullptr);
	if (end_error.HasError()) {
		// a state that fails to end the statement fails the query, the way the statement failing would
		end_error.Throw();
	}
	transaction.ResetActiveQuery();
	QueryProfiler::Get(*context).EndQuery();
	auto statement = active_query->TakeNextStatement();
	BeginStatementInternal(lock, statement->query);
	RefreshLoggerInternal();
	NotifyStatementBeginInternal(lock);
	QueryParameters parameters(active_query->format);
	auto produced = SubmitStatementInternal(lock, std::move(statement), parameters);
	// A statement that could not start fails the query; the result object of a collector already ended it
	if (produced && produced->HasError() && active_query && !active_query->IsOpenResult(*produced)) {
		produced->GetErrorObject().Throw();
	}
	return produced;
}

unique_ptr<QueryResult> ClientContext::RunToResultStatementInternal(ClientContextLock &lock, QueryResult &handle) {
	active_query->inside_submission = true;
	try {
		// A collector that builds its own result object and fails ends the query
		while (active_query && !active_query->result_statement_started && active_query->HasNextStatement()) {
			auto state = active_query->executor->ExecuteTask();
			UpdateProgressInternal(state);
			if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
				WaitForTask(lock, handle);
			} else if (state == QueryResultState::FINISHED) {
				auto produced = AdvanceStatementInternal(lock);
				if (produced) {
					return produced;
				}
			}
		}
	} catch (std::exception &ex) {
		if (active_query) {
			FailQueryInternal(lock, handle, ErrorData(ex));
		} else {
			handle.SetError(ErrorData(ex));
		}
		return nullptr;
	}
	if (active_query) {
		active_query->inside_submission = false;
	}
	return nullptr;
}

ErrorData ClientContext::EndStatementInternal(ClientContextLock &lock, optional_ptr<ErrorData> error) {
	if (active_query->executor) {
		active_query->executor->CancelTasks();
		active_query->executor.reset();
	}
	active_query->progress_bar.reset();
	// The plan outlives the executor
	active_query->prepared.reset();
	ErrorData end_error;
	if (!active_query->statement_began) {
		// the states saw no begin of a statement, or its end already, which a failed step between statements leaves
		return end_error;
	}
	active_query->statement_began = false;
	// every state sees the end of the statement, also when one of them fails, and the first failure is reported
	context->GetRegisteredState().ForEachState([&](ClientContextState &state) {
		try {
			state.StatementEnd(*this, error);
		} catch (std::exception &ex) {
			ErrorData state_error(ex);
			if (Exception::InvalidatesDatabase(state_error.Type()) || state_error.Type() == ExceptionType::INTERNAL) {
				// a fatal failure invalidates the database also when another failure is the one reported
				ValidChecker::Invalidate(DatabaseInstance::GetDatabase(*this), state_error.RawMessage());
			}
			if (!end_error.HasError()) {
				end_error = std::move(state_error);
			}
		} catch (...) { // LCOV_EXCL_START
			if (!end_error.HasError()) {
				end_error = ErrorData("Unhandled exception in StatementEnd");
			}
		} // LCOV_EXCL_STOP
	});
	return end_error;
}

void ClientContext::NotifyStatementBeginInternal(ClientContextLock &lock) {
	active_query->statement_began = true;
	context->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.StatementBegin(*this); });
}

unique_ptr<QueryResult> ClientContext::TakeFinishedResultInternal(ClientContextLock &lock) {
	if (!active_query->ExpandsIntoSeveral() || active_query->result_statement_started) {
		return GetExecutor().GetResult();
	}
	// No statement returned rows: the result reports nothing, the way a single statement that returns nothing does
	StatementProperties properties;
	properties.return_type = StatementReturnType::NOTHING;
	return EmptyResultInternal(active_query->handle->GetStatementType(),
	                           active_query->QueryProperties(std::move(properties)), {LogicalType::BOOLEAN},
	                           {Identifier("Success")}, active_query->format);
}

unique_ptr<QueryResult> ClientContext::EmptyResultInternal(StatementType statement_type, StatementProperties properties,
                                                           vector<LogicalType> types, vector<Identifier> names,
                                                           shared_ptr<ResultFormat> format) {
	if (!format) {
		format = ChunkFormat::InMemory();
	}
	auto client_properties = GetClientProperties();
	ResultFormatContext format_context {types, names, client_properties, ResultOrdering::UNORDERED};
	shared_ptr<ResultFormatGlobalState> format_state = format->InitGlobal(format_context);
	auto collection = format->CreateCollection(*this, *format_state, format_context);
	// A retained collection is read only once finalized
	collection->Finalize();
	return make_uniq<QueryResult>(statement_type, std::move(properties), std::move(types), std::move(names),
	                              std::move(collection), std::move(format), std::move(format_state),
	                              std::move(client_properties));
}

ResultLifetime ClientContext::DecideResultLifetimeInternal(ClientContextLock &lock, ResultLifetime decision) {
	D_ASSERT(active_query && !active_query->result_statement_started);
	D_ASSERT(decision != ResultLifetime::UNDECIDED);
	if (active_query->retention == ResultLifetime::UNDECIDED) {
		active_query->retention = decision;
	}
	return active_query->retention;
}

} // namespace duckdb
