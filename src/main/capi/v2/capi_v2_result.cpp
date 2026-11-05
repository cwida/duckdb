#include "duckdb/main/capi_v2/capi_v2_result_internal.hpp"

#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/common/arrow/nanoarrow/nanoarrow.hpp"
#include "duckdb/common/box_renderer.hpp"
#include "duckdb/common/box_renderer_context.hpp"
#include "duckdb/common/column_data_collection_render_interface.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

#include "duckdb/common/enums/query_result_state.hpp"

namespace duckdb::capiv2 {
namespace {

// Map duckdb::StatementReturnType to DUCKDB_V2_RESULT_TYPE. Values are
// numerically identical by §4 of the V2 conventions (numeric enum-id
// round-trip); the switch is the explicit mapping that gives -Wswitch
// teeth and asserts in debug if core adds a variant the V2 enum does
// not yet surface.
DUCKDB_V2_RESULT_TYPE MapResultType(StatementReturnType t) {
	switch (t) {
	case StatementReturnType::QUERY_RESULT:
		return DUCKDB_V2_RESULT_TYPE_QUERY_RESULT;
	case StatementReturnType::CHANGED_ROWS:
		return DUCKDB_V2_RESULT_TYPE_CHANGED_ROWS;
	case StatementReturnType::NOTHING:
		return DUCKDB_V2_RESULT_TYPE_NOTHING;
	}
	D_ASSERT(false); // unmapped StatementReturnType variant
	return DUCKDB_V2_RESULT_TYPE_QUERY_RESULT;
}

//! The count is the first row of the single BIGINT column.
void ReadChangedRows(const DataChunk &chunk, idx_t &rows_changed) {
	if (chunk.size() > 0) {
		rows_changed = static_cast<idx_t>(chunk.GetValue(0, 0).GetValue<int64_t>());
	}
}

void ReadChangedRows(const ArrowArrayWrapper &unit, idx_t &rows_changed) {
	if (unit.arrow_array.length > 0) {
		rows_changed = ArrowFormat::ChangedRows(unit.arrow_array);
	}
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ResultWrapperV2 state machine
// ---------------------------------------------------------------------------

void ResultWrapperV2::BeginPending(unique_ptr<QueryResult> next_handle, StatementType written_type) {
	next_handle->ThrowIfError();
	stream_when_ready = StreamsByDefault(written_type, *next_handle);
	if (!stream_when_ready) {
		// Settled before the caller steps it, so every step stays one bounded unit of work
		next_handle->Materialize();
	}
	statement_type = next_handle->GetStatementType();
	handle = std::move(next_handle);
	state = State::PENDING;
	CaptureMetadata();
}

void ResultWrapperV2::CaptureMetadata() {
	if (metadata_available) {
		return;
	}
	const bool arrow = format && format->Is<ArrowFormat>();
	if (stream) {
		// A failed result reports empty metadata, which is not the result's
		if (stream->HasError() || !stream->MetadataAvailable()) {
			return;
		}
		types = stream->GetTypes();
		names = stream->GetNames();
		properties = stream->GetStatementProperties();
		if (arrow) {
			auto &arrow_stream = static_cast<QueryResultStream<ArrowFormat> &>(*stream);
			CopyArrowSchema(arrow_stream.FormatState().Schema(), arrow_schema.arrow_schema);
		}
	} else if (handle) {
		if (handle->HasError() || !handle->MetadataAvailable()) {
			return;
		}
		types = handle->GetTypes();
		names = handle->GetNames();
		properties = handle->GetStatementProperties();
		if (arrow) {
			CopyArrowSchema(handle->FormatState<ArrowFormat>().Schema(), arrow_schema.arrow_schema);
		}
	} else {
		return;
	}
	metadata_available = true;
}

void ResultWrapperV2::RequireMetadata() {
	CaptureMetadata();
	if (metadata_available) {
		return;
	}
	// A result that ended before its metadata was known never gets it
	if (state == State::ERRORED) {
		error.Throw();
	}
	if (state == State::CANCELLED) {
		throw InterruptException();
	}
	throw MetadataDeferredException("result metadata is not yet available: the statement expands into several, and "
	                                "the one that produces the result has not been bound; step the result");
}

DUCKDB_V2_RESULT_STEP_STATUS ResultWrapperV2::HandleExecutionError(ErrorData error_data) {
	// Only a consumer-initiated cancellation is a cancellation. An
	// engine-initiated interrupt that shares the INTERRUPT exception type
	// (e.g. a max_execution_time timeout) must surface as an error carrying the
	// engine's message, mirroring the eager ClientContext::Query path.
	bool user_cancelled = error_data.Type() == ExceptionType::INTERRUPT && busy_slot &&
	                      busy_slot->cancel_requests.load() != cancel_requests_at_start;
	stream.reset();
	handle.reset();
	if (user_cancelled) {
		// In the step channel, cancellation is a status, not an error,
		// regardless of which phase the interrupt landed in. The status
		// channel carries no message, so the engine's error text is
		// deliberately dropped here.
		state = State::CANCELLED;
		return DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED;
	}
	state = State::ERRORED;
	error = std::move(error_data);
	error.Throw();
}

template <class FORMAT>
DUCKDB_V2_RESULT_STEP_STATUS ResultWrapperV2::Step(unique_ptr<typename FORMAT::T> &out_unit) {
	D_ASSERT((format ? format->Is<FORMAT>() : std::is_same<FORMAT, ChunkFormat>::value));
	out_unit.reset();
	switch (state) {
	case State::FINISHED:
		return DUCKDB_V2_RESULT_STEP_STATUS_FINISHED;
	case State::CANCELLED:
		return DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED;
	case State::ERRORED:
		// Sticky: rethrow the recorded error without touching the (closed)
		// internal results.
		error.Throw();
	case State::PENDING: {
		QueryResultState exec;
		try {
			exec = handle->ExecuteTask();
			CaptureMetadata();
		} catch (std::exception &ex) {
			return HandleExecutionError(ErrorData(ex));
		}
		switch (exec) {
		case QueryResultState::NOT_READY:
		case QueryResultState::BLOCKED:
		case QueryResultState::NO_TASKS_AVAILABLE:
			return DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		case QueryResultState::EXECUTION_ERROR:
			return HandleExecutionError(handle->GetErrorObject());
		case QueryResultState::READY:
		case QueryResultState::FINISHED: {
			// Transition to the streaming phase. Reporting WAITING after the
			// transition keeps the contract simple: one unit of work per
			// step; the next step hits the stream.
			try {
				if (!stream_when_ready) {
					handle->Complete();
					if (handle->HasError()) {
						return HandleExecutionError(handle->GetErrorObject());
					}
				} else {
					stream = make_uniq<QueryResultStream<FORMAT>>(std::move(handle));
				}
				CaptureMetadata();
			} catch (std::exception &ex) {
				return HandleExecutionError(ErrorData(ex));
			}
			state = State::STREAMING;
			return DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		}
		}
		D_ASSERT(false); // unmapped QueryResultState variant
		return DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	}
	case State::STREAMING: {
		if (stream) {
			QueryResultState exec;
			try {
				exec = stream->ExecuteTask();
				CaptureMetadata();
			} catch (std::exception &ex) {
				// The stream records an interrupt as an error rather than
				// throwing; this is a guard for anything that escapes.
				return HandleExecutionError(ErrorData(ex));
			}
			switch (exec) {
			case QueryResultState::NOT_READY:
			case QueryResultState::BLOCKED:
			case QueryResultState::NO_TASKS_AVAILABLE:
				return DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
			case QueryResultState::EXECUTION_ERROR:
				return HandleExecutionError(stream->GetErrorObject());
			case QueryResultState::READY:
			case QueryResultState::FINISHED:
				// Both states mean "Fetch without doing more execution
				// work": READY has a buffered chunk; after FINISHED the
				// buffer may still hold trailing chunks and Fetch drains
				// them until it reports end-of-stream.
				break;
			}
		}
		// A retained statement has all its data, so every step lands here directly
		unique_ptr<typename FORMAT::T> unit;
		try {
			unit =
			    stream ? static_cast<QueryResultStream<FORMAT> &>(*stream).Fetch() : handle->template Fetch<FORMAT>();
		} catch (std::exception &ex) {
			return HandleExecutionError(ErrorData(ex));
		}
		if (stream ? stream->HasError() : handle->HasError()) {
			// The stream reports late execution errors by setting the error
			// and returning null.
			return HandleExecutionError(stream ? stream->GetErrorObject() : handle->GetErrorObject());
		}
		try {
			CaptureMetadata();
		} catch (std::exception &ex) {
			return HandleExecutionError(ErrorData(ex));
		}
		if (!unit) {
			stream.reset();
			handle.reset();
			state = State::FINISHED;
			return DUCKDB_V2_RESULT_STEP_STATUS_FINISHED;
		}
		out_unit = std::move(unit);
		return DUCKDB_V2_RESULT_STEP_STATUS_CHUNK;
	}
	}
	D_ASSERT(false); // unreachable: all states handled above
	return DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
}

void ResultWrapperV2::Wait() {
	// Unlike ExecuteTask, the engine's WaitForTask performs no
	// executability check and dereferences the context's active query
	// unguarded; an unchecked wait on an invalidated result is UB. Verify
	// executability first, the same way the steps do.
	switch (state) {
	case State::PENDING: {
		// Poll re-validates executability and reports the engine state
		// without running work. Its outcome decides whether blocking is
		// safe: the engine may already have processed an execution error and
		// ended the query internally (waiting then dereferences the closed
		// query), and a ready or finished query has no task to wake on.
		auto polled = handle->Poll();
		CaptureMetadata();
		switch (polled) {
		case QueryResultState::BLOCKED:
		case QueryResultState::NO_TASKS_AVAILABLE:
			// No progress possible right now; this is the one case where
			// blocking is meaningful.
			handle->WaitForTask();
			return;
		case QueryResultState::EXECUTION_ERROR:
			// The error (interrupts included) is already recorded on the
			// handle and the engine closed the query. Transition the state
			// machine now: a later ExecuteTask would trip the engine's
			// closed-result check and misreport the error as INVALID_INPUT.
			HandleExecutionError(handle->GetErrorObject());
			return;
		default:
			// NOT_READY / READY / FINISHED: the next step makes progress
			// without blocking.
			return;
		}
	}
	case State::STREAMING:
		if (stream) {
			if (!stream->IsOpen()) {
				// Closed or invalidated: the next step returns without
				// blocking, so there is nothing to wait for.
				return;
			}
			stream->WaitForTask();
		}
		// A retained result makes progress on every step; nothing to wait for.
		return;
	default:
		// Terminal states: waiting is a no-op, never an error.
		return;
	}
}

template <class FORMAT>
unique_ptr<typename FORMAT::T> ResultWrapperV2::FetchBlocking() {
	while (true) {
		unique_ptr<typename FORMAT::T> unit;
		switch (Step<FORMAT>(unit)) {
		case DUCKDB_V2_RESULT_STEP_STATUS_CHUNK:
			return unit;
		case DUCKDB_V2_RESULT_STEP_STATUS_FINISHED:
			return nullptr;
		case DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED:
			// fetch_chunk has no status channel, so cancellation surfaces
			// as ERROR_RUNTIME_INTERRUPT. The state stays CANCELLED (not
			// ERRORED), so steps keep reporting the status. Throwing a
			// fresh InterruptException means the error text is the generic
			// "Interrupted!", not the engine's message; deliberate, since
			// the CANCELLED state carries no message either.
			throw InterruptException();
		case DUCKDB_V2_RESULT_STEP_STATUS_WAITING:
			Wait();
			break;
		default:
			break;
		}
	}
}

template <class FORMAT>
void ResultWrapperV2::AdvanceToMetadata() {
	while (!metadata_available) {
		unique_ptr<typename FORMAT::T> discard;
		auto status = Step<FORMAT>(discard);
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			Wait();
			continue;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK) {
			throw InternalException("a row was produced before result metadata was available");
		}
		break; // FINISHED / CANCELLED
	}
}

template <class FORMAT>
idx_t ResultWrapperV2::Drain() {
	// CHANGED_ROWS results stream a single-row BIGINT Count unit. The metadata of a statement that expands into several
	// may only become available mid-drain, so check per unit.
	idx_t rows_changed = 0;
	while (auto unit = FetchBlocking<FORMAT>()) {
		if (metadata_available && properties.return_type == StatementReturnType::CHANGED_ROWS) {
			ReadChangedRows(*unit, rows_changed);
		}
	}
	return rows_changed;
}

// Put def's of the arrow specializations here so they are instantiated
template DUCKDB_V2_RESULT_STEP_STATUS ResultWrapperV2::Step<ArrowFormat>(unique_ptr<ArrowArrayWrapper> &);
template unique_ptr<ArrowArrayWrapper> ResultWrapperV2::FetchBlocking<ArrowFormat>();
template void ResultWrapperV2::AdvanceToMetadata<ArrowFormat>();
template idx_t ResultWrapperV2::Drain<ArrowFormat>();

void CopyArrowSchema(const ArrowSchema &source, ArrowSchema &out) {
	out.release = nullptr;
	if (duckdb_nanoarrow::ArrowSchemaDeepCopy(&source, &out) != NANOARROW_OK) {
		throw OutOfMemoryException("failed to copy the Arrow schema");
	}
}

auto Convert(ResultWrapperV2 *wrapper) -> duckdb_v2_result_handle {
	return reinterpret_cast<duckdb_v2_result_handle>(wrapper);
}
auto Convert(duckdb_v2_result_handle handle) -> ResultWrapperV2 * {
	return reinterpret_cast<ResultWrapperV2 *>(handle);
}
auto ConvertArrowResult(ResultWrapperV2 *wrapper) -> duckdb_v2_arrow_result_handle {
	return reinterpret_cast<duckdb_v2_arrow_result_handle>(wrapper);
}
auto Convert(duckdb_v2_arrow_result_handle handle) -> ResultWrapperV2 * {
	return reinterpret_cast<ResultWrapperV2 *>(handle);
}

auto ExecutePreparedStatementV2(const shared_ptr<ClientContext> &context, PreparedStatement &prepared,
                                identifier_map_t<BoundParameterData> &values, shared_ptr<ResultFormat> format)
    -> unique_ptr<ResultWrapperV2> {
	auto wrapper = make_uniq<ResultWrapperV2>();
	wrapper->busy_slot = GetBusySlot(*context);
	wrapper->cancel_requests_at_start = wrapper->busy_slot->cancel_requests.load();
	wrapper->context = context;
	wrapper->format = std::move(format);
	// The engine refuses the submission while another statement on the connection is running
	wrapper->BeginPending(prepared.Submit(values, QueryParameters(wrapper->format)), prepared.GetStatementType());
	// The engine runs a prepared statement through an internal EXECUTE, whose statement type
	// would otherwise be what the result reports. Report the type of the statement that was
	// prepared instead, so a prepared result is indistinguishable from a stateless one.
	wrapper->statement_type = prepared.GetStatementType();
	return wrapper;
}

auto ExecuteStatementV2(const shared_ptr<ClientContext> &context, const SQLStatement &statement,
                        const duckdb_v2_identifier_t *parameter_names, const duckdb_v2_value_handle *parameter_values,
                        idx_t parameter_count, const char *function_name, shared_ptr<ResultFormat> format)
    -> unique_ptr<ResultWrapperV2> {
	auto wrapper = make_uniq<ResultWrapperV2>();
	// The busy slot lives in the context's registered-state map, so the connection handle stays a bare Connection *
	wrapper->busy_slot = GetBusySlot(*context);
	wrapper->cancel_requests_at_start = wrapper->busy_slot->cancel_requests.load();

	// Borrowed, not consumed: execute a copy so the caller keeps the original.
	auto stmt = statement.Copy();
	// Fold parameter values in as constants, keyed by name when parameter_names
	// supplies one and positionally ("1".."N") otherwise.
	identifier_map_t<BoundParameterData> values;
	BuildParameterMap(parameter_names, parameter_values, parameter_count, function_name, values);
	wrapper->context = context;
	wrapper->format = std::move(format);
	// Lazy streaming execution: nothing executes until the result is stepped. The engine expands the statement,
	// running what it expands into as one query, and refuses the submission while another statement on the
	// connection is running.
	auto handle = values.empty() ? context->Submit(std::move(stmt), QueryParameters(wrapper->format))
	                             : context->Submit(std::move(stmt), values, QueryParameters(wrapper->format));
	// The result's type, so that a PRAGMA the engine reparses into a SELECT streams as that SELECT
	const auto result_type = handle->GetStatementType();
	wrapper->BeginPending(std::move(handle), result_type);
	return wrapper;
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_statement_execute(duckdb_v2_connection_handle conn, duckdb_v2_sql_statement_handle statement,
                                            const duckdb_v2_identifier_t *parameter_names,
                                            const duckdb_v2_value_handle *parameter_values, idx_t parameter_count,
                                            duckdb_v2_result_handle *out_result, duckdb_v2_error_info_handle *err) {
	// The refusals here never reach the engine and leave the statement intact
	// (the spec commits to this).
	DUCKDB_CHECK_ARG(out_result);
	*out_result = nullptr;
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(statement);
	if (parameter_count > 0 && !parameter_values) {
		return NullArgumentError(err, __func__, "parameter_values");
	}
	return WithErrorHandler(err, [&]() {
		*out_result = Convert(ExecuteStatementV2(Convert(conn)->context, *Convert(statement), parameter_names,
		                                         parameter_values, parameter_count, __func__, nullptr)
		                          .release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_result_drain(duckdb_v2_result_handle result, idx_t *out_rows_changed,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(out_rows_changed);
	return WithErrorHandler(err, [&]() { *out_rows_changed = Convert(result)->Drain<duckdb::ChunkFormat>(); });
}

DUCKDB_V2_ERROR duckdb_v2_result_render_box(duckdb_v2_result_handle *result, idx_t max_rows, idx_t max_width,
                                            idx_t max_col_width, const duckdb_v2_str *null_value, idx_t render_mode,
                                            idx_t limit, duckdb_v2_text_sink_fn sink, void *user_data,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(*result);
	DUCKDB_CHECK_ARG(sink);
	// Validate the view arguments before consuming the result, so an
	// argument rejection leaves the caller's result intact (as the null-arg
	// rejections above do).
	DUCKDB_CHECK_ARG(null_value);
	return WithErrorHandler(err, [&]() {
		if (render_mode > 1) {
			throw duckdb::InvalidInputException("render_mode must be 0 (rows) or 1 (columns)");
		}
		// Adopt by transfer; consumed on success and failure alike.
		auto wrapper = duckdb::unique_ptr<ResultWrapperV2>(Convert(*result));
		*result = nullptr;
		// Names and types must be available before building the collection.
		wrapper->AdvanceToMetadata<duckdb::ChunkFormat>();
		if (!wrapper->context) {
			throw duckdb::InvalidInputException("result is not associated with an active context");
		}
		auto &ctx = *wrapper->context;

		// Materialize the remainder; max_rows bounds display, not the read.
		duckdb::ColumnDataCollection collection(duckdb::Allocator::DefaultAllocator(), wrapper->types);
		while (auto chunk = wrapper->FetchBlocking<duckdb::ChunkFormat>()) {
			collection.Append(*chunk);
		}

		duckdb::BoxRendererConfig config;
		if (max_rows != 0) {
			config.max_rows = max_rows;
		}
		if (max_width != 0) {
			config.max_width = max_width;
		}
		if (max_col_width != 0) {
			config.max_col_width = max_col_width;
		}
		if (null_value->len) {
			config.null_value = std::string(null_value->ptr, null_value->len);
		}
		config.render_mode = render_mode == 1 ? duckdb::RenderMode::COLUMNS : duckdb::RenderMode::ROWS;
		// The caller's query-side LIMIT: when the materialized result fills it, the
		// footer renders "? rows" since the true total is unknown.
		config.limit = limit;

		duckdb::ClientBoxRendererContext render_context(ctx);
		duckdb::BoxRenderer renderer(config);
		duckdb::ColumnDataCollectionWrapper data(collection);

		duckdb::vector<duckdb::string> names;
		for (auto &ident : wrapper->names) {
			names.push_back(ident.GetIdentifierName());
		}

		auto text = renderer.ToString(render_context, names, data);
		InvokeTextSink(sink, Convert(text), user_data);
	});
}

DUCKDB_V2_ERROR duckdb_v2_result_destroy(duckdb_v2_result_handle *result) {
	return WithErrorHandler(nullptr, [&]() {
		if (!result || !*result) {
			return;
		}
		// Adopt so the wrapper is freed even if Finalize() throws.
		duckdb::unique_ptr<ResultWrapperV2> wrapper(Convert(*result));
		*result = nullptr;
		wrapper->Finalize();
	});
}

// ---------------------------------------------------------------------------
// Streaming consumption
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_result_step(duckdb_v2_result_handle result, duckdb_v2_data_chunk_handle *out_chunk,
                                      DUCKDB_V2_RESULT_STEP_STATUS *out_status, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(out_chunk);
	DUCKDB_CHECK_ARG(out_status);
	*out_chunk = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *wrapper = Convert(result);
		duckdb::unique_ptr<duckdb::DataChunk> chunk;
		auto status = wrapper->Step<duckdb::ChunkFormat>(chunk);
		*out_status = status;
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK) {
			*out_chunk = Convert(chunk.release());
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_result_fetch_chunk(duckdb_v2_result_handle result, duckdb_v2_data_chunk_handle *out_chunk,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(out_chunk);
	*out_chunk = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *wrapper = Convert(result);
		auto chunk = wrapper->FetchBlocking<duckdb::ChunkFormat>();
		if (chunk) {
			*out_chunk = Convert(chunk.release());
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_result_wait(duckdb_v2_result_handle result, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	return WithErrorHandler(err, [&]() { Convert(result)->Wait(); });
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_result_get_result_type(duckdb_v2_result_handle result, DUCKDB_V2_RESULT_TYPE *out_type,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(out_type);

	return WithErrorHandler(err, [&]() {
		auto *r = Convert(result);
		r->RequireMetadata();
		*out_type = MapResultType(r->properties.return_type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_result_get_statement_type(duckdb_v2_result_handle result, DUCKDB_V2_STATEMENT_TYPE *out_type,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(out_type);

	return WithErrorHandler(err, [&]() { *out_type = Convert(Convert(result)->statement_type); });
}

DUCKDB_V2_ERROR duckdb_v2_result_get_schema(duckdb_v2_result_handle result, duckdb_v2_schema_handle *out_schema,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_schema);
	*out_schema = nullptr;

	DUCKDB_CHECK_ARG(result);

	return WithErrorHandler(err, [&]() {
		auto *r = Convert(result);
		r->RequireMetadata();
		auto schema = duckdb::make_uniq<CV2Schema>();
		for (duckdb::idx_t i = 0; i < r->types.size(); i++) {
			schema->fields.push_back({r->names[i].GetIdentifierName(), r->types[i]});
		}
		*out_schema = Convert(schema.release());
	});
}
