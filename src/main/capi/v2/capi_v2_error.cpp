#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_error_info_get_text(duckdb_v2_error_info_handle info, const char **out_text,
                                              idx_t *out_text_length) {
	if (!info || !out_text || !out_text_length) {
		return DUCKDB_V2_ERROR_INPUT_INVALID;
	}
	const auto *ei = Convert(info);
	SetStringOut(out_text, out_text_length, ei->message);
	return DUCKDB_V2_ERROR_NONE;
}

DUCKDB_V2_ERROR duckdb_v2_error_info_get_raw_text(duckdb_v2_error_info_handle info, const char **out_raw_text,
                                                  idx_t *out_raw_text_length) {
	if (!info || !out_raw_text || !out_raw_text_length) {
		return DUCKDB_V2_ERROR_INPUT_INVALID;
	}
	const auto *ei = Convert(info);
	SetStringOut(out_raw_text, out_raw_text_length,
	             ei->raw_message ? std::string_view(ei->raw_message.value()) : std::string_view());
	return DUCKDB_V2_ERROR_NONE;
}

DUCKDB_V2_ERROR duckdb_v2_error_info_set_text(duckdb_v2_error_info_handle info, const char *text, idx_t text_length) {
	if (!info) {
		return DUCKDB_V2_ERROR_NONE;
	}
	if (IsNullArgument(text, text_length)) {
		return DUCKDB_V2_ERROR_INPUT_INVALID;
	}
	auto *ei = Convert(info);
	ei->message = Convert(text, text_length);
	ei->raw_message.reset(); // Directly-set message has no raw body.
	return DUCKDB_V2_ERROR_NONE;
}

DUCKDB_V2_ERROR duckdb_v2_error_info_get_code(duckdb_v2_error_info_handle info, DUCKDB_V2_ERROR *out_code) {
	if (!info || !out_code) {
		return DUCKDB_V2_ERROR_INPUT_INVALID;
	}
	const auto *ei = Convert(info);
	*out_code = ei->code;
	return DUCKDB_V2_ERROR_NONE;
}

DUCKDB_V2_ERROR duckdb_v2_error_info_set_code(duckdb_v2_error_info_handle info, DUCKDB_V2_ERROR code) {
	if (!info) {
		return DUCKDB_V2_ERROR_NONE;
	}
	const auto ei = Convert(info);
	ei->code = code;
	return DUCKDB_V2_ERROR_NONE;
}

DUCKDB_V2_ERROR duckdb_v2_error_info_destroy(duckdb_v2_error_info_handle *info) {
	if (info && *info) {
		delete Convert(*info);
		*info = nullptr;
	}
	return DUCKDB_V2_ERROR_NONE;
}
