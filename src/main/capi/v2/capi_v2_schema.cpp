#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_schema_get_count(duckdb_v2_schema_handle schema, idx_t *out_count,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(schema);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = Convert(schema)->fields.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_schema_get_field(duckdb_v2_schema_handle schema, idx_t index,
                                           duckdb_v2_identifier_t *out_name, duckdb_v2_logical_type_handle *out_type,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(schema);
	DUCKDB_CHECK_ARG(out_name);
	DUCKDB_CHECK_ARG(out_type);
	return WithErrorHandler(err, [&]() {
		*out_name = duckdb_v2_identifier_t {nullptr, 0};
		*out_type = nullptr;
		auto &wrapper = *Convert(schema);
		if (index >= wrapper.fields.size()) {
			throw duckdb::InvalidInputException("index out of range in duckdb_v2_schema_get_field");
		}
		auto &field = wrapper.fields[index];
		// Both borrowed, valid until the schema is destroyed; out_type aliases the
		// wrapper-owned LogicalType and must not be destroyed by the caller.
		*out_name = Convert(field.name);
		*out_type = ConvertBorrowed(field.type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_schema_destroy(duckdb_v2_schema_handle *schema) {
	if (!schema || !*schema) {
		return static_cast<DUCKDB_V2_ERROR>(DUCKDB_V2_ERROR_NONE);
	}
	delete Convert(*schema);
	*schema = nullptr;
	return static_cast<DUCKDB_V2_ERROR>(DUCKDB_V2_ERROR_NONE);
}
