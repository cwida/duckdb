#include "test_capi_v2.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 variant builder tests: produce VARIANT rows value by value through a
// table function's output vector, then read them back through SQL and compare
// them with the same values built by the engine's own casts.
//
// The callbacks avoid Catch assertions: on failure they populate the error
// slot and return, and the failure surfaces as a query error.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_str Str(const char *s) {
	return duckdb_v2_str {s, std::strlen(s)};
}

// One failed call ends the build: the macro returns false from the enclosing function, leaving `err` set.
#define BUILD_CHECK(call)                                                                                              \
	do {                                                                                                               \
		if ((call) != DUCKDB_V2_ERROR_NONE) {                                                                          \
			return false;                                                                                              \
		}                                                                                                              \
	} while (0)

// The rows variant_rows() produces. Row 1 is never begun and row 2 holds a lone null: both read as SQL NULL.
constexpr idx_t VARIANT_ROW_COUNT = 7;

bool AppendScalar(duckdb_v2_variant_builder_handle b, DUCKDB_V2_LOGICAL_TYPE_ID id, const void *data,
                  duckdb_v2_error_info_handle *err) {
	return duckdb_v2_variant_builder_append_scalar(b, id, data, err) == DUCKDB_V2_ERROR_NONE;
}

bool AppendKey(duckdb_v2_variant_builder_handle b, const char *key, duckdb_v2_error_info_handle *err) {
	auto str = Str(key);
	return duckdb_v2_variant_builder_append_key(b, &str, err) == DUCKDB_V2_ERROR_NONE;
}

bool AppendText(duckdb_v2_variant_builder_handle b, const char *text, duckdb_v2_error_info_handle *err) {
	return duckdb_v2_variant_builder_append_bytes(b, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR,
	                                              reinterpret_cast<const uint8_t *>(text), std::strlen(text),
	                                              err) == DUCKDB_V2_ERROR_NONE;
}

