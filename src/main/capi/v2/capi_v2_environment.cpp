#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_environment_create(duckdb_v2_environment_handle *out_env, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_env);
	*out_env = nullptr;
	return WithErrorHandler(err, [&]() {
		auto wrapper = duckdb::make_uniq<CV2Environment>();
		wrapper->cache = duckdb::make_uniq<duckdb::DBInstanceCache>();
		*out_env = Convert(wrapper.release());
	});
}

// environment_destroy keeps a manual return path so the live-instances case
// can surface as RESOURCE_IN_USE — there is no ExceptionType that maps to
// that V2 code, so routing it through WithErrorHandler would degrade it.
DUCKDB_V2_ERROR duckdb_v2_environment_destroy(duckdb_v2_environment_handle *env) {
	if (!env || !*env) {
		return DUCKDB_V2_ERROR_NONE;
	}
	const auto *wrapper = Convert(*env);
	auto count = wrapper->instance_count.load(std::memory_order_acquire);
	if (count != 0) {
		return DUCKDB_V2_ERROR_RESOURCE_IN_USE;
	}
	delete wrapper;
	*env = nullptr;
	return DUCKDB_V2_ERROR_NONE;
}

DUCKDB_V2_ERROR duckdb_v2_environment_get_instance_count(duckdb_v2_environment_handle env, idx_t *out_count,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(env);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = Convert(env)->instance_count.load(); });
}
