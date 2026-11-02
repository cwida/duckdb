#include "duckdb/main/query_result_stream.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/buffered_data/buffered_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/result_unit.hpp"

namespace duckdb {

bool StreamsByDefault(StatementType written_type, const QueryResult &result, bool reads_to_the_end) {
	// Checked first: a statement that expands into several is no SELECT, and its properties may not be known yet
	if (written_type != StatementType::SELECT_STATEMENT) {
		return false;
	}
	auto &properties = result.GetStatementProperties();
	if (properties.return_type != StatementReturnType::QUERY_RESULT) {
		return false;
	}
	return reads_to_the_end || properties.IsReadOnly();
}

ResultStreamBase::ResultStreamBase(unique_ptr<QueryResult> result, const char *expected_format)
    : handle(std::move(result)) {
	if (!handle) {
		throw InvalidInputException("Attempting to open a stream on a query result that does not exist");
	}
	if (handle->HasError()) {
		throw InvalidInputException("Attempting to open a stream on an unsuccessful query result\nError: %s",
		                            handle->GetError());
	}
	if (handle->HasBufferedData()) {
		if (handle->GetBufferedData().Decide(ResultLifetime::DRAINING) != ResultLifetime::DRAINING) {
			throw InvalidInputException("Attempting to open a stream on a query result that is being retained");
		}
	} else if (handle->context && !handle->MetadataAvailable()) {
		// A statement that expands into several: the statement producing the result takes the decision when it
		// starts
		auto keep_alive = handle->context;
		auto lock = keep_alive->LockContext();
		handle->CheckExecutableInternal(*lock);
		if (keep_alive->DecideResultLifetimeInternal(*lock, ResultLifetime::DRAINING) != ResultLifetime::DRAINING) {
			throw InvalidInputException("Attempting to open a stream on a query result that is being retained");
		}
	} else {
		throw InvalidInputException("Attempting to open a stream on a query result that has no streaming buffer");
	}
	if (!StringUtil::Equals(handle->Format().Name(), expected_format)) {
		throw InvalidInputException("Attempting to open a \"%s\" stream on a query result in the \"%s\" format",
		                            expected_format, handle->Format().Name());
	}
}

ResultStreamBase::~ResultStreamBase() {
	Close();
}

void ResultStreamBase::Close() {
	handle->Close();
}

bool ResultStreamBase::IsOpen() {
	return handle->IsOpen();
}

bool ResultStreamBase::MetadataAvailable() const {
	return handle->MetadataAvailable();
}

QueryResultState
ResultStreamBase::GuardedInternal(const char *name,
                                  const std::function<QueryResultState(ClientContextLock &lock)> &call) {
	if (!handle->context) {
		// The stream already ended. Keep reporting the terminal state
		return handle->HasError() ? QueryResultState::EXECUTION_ERROR : QueryResultState::FINISHED;
	}
	// Ending the query drops the handle's reference to the context, which the lock below outlives
	auto keep_alive = handle->context;
	auto lock = keep_alive->LockContext();
	try {
		return call(*lock);
	} catch (std::exception &ex) {
		// A pending interrupt reaches the consumer as an error on the stream, never as a throw
		handle->HandleFetchFailure(*lock, ErrorData(ex));
	} catch (...) { // LCOV_EXCL_START
		handle->SetError(ErrorData(StringUtil::Format("Unhandled exception in %s", name)));
		handle->EndQuery(*lock, true);
	} // LCOV_EXCL_STOP
	return QueryResultState::EXECUTION_ERROR;
}

QueryResultState ResultStreamBase::DriveWithoutBufferInternal(ClientContextLock &lock, bool execute_task) {
	D_ASSERT(!handle->buffer);
	if (!handle->IsOpenInternal(lock)) {
		return handle->Cancelled();
	}
	auto &context = *handle->context;
	// Checked as the buffer's own Poll checks it, so a cancel is seen before a finished execution can be ended. A
	// worker error also raises the flag; both loads are seq_cst
	if (context.interrupt_state.load() == ClientInterruptState::INTERRUPTED && !Executor::Get(context).HasError()) {
		throw InterruptException();
	}
	return execute_task ? context.ExecuteTaskInternal(lock, *handle) : context.PollInternal(lock, *handle);
}

QueryResultState ResultStreamBase::Poll() {
	auto state = GuardedInternal("Poll", [&](ClientContextLock &lock) {
		if (!handle->buffer) {
			auto state = DriveWithoutBufferInternal(lock, false);
			if (!handle->buffer) {
				return state;
			}
		}
		return handle->buffer->Poll(lock, *handle);
	});
	if (state == QueryResultState::EXECUTION_ERROR) {
		Close();
	}
	return state;
}

QueryResultState ResultStreamBase::ExecuteTask() {
	auto state = GuardedInternal("ExecuteTask", [&](ClientContextLock &lock) {
		if (!handle->buffer) {
			auto state = DriveWithoutBufferInternal(lock, true);
			// When the statement producing the result started, the buffer reports what is poppable
			return handle->buffer ? handle->buffer->Poll(lock, *handle) : state;
		}
		return handle->buffer->Participate(lock, *handle);
	});
	if (state == QueryResultState::EXECUTION_ERROR) {
		// A finished execution can still hold trailing units, so only an error ends the stream here
		Close();
	}
	return state;
}

void ResultStreamBase::WaitForTask() {
	if (!handle->context) {
		return;
	}
	if (handle->buffer) {
		handle->buffer->UnblockSinks();
	}
	handle->WaitForTask();
}

QueryResultState ResultStreamBase::TryFetchUnit(unique_ptr<ResultUnit> &out_unit) {
	out_unit.reset();
	auto state = GuardedInternal("TryFetch", [&](ClientContextLock &lock) {
		if (!handle->buffer) {
			auto state = DriveWithoutBufferInternal(lock, false);
			if (!handle->buffer) {
				if (state == QueryResultState::FINISHED) {
					// A statement that expands into several and returns no rows ends without a unit
					return handle->EndFinishedInternal(lock);
				}
				return state;
			}
		}
		auto &buffer = *handle->buffer;
		auto state = buffer.Poll(lock, *handle);
		if (state == QueryResultState::EXECUTION_ERROR) {
			return state;
		}
		if (state == QueryResultState::READY) {
			out_unit = buffer.Scan();
			// READY is only reported with a unit queued
			D_ASSERT(out_unit);
			return QueryResultState::READY;
		}
		if (state == QueryResultState::FINISHED) {
			// The buffer is drained and execution is done: this is the end of the stream
			buffer.AssertNoBlockedSinks();
			handle->EndQuery(lock);
			// Cleanup can fail on an autocommit commit. It records the error without throwing
			return handle->HasError() ? QueryResultState::EXECUTION_ERROR : QueryResultState::FINISHED;
		}
		return state;
	});
	if (IsTerminal(state)) {
		Close();
	}
	return state;
}

unique_ptr<ResultUnit> ResultStreamBase::FetchUnitInternal(ClientContextLock &lock) {
	try {
		// The statements before the one producing the result of a statement that expands run on this thread
		while (!handle->buffer) {
			auto state = handle->context->ExecuteTaskInternal(lock, *handle);
			if (state == QueryResultState::EXECUTION_ERROR) {
				return nullptr;
			}
			if (state == QueryResultState::FINISHED && !handle->buffer) {
				handle->EndFinishedInternal(lock);
				return nullptr;
			}
			if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
				handle->context->WaitForTask(lock, *handle);
			}
		}
		auto &buffer = *handle->buffer;
		auto state = buffer.ReplenishBuffer(lock, *handle);
		if (state == QueryResultState::EXECUTION_ERROR) {
			return nullptr;
		}
		auto unit = buffer.Scan();
		if (!unit) {
			// READY is only reported with a unit queued, so an empty scan means execution finished
			D_ASSERT(state == QueryResultState::FINISHED);
			handle->EndQuery(lock);
			return nullptr;
		}
		return unit;
	} catch (std::exception &ex) {
		handle->HandleFetchFailure(lock, ErrorData(ex));
	} catch (...) { // LCOV_EXCL_START
		handle->SetError(ErrorData("Unhandled exception in Fetch"));
		handle->EndQuery(lock, true);
	} // LCOV_EXCL_STOP
	return nullptr;
}

