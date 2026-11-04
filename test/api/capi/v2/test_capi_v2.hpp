#pragma once

//----------------------------------------------------------------------------------------------------------------------
// Common headers for all C-API-V2 tests
//----------------------------------------------------------------------------------------------------------------------

#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb_v2.h"

#include <string>
#include <cstring>

//----------------------------------------------------------------------------------------------------------------------
// String helpers (need to be in global namespace)
//----------------------------------------------------------------------------------------------------------------------

inline bool operator==(duckdb_v2_str a, const std::string &b) {
	return a.len == b.size() && (a.len == 0 || std::memcmp(a.ptr, b.data(), a.len) == 0);
}
inline bool operator==(const std::string &a, duckdb_v2_str b) {
	return b == a;
}
inline bool operator==(duckdb_v2_str a, const char *b) {
	return a == std::string(b ? b : "");
}
inline bool operator==(const char *a, duckdb_v2_str b) {
	return b == std::string(a ? a : "");
}
inline bool operator!=(duckdb_v2_str a, const std::string &b) {
	return !(a == b);
}
inline bool operator!=(const std::string &a, duckdb_v2_str b) {
	return !(b == a);
}
inline bool operator!=(duckdb_v2_str a, const char *b) {
	return !(a == b);
}
inline bool operator!=(const char *a, duckdb_v2_str b) {
	return !(b == a);
}

