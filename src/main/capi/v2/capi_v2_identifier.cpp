#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/common/sql_identifier.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_identifier_render_quoted(const char *name, idx_t name_length, char *out_text,
                                                   idx_t out_capacity, idx_t *out_length,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_STR_ARG(name, name_length);
	DUCKDB_CHECK_ARG(out_length);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		auto rendered = duckdb::SQLIdentifier::ToString(duckdb::string(ConvertIdentifierName(name, name_length)));
		FillCallerText(out_text, out_capacity, out_length, rendered, "duckdb_v2_identifier_render_quoted");
	});
}
