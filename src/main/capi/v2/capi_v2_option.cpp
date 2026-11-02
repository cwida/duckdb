#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_option_destroy(duckdb_v2_option_handle *option) {
	return WithErrorHandler(nullptr, [&]() {
		if (!option) {
			return;
		}
		if (*option) {
			delete Convert(*option);
			*option = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_option_get_name(duckdb_v2_option_handle option, duckdb_v2_identifier_t *out_name,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_name);
	return WithErrorHandler(err, [&]() { *out_name = Convert(Convert(option)->name); });
}

DUCKDB_V2_ERROR duckdb_v2_option_get_default_value(duckdb_v2_option_handle option, duckdb_v2_value_handle *out_value,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() { *out_value = Convert(new duckdb::Value(Convert(option)->default_value)); });
}

DUCKDB_V2_ERROR duckdb_v2_option_get_description(duckdb_v2_option_handle option, duckdb_v2_str *out_description,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_description);
	return WithErrorHandler(err, [&]() { *out_description = Convert(Convert(option)->description); });
}

DUCKDB_V2_ERROR duckdb_v2_option_supports_scope(duckdb_v2_option_handle option, DUCKDB_V2_SETTING_SCOPE scope,
                                                bool *out_supported, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_supported);
	return WithErrorHandler(err, [&]() {
		const auto &wrapper = *Convert(option);
		switch (scope) {
		case DUCKDB_V2_SETTING_SCOPE_GLOBAL:
			*out_supported = wrapper.supports_global;
			break;
		case DUCKDB_V2_SETTING_SCOPE_SESSION:
			*out_supported = wrapper.supports_session;
			break;
		default:
			*out_supported = wrapper.supports_global || wrapper.supports_session;
			break;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_option_get_default_scope(duckdb_v2_option_handle option, DUCKDB_V2_SETTING_SCOPE *out_scope,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_scope);
	return WithErrorHandler(err, [&]() { *out_scope = Convert(option)->default_scope; });
}

DUCKDB_V2_ERROR duckdb_v2_option_get_alias_count(duckdb_v2_option_handle option, idx_t *out_count,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = static_cast<idx_t>(Convert(option)->aliases.size()); });
}

DUCKDB_V2_ERROR duckdb_v2_option_get_alias(duckdb_v2_option_handle option, idx_t index,
                                           duckdb_v2_identifier_t *out_alias, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(option);
	DUCKDB_CHECK_ARG(out_alias);
	return WithErrorHandler(err, [&]() {
		const auto &wrapper = *Convert(option);
		if (index >= wrapper.aliases.size()) {
			*out_alias = duckdb_v2_str {nullptr, 0};
			throw duckdb::InvalidInputException("alias index out of range in duckdb_v2_option_get_alias");
		}
		*out_alias = Convert(wrapper.aliases[index]);
	});
}

// ---------------------------------------------------------------------------
// Context options: the connection's cascade, from inside DuckDB
// ---------------------------------------------------------------------------