unique_ptr<ResultUnit> ResultStreamBase::FetchUnit() {
	if (!handle->context && !handle->HasError()) {
		// The stream ended cleanly. Keep reporting the end, the way TryFetch and Poll do
		return nullptr;
	}
	unique_ptr<ResultUnit> unit;
	{
		// Ending the query drops the handle's reference to the context, which the lock below outlives
		auto keep_alive = handle->context;
		auto lock = handle->LockContext();
		handle->CheckExecutableInternal(*lock);
		unit = FetchUnitInternal(*lock);
	}
	if (!unit) {
		if (!HasError() && handle->buffer) {
			handle->buffer->AssertNoBlockedSinks();
		}
		Close();
		return nullptr;
	}
	return unit;
}

const ResultFormatGlobalState &ResultStreamBase::FormatStateInternal() const {
	if (!handle->buffer) {
		return handle->CheckedFormatState(handle->Format().Name());
	}
	return handle->GetBufferedData().FormatState();
}

void ResultStreamBase::SetError(ErrorData error) {
	handle->SetError(std::move(error));
}

bool ResultStreamBase::HasError() const {
	return handle->HasError();
}

const string &ResultStreamBase::GetError() const {
	return handle->GetError();
}

const ErrorData &ResultStreamBase::GetErrorObject() const {
	return handle->GetErrorObject();
}

const ExceptionType &ResultStreamBase::GetErrorType() const {
	return handle->GetErrorType();
}

const vector<LogicalType> &ResultStreamBase::GetTypes() const {
	return handle->GetTypes();
}

const vector<Identifier> &ResultStreamBase::GetNames() const {
	return handle->GetNames();
}

const Identifier &ResultStreamBase::ColumnName(idx_t index) const {
	return handle->ColumnName(index);
}

idx_t ResultStreamBase::ColumnCount() const {
	return handle->ColumnCount();
}

StatementType ResultStreamBase::GetStatementType() const {
	return handle->GetStatementType();
}

const StatementProperties &ResultStreamBase::GetStatementProperties() const {
	return handle->GetStatementProperties();
}

const ClientProperties &ResultStreamBase::GetClientProperties() const {
	return handle->client_properties;
}

ClientProperties &ResultStreamBase::GetClientProperties() {
	return handle->client_properties;
}

} // namespace duckdb
