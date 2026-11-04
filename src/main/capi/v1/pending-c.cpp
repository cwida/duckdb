#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/optional_ptr.hpp"

using duckdb::case_insensitive_map_t;
using duckdb::IsObservable;
using duckdb::make_uniq;
using duckdb::optional_ptr;
using duckdb::PendingStatementWrapper;
using duckdb::PreparedStatementWrapper;
using duckdb::QueryResult;
using duckdb::QueryResultState;
using duckdb::Value;

duckdb_state duckdb_pending_prepared_internal(duckdb_prepared_statement prepared_statement,
                                              duckdb_pending_result *out_result, bool allow_streaming) {
	if (!prepared_statement || !out_result) {
		return DuckDBError;
	}
	auto wrapper = reinterpret_cast<PreparedStatementWrapper *>(prepared_statement);
	auto result = new PendingStatementWrapper();
	result->allow_streaming = false;

	try {
		result->statement = wrapper->statement->Submit(wrapper->values);
		result->allow_streaming =
		    allow_streaming && duckdb::StreamsByDefault(wrapper->statement->GetStatementType(), *result->statement);
		if (!result->allow_streaming) {
			// Settled before the caller drives it, so each execute_task call stays one unit of work
			result->statement->Materialize();
		}
	} catch (std::exception &ex) {
		result->statement = make_uniq<QueryResult>(duckdb::ErrorData(ex));
	}
	duckdb_state return_value = !result->statement->HasError() ? DuckDBSuccess : DuckDBError;
	*out_result = reinterpret_cast<duckdb_pending_result>(result);

	return return_value;
}

duckdb_state duckdb_pending_prepared(duckdb_prepared_statement prepared_statement, duckdb_pending_result *out_result) {
	return duckdb_pending_prepared_internal(prepared_statement, out_result, false);
}

duckdb_state duckdb_pending_prepared_streaming(duckdb_prepared_statement prepared_statement,
                                               duckdb_pending_result *out_result) {
	return duckdb_pending_prepared_internal(prepared_statement, out_result, true);
}

void duckdb_destroy_pending(duckdb_pending_result *pending_result) {
	if (!pending_result || !*pending_result) {
		return;
	}
	auto wrapper = reinterpret_cast<PendingStatementWrapper *>(*pending_result);
	if (wrapper->statement) {
		wrapper->statement->Close();
	}
	delete wrapper;
	*pending_result = nullptr;
}

const char *duckdb_pending_error(duckdb_pending_result pending_result) {
	if (!pending_result) {
		return nullptr;
	}
	auto wrapper = reinterpret_cast<PendingStatementWrapper *>(pending_result);
	if (!wrapper->statement) {
		return nullptr;
	}
	return wrapper->statement->GetError().c_str();
}

duckdb_pending_state duckdb_pending_execute_check_state(duckdb_pending_result pending_result) {
	if (!pending_result) {
		return DUCKDB_PENDING_ERROR;
	}
	auto wrapper = reinterpret_cast<PendingStatementWrapper *>(pending_result);
	if (!wrapper->statement) {
		return DUCKDB_PENDING_ERROR;
	}
	if (wrapper->statement->HasError()) {
		return DUCKDB_PENDING_ERROR;
	}
	QueryResultState return_value;
	try {
		return_value = wrapper->statement->Poll();
	} catch (std::exception &ex) {
		wrapper->statement->SetError(duckdb::ErrorData(ex));
		return DUCKDB_PENDING_ERROR;
	}
	switch (return_value) {
	case QueryResultState::BLOCKED:
	case QueryResultState::READY:
	case QueryResultState::FINISHED:
		return DUCKDB_PENDING_RESULT_READY;
	case QueryResultState::NO_TASKS_AVAILABLE:
		return DUCKDB_PENDING_NO_TASKS_AVAILABLE;
	case QueryResultState::NOT_READY:
		return DUCKDB_PENDING_RESULT_NOT_READY;
	default:
		return DUCKDB_PENDING_ERROR;
	}
}

duckdb_pending_state duckdb_pending_execute_task(duckdb_pending_result pending_result) {
	if (!pending_result) {
		return DUCKDB_PENDING_ERROR;
	}
	auto wrapper = reinterpret_cast<PendingStatementWrapper *>(pending_result);
	if (!wrapper->statement) {
		return DUCKDB_PENDING_ERROR;
	}
	if (wrapper->statement->HasError()) {
		return DUCKDB_PENDING_ERROR;
	}
	QueryResultState return_value;
	try {
		return_value = wrapper->statement->ExecuteTask();
	} catch (std::exception &ex) {
		wrapper->statement->SetError(duckdb::ErrorData(ex));
		return DUCKDB_PENDING_ERROR;
	}
	switch (return_value) {
	case QueryResultState::FINISHED:
	case QueryResultState::READY:
		return DUCKDB_PENDING_RESULT_READY;
	case QueryResultState::BLOCKED:
	case QueryResultState::NO_TASKS_AVAILABLE:
		return DUCKDB_PENDING_NO_TASKS_AVAILABLE;
	case QueryResultState::NOT_READY:
		return DUCKDB_PENDING_RESULT_NOT_READY;
	default:
		return DUCKDB_PENDING_ERROR;
	}
}

bool duckdb_pending_execution_is_finished(duckdb_pending_state pending_state) {
	switch (pending_state) {
	case DUCKDB_PENDING_RESULT_READY:
		return IsObservable(QueryResultState::READY);
	case DUCKDB_PENDING_NO_TASKS_AVAILABLE:
		return IsObservable(QueryResultState::NO_TASKS_AVAILABLE);
	case DUCKDB_PENDING_RESULT_NOT_READY:
		return IsObservable(QueryResultState::NOT_READY);
	case DUCKDB_PENDING_ERROR:
		return IsObservable(QueryResultState::EXECUTION_ERROR);
	default:
		return IsObservable(QueryResultState::EXECUTION_ERROR);
	}
}

duckdb_state duckdb_execute_pending(duckdb_pending_result pending_result, duckdb_result *out_result) {
	if (!pending_result || !out_result) {
		return DuckDBError;
	}
	memset(out_result, 0, sizeof(duckdb_result));
	auto wrapper = reinterpret_cast<PendingStatementWrapper *>(pending_result);
	if (!wrapper->statement) {
		return DuckDBError;
	}

	auto result = std::move(wrapper->statement);
	const bool stream_result = wrapper->allow_streaming && !result->HasError();
	try {
		if (stream_result) {
			return DuckDBTranslateStreamResult(duckdb::make_uniq<duckdb::QueryResultStream<>>(std::move(result)),
			                                   out_result);
		}
		result->Complete();
	} catch (std::exception &ex) {
		duckdb::ErrorData error(ex);
		result = duckdb::make_uniq<QueryResult>(std::move(error));
	}
	return DuckDBTranslateResult(std::move(result), out_result);
}