namespace test_capi_v2 {
//----------------------------------------------------------------------------------------------------------------------
// Common Fixtures
//----------------------------------------------------------------------------------------------------------------------

//! Creates an instance handle under `env`, attaches `path` and makes it the default database, destroying the handle
//! again if that fails.
inline DUCKDB_V2_ERROR OpenInstance(duckdb_v2_environment_handle env, duckdb_v2_str path,
                                    duckdb_v2_instance_handle *out_instance, duckdb_v2_error_info_handle *err) {
	auto rc = duckdb_v2_instance_create(env, out_instance, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	rc = duckdb_v2_instance_attach(*out_instance, nullptr, &path, nullptr, true, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_instance_destroy(out_instance);
	}
	return rc;
}

//! The factory of `conn`, borrowed.
inline duckdb_v2_factory_handle FactoryOf(duckdb_v2_connection_handle conn) {
	duckdb_v2_factory_handle factory = nullptr;
	REQUIRE(duckdb_v2_connection_get_factory(conn, &factory, nullptr) == DUCKDB_V2_ERROR_NONE);
	return factory;
}

struct EnvFixture {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	//! Borrowed from `conn`.
	duckdb_v2_factory_handle factory = nullptr;
	//! Borrowed from `conn`.
	duckdb_v2_config_handle config = nullptr;
	//! Borrowed from `instance`: the GLOBAL scope.
	duckdb_v2_config_handle instance_config = nullptr;
	EnvFixture() {
		duckdb_v2_environment_create(&env, nullptr);
		OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr);
		duckdb_v2_connection_create(instance, &conn, nullptr);
		duckdb_v2_connection_get_factory(conn, &factory, nullptr);
		duckdb_v2_connection_get_config(conn, &config, nullptr);
		duckdb_v2_instance_get_config(instance, &instance_config, nullptr);
	}
	~EnvFixture() {
		duckdb_v2_connection_destroy(&conn);
		duckdb_v2_instance_destroy(&instance);
		duckdb_v2_environment_destroy(&env);
	}
};

//----------------------------------------------------------------------------------------------------------------------
// V1/V2 Converters
//----------------------------------------------------------------------------------------------------------------------

//! Consumes a V1 logical type, returning an owned V2 handle to the same type. A V1 handle points to a LogicalType, a
//! V2 handle to the type's LogicalTypeInfo (holding one reference).
inline auto ConvertToV2(duckdb_logical_type t) -> duckdb_v2_logical_type_handle {
	auto type = *reinterpret_cast<duckdb::LogicalType *>(t);
	duckdb_destroy_logical_type(&t);
	auto &type_info = std::move(type).ReleaseTypeInfo();
	return reinterpret_cast<duckdb_v2_logical_type_handle>(reinterpret_cast<uintptr_t>(&type_info));
}
//! The engine type behind a V2 handle - a copy, the handle keeps its own reference
inline auto V2LogicalType(duckdb_v2_logical_type_handle t) -> duckdb::LogicalType {
	auto borrowed = duckdb::LogicalType::AdoptTypeInfo(*reinterpret_cast<const duckdb::LogicalTypeInfo *>(t));
	duckdb::LogicalType result = borrowed;
	std::move(borrowed).ReleaseTypeInfo();
	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// Error Helpers
//----------------------------------------------------------------------------------------------------------------------

// Helper to create an error info handle out of thin air
DUCKDB_V2_ERROR SetErrorInfo(duckdb_v2_error_info_handle *err, DUCKDB_V2_ERROR code, const char *msg);

//----------------------------------------------------------------------------------------------------------------------
// String Helpers
//----------------------------------------------------------------------------------------------------------------------

// Build a borrowed string view from a null-terminated C string. A null pointer yields the empty view {NULL, 0}.
inline auto Convert(const char *s) -> duckdb_v2_str {
	return duckdb_v2_str {s, s ? std::strlen(s) : 0};
}

inline auto Convert(const std::string &s) -> duckdb_v2_str {
	return duckdb_v2_str {s.data(), s.size()};
}

// Materialize a borrowed view as a std::string for comparison/printing.
inline auto Convert(duckdb_v2_str s) -> std::string {
	return s.ptr ? std::string(s.ptr, s.len) : std::string();
}

inline auto Convert(const duckdb_v2_bytes &s) -> duckdb_v2_str {
#ifdef DUCKDB_DEBUG_NO_INLINE
	uint32_t len = s.value.pointer.length;
	const char *ptr = s.value.pointer.ptr;
	return duckdb_v2_str {ptr, len};
#else
	uint32_t len = s.value.inlined.length;
	const char *ptr = len <= DUCKDB_V2_BYTES_INLINE_LENGTH ? s.value.inlined.inlined : s.value.pointer.ptr;
	return duckdb_v2_str {ptr, len};
#endif
}

//----------------------------------------------------------------------------------------------------------------------
// Caller-buffer text getters
//----------------------------------------------------------------------------------------------------------------------

// Runs the two-call text protocol: size with a null buffer, then fill one that
// leaves room for the terminator. `rc` gets the result of whichever call failed.
template <class CALL>
inline std::string RenderText(CALL call, DUCKDB_V2_ERROR &rc) {
	idx_t len = 0;
	rc = call(nullptr, 0, &len);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return std::string();
	}
	std::vector<char> buf(len + 1, '\xff');
	rc = call(buf.data(), buf.size(), &len);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return std::string();
	}
	// The protocol promises a terminator the caller can rely on.
	REQUIRE(buf[len] == '\0');
	return std::string(buf.data(), len);
}

