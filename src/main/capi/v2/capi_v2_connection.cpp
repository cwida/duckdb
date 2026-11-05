#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_connection_create(duckdb_v2_instance_handle instance, duckdb_v2_connection_handle *out_conn,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(out_conn);
	*out_conn = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &instance_wrapper = *Convert(instance);
		auto connection = duckdb::make_uniq<CV2Connection>(instance_wrapper.GetDatabase());
		*out_conn = Convert(connection.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_destroy(duckdb_v2_connection_handle *conn) {
	return WithErrorHandler(nullptr, [&]() {
		if (!conn) {
			return;
		}
		if (*conn) {
			delete Convert(*conn);
			*conn = nullptr;
		}
	});
}

// ---------------------------------------------------------------------------
// Query process management
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_connection_interrupt(duckdb_v2_connection_handle conn, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	return WithErrorHandler(err, [&]() {
		// Record that the cancellation was user-initiated.
		auto &context = *Convert(conn)->context;
		GetBusySlot(context)->cancel_requests.fetch_add(1);

		// ClientContext::Interrupt is an atomic store; safe to call from any thread
		context.Interrupt();
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_progress_get(duckdb_v2_connection_handle conn, double *out_percentage,
                                                  uint64_t *out_rows_processed, uint64_t *out_total_rows_to_process,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(out_percentage);
	DUCKDB_CHECK_ARG(out_rows_processed);
	DUCKDB_CHECK_ARG(out_total_rows_to_process);
	return WithErrorHandler(err, [&]() {
		auto progress = Convert(conn)->context->GetQueryProgress();
		*out_percentage = progress.GetPercentage();
		*out_rows_processed = progress.GetRowsProcessed();
		*out_total_rows_to_process = progress.GetTotalRowsToProcess();
	});
}
