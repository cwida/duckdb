#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/common/sql_identifier.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_identifier_render_quoted(const duckdb_v2_identifier_t *name, char *out_text,
                                                   idx_t out_capacity, idx_t *out_length,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(out_length);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		auto rendered = duckdb::SQLIdentifier::ToString(duckdb::string(ConvertIdentifierName(name)));
		FillCallerText(out_text, out_capacity, out_length, rendered, "duckdb_v2_identifier_render_quoted");
	});
}