// Row 0: {'name': 'duck', 'n': 42, 'ok': true, 'none': null, 'list': [1.5, 'x', {}], 'empty': []} - the keys are
// given out of order on purpose.
bool BuildMixedObject(duckdb_v2_variant_builder_handle b, duckdb_v2_error_info_handle *err) {
	BUILD_CHECK(duckdb_v2_variant_builder_begin_object(b, 6, err));
	if (!AppendKey(b, "name", err) || !AppendText(b, "duck", err)) {
		return false;
	}
	int32_t n = 42;
	if (!AppendKey(b, "n", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, &n, err)) {
		return false;
	}
	bool ok = true;
	if (!AppendKey(b, "ok", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN, &ok, err)) {
		return false;
	}
	if (!AppendKey(b, "none", err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_append_null(b, err));
	if (!AppendKey(b, "list", err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_begin_array(b, 3, err));
	double d = 1.5;
	if (!AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE, &d, err) || !AppendText(b, "x", err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_begin_object(b, 0, err));
	BUILD_CHECK(duckdb_v2_variant_builder_end_object(b, err));
	BUILD_CHECK(duckdb_v2_variant_builder_end_array(b, err));
	if (!AppendKey(b, "empty", err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_begin_array(b, 0, err));
	BUILD_CHECK(duckdb_v2_variant_builder_end_array(b, err));
	BUILD_CHECK(duckdb_v2_variant_builder_end_object(b, err));
	return true;
}

// Row 3: one member per typed scalar kind, matching TYPED_OBJECT_SQL below.
bool BuildTypedObject(duckdb_v2_variant_builder_handle b, duckdb_v2_error_info_handle *err) {
	BUILD_CHECK(duckdb_v2_variant_builder_begin_object(b, 13, err));
	int32_t date = 19782; // 2024-02-29
	if (!AppendKey(b, "date", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_DATE, &date, err)) {
		return false;
	}
	int64_t time = (12 * 3600 + 34 * 60 + 56) * 1000000LL + 789000; // 12:34:56.789
	if (!AppendKey(b, "time", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_TIME, &time, err)) {
		return false;
	}
	int64_t ts = date * 86400000000LL + time; // 2024-02-29 12:34:56.789
	if (!AppendKey(b, "ts", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP, &ts, err)) {
		return false;
	}
	int64_t ts_ms = ts / 1000;
	if (!AppendKey(b, "ts_ms", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_MS, &ts_ms, err)) {
		return false;
	}
	int64_t dec18 = 1234567; // 1234.567 as DECIMAL(18, 3)
	if (!AppendKey(b, "dec", err) ||
	    duckdb_v2_variant_builder_append_decimal(b, 18, 3, &dec18, err) != DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	int16_t dec4 = 125; // 12.5 as DECIMAL(4, 1)
	if (!AppendKey(b, "dec2", err) ||
	    duckdb_v2_variant_builder_append_decimal(b, 4, 1, &dec4, err) != DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	duckdb_v2_hugeint_t dec38 = {12345678901ULL, 0}; // 1.2345678901 as DECIMAL(38, 10)
	if (!AppendKey(b, "dec38", err) ||
	    duckdb_v2_variant_builder_append_decimal(b, 38, 10, &dec38, err) != DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	const uint8_t blob[] = {0x00, 0x01, 0xFF};
	if (!AppendKey(b, "blob", err) ||
	    duckdb_v2_variant_builder_append_bytes(b, DUCKDB_V2_LOGICAL_TYPE_ID_BLOB, blob, sizeof(blob), err) !=
	        DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	int64_t big = -5;
	if (!AppendKey(b, "big", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &big, err)) {
		return false;
	}
	duckdb_v2_hugeint_t hug = {0, 64}; // 2^70
	if (!AppendKey(b, "hug", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_HUGEINT, &hug, err)) {
		return false;
	}
	uint8_t u8 = 255;
	if (!AppendKey(b, "u8", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_UTINYINT, &u8, err)) {
		return false;
	}
	float f = 0.5f;
	if (!AppendKey(b, "f", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_FLOAT, &f, err)) {
		return false;
	}
	duckdb_v2_interval_t iv = {1, 2, 3};
	if (!AppendKey(b, "iv", err) || !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_INTERVAL, &iv, err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_end_object(b, err));
	return true;
}

const char *TYPED_OBJECT_SQL = "{'date': DATE '2024-02-29', 'time': TIME '12:34:56.789', "
                               "'ts': TIMESTAMP '2024-02-29 12:34:56.789', 'ts_ms': TIMESTAMP_MS '2024-02-29 "
                               "12:34:56.789', 'dec': 1234.567::DECIMAL(18, 3), 'dec2': 12.5::DECIMAL(4, 1), "
                               "'dec38': 1.2345678901::DECIMAL(38, 10), 'blob': '\\x00\\x01\\xFF'::BLOB, "
                               "'big': -5::BIGINT, 'hug': (2::HUGEINT ^ 70)::HUGEINT, 'u8': 255::UTINYINT, 'f': "
                               "0.5::FLOAT, 'iv': INTERVAL '1 month 2 days 3 microseconds'}::VARIANT";

// Row 5: [[1, 2], [3]]
bool BuildNestedArrays(duckdb_v2_variant_builder_handle b, duckdb_v2_error_info_handle *err) {
	BUILD_CHECK(duckdb_v2_variant_builder_begin_array(b, 2, err));
	BUILD_CHECK(duckdb_v2_variant_builder_begin_array(b, 2, err));
	int64_t one = 1, two = 2, three = 3;
	if (!AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &one, err) ||
	    !AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &two, err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_end_array(b, err));
	BUILD_CHECK(duckdb_v2_variant_builder_begin_array(b, 1, err));
	if (!AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &three, err)) {
		return false;
	}
	BUILD_CHECK(duckdb_v2_variant_builder_end_array(b, err));
	BUILD_CHECK(duckdb_v2_variant_builder_end_array(b, err));
	return true;
}

// Fills `vector` with the VARIANT_ROW_COUNT rows.
bool BuildRows(duckdb_v2_vector_handle vector, duckdb_v2_error_info_handle *err) {
	duckdb_v2_variant_builder_handle b = nullptr;
	BUILD_CHECK(duckdb_v2_variant_builder_create(vector, VARIANT_ROW_COUNT, &b, err));
	bool ok = false;
	do {
		if (duckdb_v2_variant_builder_begin_row(b, 0, err) != DUCKDB_V2_ERROR_NONE || !BuildMixedObject(b, err) ||
		    duckdb_v2_variant_builder_end_row(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		// row 1 is skipped: never begun
		if (duckdb_v2_variant_builder_begin_row(b, 2, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_variant_builder_append_null(b, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_variant_builder_end_row(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (duckdb_v2_variant_builder_begin_row(b, 3, err) != DUCKDB_V2_ERROR_NONE || !BuildTypedObject(b, err) ||
		    duckdb_v2_variant_builder_end_row(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (duckdb_v2_variant_builder_begin_row(b, 4, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_variant_builder_begin_object(b, 0, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_variant_builder_end_object(b, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_variant_builder_end_row(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (duckdb_v2_variant_builder_begin_row(b, 5, err) != DUCKDB_V2_ERROR_NONE || !BuildNestedArrays(b, err) ||
		    duckdb_v2_variant_builder_end_row(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (duckdb_v2_variant_builder_begin_row(b, 6, err) != DUCKDB_V2_ERROR_NONE ||
		    !AppendText(b, "just text", err) || duckdb_v2_variant_builder_end_row(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (duckdb_v2_variant_builder_finish(b, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		ok = true;
	} while (false);
	duckdb_v2_variant_builder_destroy(&b);
	return ok;
}

// ---------------------------------------------------------------------------
// variant_rows(): a VARIANT column "v" with the rows above and their index "i", produced in one batch.
// ---------------------------------------------------------------------------

struct RowsGlobal {
	bool done = false;
};

void DeleteRowsGlobal(void *ptr) {
	delete static_cast<RowsGlobal *>(ptr);
}

void RowsBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle variant = nullptr;
	auto text = Str("VARIANT");
	if (duckdb_v2_factory_create_type_from_text(factory, &text, &variant, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto name = duckdb_v2_identifier_t {"v", 1};
	auto rc = duckdb_v2_table_function_bind_add_result_column(result, &name, variant, err);
	duckdb_v2_logical_type_destroy(&variant);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, 0, &bigint,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto index_name = duckdb_v2_identifier_t {"i", 1};
	rc = duckdb_v2_table_function_bind_add_result_column(result, &index_name, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_bind_set_cardinality(result, VARIANT_ROW_COUNT, true, err);
}

void RowsInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new RowsGlobal(), DeleteRowsGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void RowsExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<RowsGlobal *>(global_ptr);
	duckdb_v2_vector_handle vec = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (global.done) {
		duckdb_v2_vector_set_size(vec, 0, err);
		return;
	}
	global.done = true;
	if (!BuildRows(vec, err)) {
		return;
	}
	duckdb_v2_vector_handle index = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 1, &index, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(index, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < VARIANT_ROW_COUNT; i++) {
		static_cast<int64_t *>(raw)[i] = static_cast<int64_t>(i);
	}
	duckdb_v2_vector_set_size(vec, VARIANT_ROW_COUNT, err);
}

void RegisterVariantRows(EnvFixture &fx) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Str("variant_rows");
	REQUIRE(duckdb_v2_table_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, RowsBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, RowsInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RowsExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

// The rows of a single-column VARCHAR query, "NULL" standing in for NULL.
std::vector<std::string> QueryStrings(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql.c_str(), &result, &err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		FAIL("query failed: " << sql << ": " << Convert(text));
	}
	auto varchar = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	std::vector<std::string> rows;
	while (auto chunk = StepChunk(result)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
		for (idx_t i = 0; i < size; i++) {
			duckdb_v2_value_handle value = nullptr;
			REQUIRE(duckdb_v2_vector_get_value(vec, i, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
			bool is_null = false;
			REQUIRE(duckdb_v2_value_is_null(value, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
			if (is_null) {
				rows.push_back("NULL");
			} else {
				duckdb_v2_value_handle text = nullptr;
				REQUIRE(duckdb_v2_value_cast(FactoryOf(conn), value, varchar, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
				rows.push_back(ConsumeValue<std::string>(text));
			}
			duckdb_v2_value_destroy(&value);
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_result_destroy(&result);
	return rows;
}

std::string QueryString(duckdb_v2_connection_handle conn, const std::string &sql) {
	auto rows = QueryStrings(conn, sql);
	REQUIRE(rows.size() == 1);
	return rows[0];
}

// A one-row VARIANT chunk for the misuse tests.
struct VariantChunk {
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_vector_handle vector = nullptr;

	explicit VariantChunk(duckdb_v2_factory_handle factory) {
		duckdb_v2_logical_type_handle variant = nullptr;
		auto text = Str("VARIANT");
		REQUIRE(duckdb_v2_factory_create_type_from_text(factory, &text, &variant, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_data_chunk_create(factory, &variant, 1, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_logical_type_destroy(&variant);
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vector, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	~VariantChunk() {
		duckdb_v2_data_chunk_destroy(&chunk);
	}
};

// The error text of a failed call, destroying the error.
std::string TakeErrorText(duckdb_v2_error_info_handle &err) {
	REQUIRE(err != nullptr);
	duckdb_v2_str text = {nullptr, 0};
	duckdb_v2_error_info_get_text(err, &text);
	auto out = Convert(text);
	duckdb_v2_error_info_destroy(&err);
	return out;
}

} // namespace

TEST_CASE("V2 variant builder: rows read back through SQL", "[capi_v2][variant_builder]") {
	EnvFixture fx;
	RegisterVariantRows(fx);

	// Row 1 was never begun, row 2 holds a lone VARIANT null: both are SQL NULL.
	REQUIRE(QueryStrings(fx.conn, "SELECT v IS NULL FROM variant_rows() ORDER BY i") ==
	        std::vector<std::string> {"false", "true", "true", "false", "false", "false", "false"});

	// Object keys are listed in sorted order whatever order they were appended in; a NULL row types as VARIANT_NULL.
	REQUIRE(QueryStrings(fx.conn, "SELECT variant_typeof(v) FROM variant_rows() ORDER BY i") ==
	        std::vector<std::string> {"OBJECT(empty, list, n, name, none, ok)", "VARIANT_NULL", "VARIANT_NULL",
	                                  "OBJECT(big, blob, date, dec, dec2, dec38, f, hug, iv, time, ts, ts_ms, u8)",
	                                  "OBJECT()", "ARRAY(2)", "VARCHAR"});

	// Row 0: the heterogeneous object, member by member.
	auto row0 = "SELECT %s FROM variant_rows() WHERE i = 0";
	auto member = [&](const char *expr) {
		std::string sql = row0;
		sql.replace(sql.find("%s"), 2, expr);
		return QueryString(fx.conn, sql);
	};
	REQUIRE(member("variant_extract(v, 'name')::VARCHAR") == "duck");
	REQUIRE(member("variant_typeof(variant_extract(v, 'n'))") == "INT32");
	REQUIRE(member("variant_extract(v, 'n')::INTEGER") == "42");
	REQUIRE(member("variant_extract(v, 'ok')::BOOLEAN") == "true");
	REQUIRE(member("variant_typeof(variant_extract(v, 'none'))") == "VARIANT_NULL");
	REQUIRE(member("variant_typeof(variant_extract(v, 'list'))") == "ARRAY(3)");
	REQUIRE(member("variant_typeof(variant_extract(variant_extract(v, 'list'), 1))") == "DOUBLE");
	REQUIRE(member("variant_extract(variant_extract(v, 'list'), 2)::VARCHAR") == "x");
	REQUIRE(member("variant_typeof(variant_extract(variant_extract(v, 'list'), 3))") == "OBJECT()");
	REQUIRE(member("variant_typeof(variant_extract(v, 'empty'))") == "ARRAY(0)");

	// Row 3: every typed scalar keeps its type and value; the whole object equals the engine's own cast.
	std::string typed = "SELECT %s FROM variant_rows() WHERE i = 3";
	auto typed_member = [&](const char *expr) {
		std::string sql = typed;
		sql.replace(sql.find("%s"), 2, expr);
		return QueryString(fx.conn, sql);
	};
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'date'))") == "DATE");
	REQUIRE(typed_member("variant_extract(v, 'date')::DATE::VARCHAR") == "2024-02-29");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'time'))") == "TIME_MICROS");
	REQUIRE(typed_member("variant_extract(v, 'time')::TIME::VARCHAR") == "12:34:56.789");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'ts'))") == "TIMESTAMP_MICROS");
	REQUIRE(typed_member("variant_extract(v, 'ts')::TIMESTAMP::VARCHAR") == "2024-02-29 12:34:56.789");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'ts_ms'))") == "TIMESTAMP_MILIS");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'dec'))") == "DECIMAL(18, 3)");
	REQUIRE(typed_member("variant_extract(v, 'dec')::DECIMAL(18, 3)::VARCHAR") == "1234.567");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'dec2'))") == "DECIMAL(4, 1)");
	REQUIRE(typed_member("variant_extract(v, 'dec2')::VARCHAR") == "12.5");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'dec38'))") == "DECIMAL(38, 10)");
	REQUIRE(typed_member("variant_extract(v, 'dec38')::VARCHAR") == "1.2345678901");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'blob'))") == "BLOB");
	REQUIRE(typed_member("variant_extract(v, 'blob')::BLOB::VARCHAR") == "\\x00\\x01\\xFF");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'hug'))") == "INT128");
	REQUIRE(typed_member("variant_extract(v, 'hug')::HUGEINT::VARCHAR") == "1180591620717411303424");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'u8'))") == "UINT8");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'f'))") == "FLOAT");
	REQUIRE(typed_member("variant_typeof(variant_extract(v, 'iv'))") == "INTERVAL");
	REQUIRE(typed_member("variant_extract(v, 'iv')::INTERVAL::VARCHAR") == "1 month 2 days 00:00:00.000003");
	REQUIRE(typed_member(("v = " + std::string(TYPED_OBJECT_SQL)).c_str()) == "true");
	REQUIRE(typed_member("v::VARCHAR") ==
	        QueryString(fx.conn, std::string("SELECT ") + TYPED_OBJECT_SQL + "::VARCHAR"));

	// Rows 5 and 6: nested arrays and a scalar root render like the engine's casts.
	REQUIRE(QueryString(fx.conn, "SELECT v::VARCHAR FROM variant_rows() WHERE i = 5") ==
	        QueryString(fx.conn, "SELECT [[1, 2], [3]]::BIGINT[][]::VARIANT::VARCHAR"));
	REQUIRE(QueryString(fx.conn, "SELECT v = [[1, 2], [3]]::BIGINT[][]::VARIANT FROM variant_rows() WHERE i = 5") ==
	        "true");
	REQUIRE(QueryString(fx.conn, "SELECT v::VARCHAR FROM variant_rows() WHERE i = 6") == "just text");

	// The rows survive being stored and read back.
	REQUIRE(RunSql(fx.conn, "CREATE TABLE built AS SELECT v, i FROM variant_rows()") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(QueryString(fx.conn, "SELECT count(*) FROM built WHERE v IS NOT NULL") == "5");
	REQUIRE(QueryString(fx.conn, "SELECT variant_extract(v, 'name')::VARCHAR FROM built WHERE i = 0") == "duck");
}

TEST_CASE("V2 variant builder: misuse is refused", "[capi_v2][variant_builder]") {
	EnvFixture fx;
	VariantChunk chunk(fx.factory);
	duckdb_v2_error_info_handle err = nullptr;

	// Not a VARIANT vector.
	{
		auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
		duckdb_v2_data_chunk_handle int_chunk = nullptr;
		REQUIRE(duckdb_v2_data_chunk_create(fx.factory, &integer, 1, &int_chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_logical_type_destroy(&integer);
		duckdb_v2_vector_handle int_vec = nullptr;
		REQUIRE(duckdb_v2_data_chunk_get_vector(int_chunk, 0, &int_vec, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_variant_builder_handle b = reinterpret_cast<duckdb_v2_variant_builder_handle>(0x1);
		REQUIRE(duckdb_v2_variant_builder_create(int_vec, 1, &b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(b == nullptr);
		REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("not VARIANT"));
		duckdb_v2_data_chunk_destroy(&int_chunk);
	}

	duckdb_v2_variant_builder_handle b = nullptr;
	REQUIRE(duckdb_v2_variant_builder_create(chunk.vector, 2, &b, nullptr) == DUCKDB_V2_ERROR_NONE);
	int64_t one = 1;

	// Values need an open row.
	REQUIRE(duckdb_v2_variant_builder_append_null(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("no row is open"));
	REQUIRE(duckdb_v2_variant_builder_end_row(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("no row is open"));

	// A row out of range, a row begun twice, a row ended without a value.
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 2, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("out of range"));
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 1, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("still open"));
	REQUIRE(duckdb_v2_variant_builder_end_row(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("no value"));

	// A row holds exactly one value.
	REQUIRE(AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &one, nullptr));
	REQUIRE(duckdb_v2_variant_builder_append_null(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("exactly one"));
	REQUIRE(duckdb_v2_variant_builder_end_row(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 0, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("begun before"));

	// Containers: counts, keys and nesting are checked.
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 1, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_begin_object(b, 2, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_append_null(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("append_key first"));
	REQUIRE(duckdb_v2_variant_builder_end_array(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("no array is open"));
	REQUIRE(duckdb_v2_variant_builder_end_object(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("begun with 2 children but holds 0"));
	REQUIRE(AppendKey(b, "a", nullptr));
	REQUIRE(AppendKey(b, "b", &err) == false);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("awaits its value"));
	REQUIRE(duckdb_v2_variant_builder_end_object(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("awaits its value"));
	REQUIRE(AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &one, nullptr));
	// The same key twice in one object is refused; the object still takes a different key.
	REQUIRE(AppendKey(b, "a", &err) == false);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("key \"a\" more than once"));
	REQUIRE(AppendKey(b, "b", nullptr));
	REQUIRE(AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &one, nullptr));
	REQUIRE(AppendKey(b, "c", &err) == false);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("already has all 2 members"));
	// finish and end_row refuse while the object is open.
	REQUIRE(duckdb_v2_variant_builder_finish(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("still open"));
	REQUIRE(duckdb_v2_variant_builder_end_row(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("still open"));
	REQUIRE(duckdb_v2_variant_builder_end_object(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_end_row(b, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Scalars: the type must be a fixed-width one, the payload present (checked before the row state).
	REQUIRE(duckdb_v2_variant_builder_append_scalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, &one, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("append_bytes"));
	REQUIRE(duckdb_v2_variant_builder_append_scalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL, &one, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("append_decimal"));
	REQUIRE(duckdb_v2_variant_builder_append_scalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_LIST, &one, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("not a fixed-width scalar"));
	REQUIRE(duckdb_v2_variant_builder_append_scalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("data is NULL"));
	REQUIRE(duckdb_v2_variant_builder_append_decimal(b, 39, 0, &one, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("width 39"));
	REQUIRE(duckdb_v2_variant_builder_append_decimal(b, 4, 5, &one, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("scale 5"));
	REQUIRE(duckdb_v2_variant_builder_append_bytes(b, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, 0, &err) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("not a text or binary type"));

	// The rows written so far are kept; finish is final.
	REQUIRE(duckdb_v2_variant_builder_finish(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_finish(b, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_THAT(TakeErrorText(err), Catch::Matchers::Contains("finished"));
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 0, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(duckdb_v2_variant_builder_destroy(&b) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(b == nullptr);
	REQUIRE(duckdb_v2_variant_builder_destroy(&b) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);

	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_vector_handle vec = chunk.vector;
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle text = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, value, varchar, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<std::string>(text) == "1");
	duckdb_v2_value_destroy(&value);
	REQUIRE(duckdb_v2_vector_get_value(vec, 1, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_cast(fx.factory, value, varchar, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<std::string>(text) == "{'a': 1, 'b': 1}");
	duckdb_v2_value_destroy(&value);
	duckdb_v2_logical_type_destroy(&varchar);
}

TEST_CASE("V2 variant builder: an empty batch and a destroyed unfinished builder", "[capi_v2][variant_builder]") {
	EnvFixture fx;
	VariantChunk chunk(fx.factory);

	// Zero rows: finish succeeds and the vector is empty.
	duckdb_v2_variant_builder_handle b = nullptr;
	REQUIRE(duckdb_v2_variant_builder_create(chunk.vector, 0, &b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_finish(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_variant_builder_destroy(&b);
	idx_t size = 1;
	REQUIRE(duckdb_v2_vector_get_size(chunk.vector, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);

	// Rows given to a builder that is destroyed without finish never reach the vector.
	REQUIRE(duckdb_v2_variant_builder_create(chunk.vector, 1, &b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_variant_builder_begin_row(b, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(AppendText(b, "lost", nullptr));
	REQUIRE(duckdb_v2_variant_builder_end_row(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_variant_builder_destroy(&b);
	REQUIRE(duckdb_v2_vector_get_size(chunk.vector, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);

	// Many rows in one batch, each a small array, read back in bulk.
	REQUIRE(duckdb_v2_vector_set_size(chunk.vector, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	const idx_t count = 100;
	REQUIRE(duckdb_v2_variant_builder_create(chunk.vector, count, &b, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t row = 0; row < count; row++) {
		REQUIRE(duckdb_v2_variant_builder_begin_row(b, row, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_variant_builder_begin_object(b, 1, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(AppendKey(b, row % 2 ? "odd" : "even", nullptr));
		int64_t value = static_cast<int64_t>(row);
		REQUIRE(AppendScalar(b, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, &value, nullptr));
		REQUIRE(duckdb_v2_variant_builder_end_object(b, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_variant_builder_end_row(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_variant_builder_finish(b, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_variant_builder_destroy(&b);
	REQUIRE(duckdb_v2_vector_get_size(chunk.vector, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == count);
	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	for (idx_t row : {idx_t(0), idx_t(1), idx_t(99)}) {
		duckdb_v2_value_handle value = nullptr;
		REQUIRE(duckdb_v2_vector_get_value(chunk.vector, row, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_value_handle text = nullptr;
		REQUIRE(duckdb_v2_value_cast(fx.factory, value, varchar, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(ConsumeValue<std::string>(text) ==
		        std::string("{'") + (row % 2 ? "odd" : "even") + "': " + std::to_string(row) + "}");
		duckdb_v2_value_destroy(&value);
	}
	duckdb_v2_logical_type_destroy(&varchar);
}

} // namespace test_capi_v2