inline std::string Render(duckdb_v2_value_handle value) {
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto out = RenderText(
	    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_value_to_string(value, buf, cap, len, nullptr); }, rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

inline std::string Render(duckdb_v2_logical_type_handle type) {
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto out = RenderText(
	    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_logical_type_to_text(type, buf, cap, len, nullptr); },
	    rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

// A duckdb_v2_text_sink_fn that copies the text into a std::string, and counts
// its invocations so tests can pin the exactly-once contract.
struct TextSinkTarget {
	std::string text;
	idx_t calls = 0;
};
inline void AppendToString(const duckdb_v2_str *text, void *user_data, duckdb_v2_error_info_handle *) {
	auto &target = *static_cast<TextSinkTarget *>(user_data);
	target.calls++;
	if (text->ptr && text->len) {
		target.text.append(text->ptr, text->len);
	}
}

// Fails the producing call by populating the slot the library handed over.
inline void FailWithIOError(const duckdb_v2_str *, void *, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("sink could not write");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// Assemble a duckdb_v2_bytes from raw bytes, using arena_allocate for the
// non-inlined path. Mirrors the C++ StringHeap::Add. `rc` gets the allocate result.
inline duckdb_v2_bytes MakeString(duckdb_v2_arena_handle heap, const char *data, idx_t len, DUCKDB_V2_ERROR &rc,
                                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_bytes storage {};
	rc = DUCKDB_V2_ERROR_NONE;
#ifdef DUCKDB_DEBUG_NO_INLINE
	if (len == 0 || data == nullptr) {
		storage.value.pointer.length = 0;
		storage.value.pointer.ptr = nullptr;
		return storage;
	}
#else
	if (len <= DUCKDB_V2_BYTES_INLINE_LENGTH) {
		storage.value.inlined.length = static_cast<uint32_t>(len);
		if (len > 0) {
			std::memcpy(storage.value.inlined.inlined, data, len);
		}
		return storage;
	}
#endif

	uint8_t *bytes = nullptr;
	rc = duckdb_v2_arena_allocate(heap, len, &bytes, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return storage;
	}
	D_ASSERT(bytes != nullptr);
	D_ASSERT(data != nullptr);
	std::memcpy(bytes, data, len);

	storage.value.pointer.length = static_cast<uint32_t>(len);
	storage.value.pointer.ptr = reinterpret_cast<char *>(bytes);
	std::memcpy(storage.value.pointer.prefix, bytes, 4);

	return storage;
}

//----------------------------------------------------------------------------------------------------------------------
// Query Helpers
//----------------------------------------------------------------------------------------------------------------------

struct QueryResult {
	duckdb_v2_result_handle handle = nullptr;
	QueryResult() = default;
	QueryResult(const QueryResult &) = delete;
	QueryResult &operator=(const QueryResult &) = delete;
	~QueryResult() {
		duckdb_v2_result_destroy(&handle);
	}
	duckdb_v2_result_handle *operator&() {
		return &handle;
	}
	operator duckdb_v2_result_handle() const {
		return handle;
	}
};

// A result's output column count, read through its schema.
inline idx_t ColumnCount(duckdb_v2_result_handle r) {
	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t count = 0;
	REQUIRE(duckdb_v2_schema_get_count(schema, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_schema_destroy(&schema);
	return count;
}

// Asserts result column `index` has the given name and type id, via the result schema
inline void RequireColumn(duckdb_v2_result_handle r, idx_t index, const char *name, DUCKDB_V2_LOGICAL_TYPE_ID id) {
	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_str field_name = {nullptr, 0};
	duckdb_v2_logical_type_handle field_type = nullptr; // borrowed; do not destroy
	REQUIRE(duckdb_v2_schema_get_field(schema, index, &field_name, &field_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(field_name.ptr ? field_name.ptr : "", field_name.len) == name);
	DUCKDB_V2_LOGICAL_TYPE_ID got = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(field_type, &got, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(got == id);
	duckdb_v2_schema_destroy(&schema);
}

// Asserts a result's schema is not yet available: the statement expands into a group whose
// row-producing statement is not bound yet.
inline void RequireSchemaDeferred(duckdb_v2_result_handle r) {
	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_result_get_schema(r, &schema, nullptr) == DUCKDB_V2_ERROR_INPUT_METADATA_DEFERRED);
	REQUIRE(schema == nullptr);
}

// Perform a query, returning the result handle
inline DUCKDB_V2_ERROR Query(duckdb_v2_connection_handle conn, const char *sql, duckdb_v2_result_handle *out_result,
                             duckdb_v2_error_info_handle *err = nullptr) {
	if (out_result) {
		*out_result = nullptr;
	}
	duckdb_v2_statement_iterator_handle iter = nullptr;
	auto rc = duckdb_v2_parse_sql(conn, sql, true, &iter, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	duckdb_v2_sql_statement_handle stmt = nullptr;
	rc = duckdb_v2_statement_iterator_next(iter, &stmt, err);
	if (rc == DUCKDB_V2_ERROR_NONE && stmt) {
		// Guard single-statement intent before running anything.
		// Destroy the fixtures before any REQUIRE/FAIL so an assertion failure cannot leak.
		duckdb_v2_sql_statement_handle extra = nullptr;
		auto extra_rc = duckdb_v2_statement_iterator_next(iter, &extra, nullptr);
		if (extra_rc != DUCKDB_V2_ERROR_NONE || extra) {
			duckdb_v2_sql_statement_destroy(&extra);
			duckdb_v2_sql_statement_destroy(&stmt);
			duckdb_v2_statement_iterator_destroy(&iter);
			REQUIRE(extra_rc == DUCKDB_V2_ERROR_NONE);
			FAIL("V2Query input contains more than one statement: " << sql);
		}
	}
	if (rc == DUCKDB_V2_ERROR_NONE) {
		rc = duckdb_v2_statement_execute(conn, stmt, nullptr, nullptr, 0, out_result, err);
	}
	// The statement is always still alive (it executes a copy), so destroy it unconditionally.
	duckdb_v2_sql_statement_destroy(&stmt);
	duckdb_v2_statement_iterator_destroy(&iter);
	return rc;
}

// Runs a single statement to exhaustion, returning the code it failed with. Execution is lazy, so a failure may only
// show up while stepping; nothing is asserted, so it also suits statements that are expected to fail.
inline DUCKDB_V2_ERROR RunSql(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	auto rc = Query(conn, sql, &result);
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED ||
		    status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	duckdb_v2_result_destroy(&result);
	return rc;
}

// Drains the next chunk out of a streaming result, returning a caller-owned chunk, or nullptr at end-of-stream.
//
// The WAITING poll runs a timing-dependent number of rounds, so it asserts
// nothing: a per-round REQUIRE would make the suite's assertion count differ
// between runs. Failures are latched and checked once on the way out, which
// costs the same four assertions per call no matter how long the poll ran.
inline duckdb_v2_data_chunk_handle StepChunk(duckdb_v2_result_handle r) {
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto step_rc = DUCKDB_V2_ERROR_NONE;
	auto wait_rc = DUCKDB_V2_ERROR_NONE;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	bool cancelled = false;
	bool chunk_matches_status = true;

	while (true) {
		chunk = nullptr;
		step_rc = duckdb_v2_result_step(r, &chunk, &status, nullptr);
		if (step_rc != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK) {
			chunk_matches_status = (chunk != nullptr);
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			chunk_matches_status = (chunk == nullptr);
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			cancelled = true;
			break;
		}
		wait_rc = duckdb_v2_result_wait(r, nullptr);
		if (wait_rc != DUCKDB_V2_ERROR_NONE) {
			break;
		}
	}

	REQUIRE(step_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(wait_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(cancelled); // unexpected CANCELLED status while draining a result
	REQUIRE(chunk_matches_status);
	return status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK ? chunk : nullptr;
}

// Drains a result to exhaustion via the step primitive, destroying each chunk, returning the total row count.
inline idx_t DrainRowCount(duckdb_v2_result_handle r) {
	idx_t total = 0;
	while (auto chunk = StepChunk(r)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_data_chunk_destroy(&chunk);
		total += size;
	}
	return total;
}

// Reads the affected-row count of a CHANGED_ROWS result by draining its single-row BIGINT Count chunk.
// Returns -1 if the stream yields no chunk.
inline int64_t DrainChangedRows(duckdb_v2_result_handle r) {
	auto chunk = StepChunk(r);
	if (!chunk) {
		return -1;
	}
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	int64_t count = view.data ? reinterpret_cast<const int64_t *>(view.data)[0] : -1;
	duckdb_v2_data_chunk_destroy(&chunk);
	// The Count chunk is the stream's only payload; pin end-of-stream.
	auto trailing = StepChunk(r);
	if (trailing) {
		duckdb_v2_data_chunk_destroy(&trailing);
		FAIL("CHANGED_ROWS stream yielded more than one chunk");
	}
	return count;
}

// Steps a result until it reports CANCELLED, destroying any chunk handed over
// on the way. How many rounds that takes is timing-dependent, so the loop
// asserts nothing; one assertion per call, and the caller checks the returned
// status. `out_saw_chunk` reports whether any chunk arrived before the cancel.
inline DUCKDB_V2_RESULT_STEP_STATUS StepUntilCancelled(duckdb_v2_result_handle r, bool *out_saw_chunk = nullptr) {
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	auto step_rc = DUCKDB_V2_ERROR_NONE;
	bool saw_chunk = false;
	for (int i = 0; i < 1000 && status != DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED; i++) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		step_rc = duckdb_v2_result_step(r, &chunk, &status, nullptr);
		saw_chunk = saw_chunk || status == DUCKDB_V2_RESULT_STEP_STATUS_CHUNK;
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (step_rc != DUCKDB_V2_ERROR_NONE) {
			break;
		}
	}
	if (out_saw_chunk) {
		*out_saw_chunk = saw_chunk;
	}
	REQUIRE(step_rc == DUCKDB_V2_ERROR_NONE);
	return status;
}

// Executes a side-effecting statement (DDL, DML, SET, ...) to completion (query + drain + destroy).
// Streaming execution is lazy, so a statement only takes effect once its result is stepped.
inline void ExecSQL(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(conn, sql, &r) == DUCKDB_V2_ERROR_NONE);
	DrainRowCount(r);
	duckdb_v2_result_destroy(&r);
}

// Reads one consistent progress snapshot.
struct QueryProgress {
	double percentage = 99.0;
	uint64_t rows_processed = 99;
	uint64_t total_rows_to_process = 99;
};
// Pass out_ok from a polling loop: the call then asserts nothing and reports
// success through the flag, so a timing-dependent round count cannot move the
// suite's assertion total. The caller latches the flag and asserts once.
inline QueryProgress ReadProgress(duckdb_v2_connection_handle conn, bool *out_ok) {
	QueryProgress out;
	auto rc = duckdb_v2_connection_progress_get(conn, &out.percentage, &out.rows_processed, &out.total_rows_to_process,
	                                            nullptr);
	*out_ok = rc == DUCKDB_V2_ERROR_NONE;
	return out;
}

inline QueryProgress ReadProgress(duckdb_v2_connection_handle conn) {
	bool ok = false;
	auto out = ReadProgress(conn, &ok);
	REQUIRE(ok);
	return out;
}

//----------------------------------------------------------------------------------------------------------------------
// Vector Helpers
//----------------------------------------------------------------------------------------------------------------------

// Resolve a logical row through a selection vector
inline idx_t SelAt(const duckdb_v2_sel_t *sel, idx_t i) {
	return sel ? static_cast<idx_t>(sel[i]) : i;
}

// Check if a vector view's row is valid
inline bool RowValid(const duckdb_v2_vector_view &view, idx_t idx) {
	if (!view.validity) {
		return true;
	}
	return (view.validity[idx / 64] & (uint64_t(1) << (idx % 64))) != 0;
}

inline DUCKDB_V2_ERROR V2VectorAssignString(duckdb_v2_vector_handle vec, idx_t index, const char *data, idx_t len,
                                            duckdb_v2_error_info_handle *err) {
	duckdb_v2_arena_handle heap = nullptr;
	auto rc = duckdb_v2_vector_get_arena(vec, &heap, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	auto storage = MakeString(heap, data, len, rc, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	void *raw = nullptr;
	rc = duckdb_v2_vector_get_data_mutable(vec, &raw, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	static_cast<duckdb_v2_bytes *>(raw)[index] = storage;
	return DUCKDB_V2_ERROR_NONE;
}

//----------------------------------------------------------------------------------------------------------------------
// Value Helpers
//----------------------------------------------------------------------------------------------------------------------

// The typed constructors, through the context of the connection the tests hold.
inline duckdb_v2_value_handle MakeBoolValue(duckdb_v2_factory_handle factory, bool payload) {
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_value_create_bool(factory, payload, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	return value;
}
inline duckdb_v2_value_handle MakeInt32Value(duckdb_v2_factory_handle factory, int32_t payload) {
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_value_create_int(factory, payload, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	return value;
}
inline duckdb_v2_value_handle MakeInt64Value(duckdb_v2_factory_handle factory, int64_t payload) {
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_value_create_bigint(factory, payload, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	return value;
}
inline duckdb_v2_value_handle MakeVarcharValue(duckdb_v2_factory_handle factory, const char *s) {
	duckdb_v2_value_handle value = nullptr;
	auto value_str = Convert(s);
	REQUIRE(duckdb_v2_value_create_varchar(factory, &value_str, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	return value;
}
inline duckdb_v2_value_handle MakeBlobValue(duckdb_v2_factory_handle factory, const void *data, idx_t len) {
	duckdb_v2_value_handle value = nullptr;
	duckdb_v2_str bytes = {static_cast<const char *>(data), len};
	REQUIRE(duckdb_v2_value_create_blob(factory, &bytes, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	return value;
}

// The kinds outside the typed set — DATE, the TIME / TIMESTAMP variants,
// INTERVAL, UUID, DECIMAL, ENUM — are built the way the API intends: a VARCHAR
// through the cast machinery.
inline duckdb_v2_value_handle MakeValueFromText(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle type,
                                                const char *text) {
	auto varchar = MakeVarcharValue(factory, text);
	duckdb_v2_value_handle value = nullptr;
	auto rc = duckdb_v2_value_cast(factory, varchar, type, &value, nullptr);
	duckdb_v2_value_destroy(&varchar);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(value != nullptr);
	return value;
}

inline duckdb_v2_value_handle MakeValueFromText(duckdb_v2_factory_handle factory, DUCKDB_V2_LOGICAL_TYPE_ID id,
                                                const char *text) {
	duckdb_v2_logical_type_handle type = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(factory, id, nullptr, nullptr, 0, &type, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	auto value = MakeValueFromText(factory, type, text);
	duckdb_v2_logical_type_destroy(&type);
	return value;
}

// One overload per typed getter; the out-param type picks which one runs.
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, bool *out) {
	return duckdb_v2_value_get_bool(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, int8_t *out) {
	return duckdb_v2_value_get_tinyint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, int16_t *out) {
	return duckdb_v2_value_get_smallint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, int32_t *out) {
	return duckdb_v2_value_get_int(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, int64_t *out) {
	return duckdb_v2_value_get_bigint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, duckdb_v2_hugeint_t *out) {
	return duckdb_v2_value_get_hugeint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, uint8_t *out) {
	return duckdb_v2_value_get_utinyint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, uint16_t *out) {
	return duckdb_v2_value_get_usmallint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, uint32_t *out) {
	return duckdb_v2_value_get_uint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, uint64_t *out) {
	return duckdb_v2_value_get_ubigint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, duckdb_v2_uhugeint_t *out) {
	return duckdb_v2_value_get_uhugeint(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, float *out) {
	return duckdb_v2_value_get_float(v, out, nullptr);
}
inline DUCKDB_V2_ERROR GetTypedValue(duckdb_v2_value_handle v, double *out) {
	return duckdb_v2_value_get_double(v, out, nullptr);
}

// Consuming forms: read, destroy the owned value, then assert, so a failing REQUIRE cannot leak it.
template <class T>
inline T ConsumeValue(duckdb_v2_value_handle &value) {
	T out {};
	auto rc = GetTypedValue(value, &out);
	duckdb_v2_value_destroy(&value);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

// VARCHAR: the borrowed string is copied out before the value goes away.
template <>
inline std::string ConsumeValue(duckdb_v2_value_handle &value) {
	duckdb_v2_str str = {nullptr, 0};
	auto rc = duckdb_v2_value_get_varchar(value, &str, nullptr);
	std::string out = rc == DUCKDB_V2_ERROR_NONE ? Convert(str) : std::string();
	duckdb_v2_value_destroy(&value);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

// Same, through the byte-string getter: BLOB, BIT and BIGNUM storage.
inline std::string ConsumeBlob(duckdb_v2_value_handle &value) {
	duckdb_v2_str str = {nullptr, 0};
	auto rc = duckdb_v2_value_get_blob(value, &str, nullptr);
	std::string out = rc == DUCKDB_V2_ERROR_NONE ? Convert(str) : std::string();
	duckdb_v2_value_destroy(&value);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

//----------------------------------------------------------------------------------------------------------------------
// Type Helpers
//----------------------------------------------------------------------------------------------------------------------

inline duckdb_v2_logical_type_handle MakeType(duckdb_v2_factory_handle factory, DUCKDB_V2_LOGICAL_TYPE_ID id) {
	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(factory, id, nullptr, nullptr, 0, &t, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	return t;
}

inline duckdb_v2_value_handle MakeTypeValue(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle t) {
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_type(factory, t, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	return v;
}

inline duckdb_v2_value_handle MakeTypeValue(duckdb_v2_factory_handle factory, DUCKDB_V2_LOGICAL_TYPE_ID id) {
	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_factory_create_type_from_id(factory, id, nullptr, nullptr, 0, &t, nullptr);
	auto v = MakeTypeValue(factory, t);
	duckdb_v2_logical_type_destroy(&t);
	return v;
}

inline duckdb_v2_logical_type_handle MakeType(duckdb_v2_factory_handle factory, const char *name,
                                              const std::vector<const char *> *names,
                                              std::vector<duckdb_v2_value_handle> values) {
	std::vector<duckdb_v2_str> name_views;
	if (names) {
		for (auto *n : *names) {
			name_views.push_back(Convert(n));
		}
	}
	// The type name is a qualified name; an unqualified one is a single part.
	duckdb_v2_identifier_t parts[1] = {Convert(name)};
	duckdb_v2_qname_handle qname = nullptr;
	DUCKDB_V2_ERROR rc = duckdb_v2_qname_create(parts, 1, &qname, nullptr);
	duckdb_v2_logical_type_handle t = nullptr;
	if (rc == DUCKDB_V2_ERROR_NONE) {
		rc = duckdb_v2_factory_create_type_from_name(factory, qname, names ? name_views.data() : nullptr,
		                                             values.empty() ? nullptr : values.data(), values.size(), &t,
		                                             nullptr);
	}
	duckdb_v2_qname_destroy(&qname);
	for (auto &v : values) {
		duckdb_v2_value_destroy(&v);
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(t != nullptr);
	return t;
}

inline duckdb_v2_logical_type_handle MakeListType(duckdb_v2_factory_handle factory, DUCKDB_V2_LOGICAL_TYPE_ID elem) {
	return MakeType(factory, "list", nullptr, {MakeTypeValue(factory, elem)});
}
inline duckdb_v2_logical_type_handle MakeMapType(duckdb_v2_factory_handle factory, DUCKDB_V2_LOGICAL_TYPE_ID key,
                                                 DUCKDB_V2_LOGICAL_TYPE_ID value) {
	return MakeType(factory, "map", nullptr, {MakeTypeValue(factory, key), MakeTypeValue(factory, value)});
}

inline duckdb_v2_logical_type_handle MakeStructType(duckdb_v2_factory_handle factory,
                                                    const std::vector<const char *> &names,
                                                    const std::vector<DUCKDB_V2_LOGICAL_TYPE_ID> &ids) {
	std::vector<duckdb_v2_value_handle> values;
	for (auto id : ids) {
		values.push_back(MakeTypeValue(factory, id));
	}
	return MakeType(factory, "struct", &names, std::move(values));
}

} // namespace test_capi_v2
