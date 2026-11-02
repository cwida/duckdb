#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_instance_get_factory(duckdb_v2_instance_handle instance,
                                               duckdb_v2_factory_handle *out_factory,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(out_factory);
	*out_factory = nullptr;
	return WithErrorHandler(err, [&]() { *out_factory = Convert(&Convert(instance)->factory); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_get_factory(duckdb_v2_connection_handle conn,
                                                 duckdb_v2_factory_handle *out_factory,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(out_factory);
	*out_factory = nullptr;
	return WithErrorHandler(err, [&]() { *out_factory = Convert(&Convert(conn)->context_handle.factory); });
}

DUCKDB_V2_ERROR duckdb_v2_context_get_factory(duckdb_v2_context_handle ctx, duckdb_v2_factory_handle *out_factory,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(ctx);
	DUCKDB_CHECK_ARG(out_factory);
	*out_factory = nullptr;
	return WithErrorHandler(err, [&]() { *out_factory = Convert(&Convert(ctx)->factory); });
}
