//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi_v2/capi_v2_result_internal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/result_format.hpp"

//! The result state machine, shared by the modules that build a result or consume one wholesale.
//! Not part of any public surface: only the V2 bridge's own translation units include this.
//! `capi_v2_result.cpp` owns it and defines the members declared here; `capi_v2_prepared_statement.cpp`
//! and `capi_v2_arrow.cpp` are the other consumers.

namespace duckdb::capiv2 {

struct ResultWrapperV2 {
	enum class State : uint8_t { PENDING, STREAMING, FINISHED, CANCELLED, ERRORED };

	~ResultWrapperV2() {
		// Finalize() (engine cleanup) runs in duckdb_v2_result_destroy, not here:
		// a destructor must not drive locked engine state behind a catch-all.
		stream.reset();
		handle.reset();
	}

	State state = State::PENDING;
	//! The handle of the running query. Live while state == PENDING, and afterwards for a result
	//! that is retained rather than streamed.
	unique_ptr<QueryResult> handle;
	//! Live while state == STREAMING, for a statement whose result can be streamed. A QueryResultStream in `format`.
	unique_ptr<ResultStreamBase> stream;
	//! Decided when the handle is adopted, since a prepared execution's handle reports the EXECUTE that runs it
	//! rather than the statement as written
	bool stream_when_ready = false;
	//! The statement is submitted in this format; null is the chunk format.
	shared_ptr<ResultFormat> format;

	//! Preserves the guarantee that an undrained result survives disconnect:
	//! the connection handle (a bare Connection *) may be destroyed while a
	//! result is live, but the context it shared with us stays alive here.
	shared_ptr<ClientContext> context;
	//! True once the metadata has been captured. For a statement that expands into several, that is once execution
	//! bound the statement that produces the result, or the group ended without one.
	bool metadata_available = false;

	//! The connection's busy slot, kept for the result's lifetime for its cancel count.
	shared_ptr<ConnectionBusySlotV2> busy_slot;
	//! The slot's cancel count when this result's statement started
	idx_t cancel_requests_at_start = 0;

	//! The result's metadata, valid once metadata_available. The statement type is known at submission.
	vector<LogicalType> types;
	vector<Identifier> names;
	StatementType statement_type = StatementType::INVALID_STATEMENT;
	StatementProperties properties;
	//! Set with the metadata when `format` is Arrow: a copy of the format's schema, which is gone once the query ends.
	ArrowSchemaWrapper arrow_schema;

	//! Sticky error, recorded when state == ERRORED.
	ErrorData error;

	//! Close() the live engine result so an abandoned active query is cleaned
	//! up (freeing the executor, which breaks the ClientContext ref cycle). The
	//! engine undoes a statement that expands into several as a whole. May throw.
	void Finalize() {
		if (stream) {
			stream->Close();
		} else if (handle) {
			handle->Close();
		}
	}

	// State-machine entry points; defined in capi_v2_result.cpp. All of
	// them throw DuckDB exceptions on failure (callers wrap in
	// WithErrorHandler) and record sticky errors before throwing.

	//! Adopts an already-submitted query into the state machine: the single seam both the
	//! stateless and prepared paths reach. written_type is the type of the statement as written, since a prepared
	//! execution's handle reports the EXECUTE that runs it. Throws on a submission error.
	void BeginPending(unique_ptr<QueryResult> handle, StatementType written_type);
	//! Drives one unit of work; never blocks. On CHUNK, out_unit holds the
	//! produced unit; on every other status it is reset. FORMAT must be `format`.
	template <class FORMAT>
	DUCKDB_V2_RESULT_STEP_STATUS Step(unique_ptr<typename FORMAT::T> &out_unit);
	//! Blocks until Step can make progress. No-op on terminal states.
	void Wait();
	//! Blocking convenience: steps/waits until a unit is produced (returned)
	//! or the stream ends (nullptr). Cancellation throws InterruptException.
	template <class FORMAT>
	unique_ptr<typename FORMAT::T> FetchBlocking();
	//! Steps a statement that expands into several until its metadata is available. Stops early when the result
	//! ends without it.
	template <class FORMAT>
	void AdvanceToMetadata();
	//! Runs to the end. Returns the changed-row count of a CHANGED_ROWS result, 0 otherwise.
	template <class FORMAT>
	idx_t Drain();
	//! Throws MetadataDeferredException unless the metadata is available.
	void RequireMetadata();

private:
	//! Copies the metadata off the live handle or stream once the engine knows it, since both go when the result ends
	void CaptureMetadata();
	//! Shared error sink: interrupts become the sticky CANCELLED state
	//! (returned as a status), everything else becomes the sticky ERRORED
	//! state and throws.
	DUCKDB_V2_RESULT_STEP_STATUS HandleExecutionError(ErrorData error_data);
};

auto Convert(ResultWrapperV2 *wrapper) -> duckdb_v2_result_handle;
auto Convert(duckdb_v2_result_handle handle) -> ResultWrapperV2 *;
auto ConvertArrowResult(ResultWrapperV2 *wrapper) -> duckdb_v2_arrow_result_handle;
auto Convert(duckdb_v2_arrow_result_handle handle) -> ResultWrapperV2 *;

//! Submits a borrowed statement in `format`. Throws ResourceInUseException while a statement on the connection is
//! still running.
auto ExecuteStatementV2(const shared_ptr<ClientContext> &context, const SQLStatement &statement,
                        const duckdb_v2_identifier_t *parameter_names, const duckdb_v2_value_handle *parameter_values,
                        idx_t parameter_count, const char *function_name, shared_ptr<ResultFormat> format)
    -> unique_ptr<ResultWrapperV2>;
//! Runs a prepared statement. `context` is the session the result holds on to, so it
//! survives disconnect. Throws ResourceInUseException while a statement on the connection is still running.
auto ExecutePreparedStatementV2(const shared_ptr<ClientContext> &context, PreparedStatement &prepared,
                                identifier_map_t<BoundParameterData> &values, shared_ptr<ResultFormat> format)
    -> unique_ptr<ResultWrapperV2>;
//! A deep copy the caller owns. `out` is left released on failure.
void CopyArrowSchema(const ArrowSchema &source, ArrowSchema &out);
//! The format of an Arrow result; a batch size of 0 selects the default.
auto ArrowResultFormat(idx_t batch_size) -> shared_ptr<ResultFormat>;

} // namespace duckdb::capiv2
