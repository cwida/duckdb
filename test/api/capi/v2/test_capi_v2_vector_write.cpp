#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// V2 vector write API tests.
//
// Type fixtures are V2-native: primitives via MakeType, composites via the
// context-scoped MakeType sugar (MakeListType / MakeStructType / MakeMapType),
// each test carrying its own EnvFixture. Intermediates are destroyed
// before any REQUIRE to avoid leaks on Catch2 assertion failure.
//
// There is no chunk-level size setter: each vector carries its own logical
// size, set via vector_set_size (the child of a LIST/MAP is sized the same
// way, through its borrowed handle). data_chunk_get_size remains for the
// read path (chunks fetched from a result), where the engine sets the
// cardinality; a manually-built write chunk reports 0 there.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// data_chunk_create
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

TEST_CASE("V2: UTF-8 validation reports input errors", "[capi_v2][vector_write]") {
	duckdb_v2_error_info_handle error = nullptr;
	duckdb_v2_str text_str = {nullptr, 0};
	REQUIRE(duckdb_v2_validate_utf8(&text_str, &error) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(error == nullptr);
	auto text_str2 = Convert("🦆");
	REQUIRE(duckdb_v2_validate_utf8(&text_str2, &error) == DUCKDB_V2_ERROR_NONE);
	const std::string embedded_nul("a\0b", 3);
	auto embedded_nul_str = Convert(embedded_nul);
	REQUIRE(duckdb_v2_validate_utf8(&embedded_nul_str, &error) == DUCKDB_V2_ERROR_NONE);
	auto text_str3 = Convert("\xFF");
	REQUIRE(duckdb_v2_validate_utf8(&text_str3, &error) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(error != nullptr);
	duckdb_v2_error_info_destroy(&error);
	duckdb_v2_str text_str4 = {nullptr, 1};
	REQUIRE(duckdb_v2_validate_utf8(&text_str4, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_validate_utf8(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

// These tests almost always assume a vector is 2048 rows to write into, so only run them if that is the case
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)

TEST_CASE("V2: data_chunk_create basic", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto varchar_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_handle types[2] = {int_type, varchar_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 2, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_logical_type_destroy(&varchar_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk != nullptr);

	idx_t vec_count = 0;
	REQUIRE(duckdb_v2_data_chunk_get_vector_count(chunk, &vec_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vec_count == 2);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vec != nullptr);

	// A fresh vector starts empty; sizing is per-vector.
	idx_t size = 99;
	REQUIRE(duckdb_v2_vector_get_size(vec, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);

	REQUIRE(duckdb_v2_vector_set_size(vec, 10, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_size(vec, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 10);

	DUCKDB_V2_VECTOR_TYPE vtype = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &vtype, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vtype == DUCKDB_V2_VECTOR_TYPE_FLAT);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk == nullptr);
}

TEST_CASE("V2: data_chunk_create null args", "[capi_v2][vector_write]") {
	EnvFixture fx;
	duckdb_v2_data_chunk_handle chunk = nullptr;

	// Null types array — out_chunk should be zeroed.
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, nullptr, 2, &chunk, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(chunk == nullptr);

	// Null out_chunk.
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, nullptr, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: data_chunk_create with null element in types", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[2] = {int_type, nullptr};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 2, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(chunk == nullptr);
}

// ---------------------------------------------------------------------------
// vector_set_size / vector_get_size
// ---------------------------------------------------------------------------

TEST_CASE("V2: vector_set_size null arg", "[capi_v2][vector_write]") {
	REQUIRE(duckdb_v2_vector_set_size(nullptr, 10, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: per-column sizing", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto double_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE);
	duckdb_v2_logical_type_handle types[2] = {int_type, double_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 2, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_logical_type_destroy(&double_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	for (idx_t col = 0; col < 2; col++) {
		duckdb_v2_vector_handle vec = nullptr;
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, col, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_vector_set_size(vec, 42, nullptr) == DUCKDB_V2_ERROR_NONE);
		idx_t size = 0;
		REQUIRE(duckdb_v2_vector_get_size(vec, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(size == 42);
	}

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// vector_set_size beyond the default capacity must auto-reserve.
TEST_CASE("V2: vector_set_size auto-reserves", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_set_size(vec, 5000, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t size = 0;
	REQUIRE(duckdb_v2_vector_get_size(vec, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 5000);

	// Writing up to the new size must not crash.
	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<int32_t *>(raw)[4999] = 42;

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// vector_reference
// ---------------------------------------------------------------------------

TEST_CASE("V2: vector_reference rejects a type mismatch", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto bigint_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_logical_type_handle types[2] = {int_type, bigint_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 2, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_logical_type_destroy(&bigint_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle int_vec = nullptr;
	duckdb_v2_vector_handle bigint_vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &int_vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 1, &bigint_vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(int_vec, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(bigint_vec, 5, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_reference(nullptr, int_vec, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_reference(int_vec, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_reference(bigint_vec, int_vec, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);
	REQUIRE(duckdb_v2_error_info_destroy(&err) == DUCKDB_V2_ERROR_NONE);

	// The rejected call left the vector untouched.
	idx_t size = 0;
	REQUIRE(duckdb_v2_vector_get_size(bigint_vec, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 5);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// vector_flatten / vector_make_constant / vector_make_sequence
// ---------------------------------------------------------------------------

TEST_CASE("V2: vector_make_constant from value", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle value = MakeInt32Value(fx.factory, 42);
	REQUIRE(duckdb_v2_vector_make_constant(vec, value, 5, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_destroy(&value) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_VECTOR_TYPE vtype = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &vtype, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vtype == DUCKDB_V2_VECTOR_TYPE_CONSTANT);

	// make_constant fills the vector from the value across all logical rows.
	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.count == 5);
	REQUIRE(view.sel != nullptr);
	const auto *rdata = static_cast<const int32_t *>(view.data);
	REQUIRE(rdata[SelAt(view.sel, 0)] == 42);
	REQUIRE(rdata[SelAt(view.sel, 4)] == 42);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_flatten resets constant", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle value = MakeInt32Value(fx.factory, 7);
	REQUIRE(duckdb_v2_vector_make_constant(vec, value, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_destroy(&value) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_flatten(vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_VECTOR_TYPE vtype = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &vtype, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vtype == DUCKDB_V2_VECTOR_TYPE_FLAT);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel == nullptr);
	// Flattening preserves the logical contents.
	const auto *rdata = static_cast<const int32_t *>(view.data);
	REQUIRE(rdata[0] == 7);
	REQUIRE(rdata[2] == 7);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_make_sequence", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto bigint_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_logical_type_handle types[1] = {bigint_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&bigint_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_make_sequence(vec, 10, 3, 4, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_flatten(vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.count == 4);

	const auto *data = static_cast<const int64_t *>(view.data);
	REQUIRE(data[0] == 10);
	REQUIRE(data[1] == 13);
	REQUIRE(data[2] == 16);
	REQUIRE(data[3] == 19);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_make_* null args", "[capi_v2][vector_write]") {
	REQUIRE(duckdb_v2_vector_flatten(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_make_constant(nullptr, nullptr, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_make_sequence(nullptr, 0, 1, 10, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: vector_make_constant null value", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	// A non-null vector with a null value must be rejected.
	REQUIRE(duckdb_v2_vector_make_constant(vec, nullptr, 5, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// vector_flat_get_validity_mutable
// ---------------------------------------------------------------------------

TEST_CASE("V2: vector_flat_get_validity_mutable + set nulls", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 4, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *data = static_cast<int32_t *>(raw);
	data[0] = 10;
	data[1] = 20;
	data[2] = 30;
	data[3] = 40;

	uint64_t *validity = nullptr;
	REQUIRE(duckdb_v2_vector_flat_get_validity_mutable(vec, &validity, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(validity != nullptr);

	validity[0] &= ~(UINT64_C(1) << 1);
	validity[0] &= ~(UINT64_C(1) << 3);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.validity != nullptr);

	REQUIRE(RowValid(view, 0));
	REQUIRE_FALSE(RowValid(view, 1));
	REQUIRE(RowValid(view, 2));
	REQUIRE_FALSE(RowValid(view, 3));

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_flat_get_validity_mutable null args", "[capi_v2][vector_write]") {
	uint64_t *validity = nullptr;
	REQUIRE(duckdb_v2_vector_flat_get_validity_mutable(nullptr, &validity, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_flat_get_validity_mutable(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: vector_flat_get_validity_mutable rejects SEQUENCE vector", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto i64_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_logical_type_handle types[1] = {i64_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&i64_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_make_sequence(vec, 0, 1, 10, nullptr) == DUCKDB_V2_ERROR_NONE);

	uint64_t *validity = nullptr;
	REQUIRE(duckdb_v2_vector_flat_get_validity_mutable(vec, &validity, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// vector_constant_set_valid
// ---------------------------------------------------------------------------
TEST_CASE("V2: vector_constant_set_valid toggles validity", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle value = MakeInt32Value(fx.factory, 77);
	REQUIRE(duckdb_v2_vector_make_constant(vec, value, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_destroy(&value) == DUCKDB_V2_ERROR_NONE);

	// Mark the single constant element NULL — every logical row reads NULL.
	REQUIRE(duckdb_v2_vector_constant_set_valid(vec, false, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel != nullptr);
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE_FALSE(RowValid(view, SelAt(view.sel, i)));
	}

	// Flip it back to valid.
	REQUIRE(duckdb_v2_vector_constant_set_valid(vec, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RowValid(view, SelAt(view.sel, 0)));

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_constant_set_valid rejects FLAT vector", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	// FLAT (the default) is not a constant vector.
	REQUIRE(duckdb_v2_vector_constant_set_valid(vec, false, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_constant_set_valid(nullptr, false, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// vector_set_null: the recursive NULL write path (nested NULL invariant)
// ---------------------------------------------------------------------------

TEST_CASE("V2: vector_set_null on a primitive vector", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < 3; i++) {
		static_cast<int32_t *>(raw)[i] = static_cast<int32_t>(i + 1);
	}

	REQUIRE(duckdb_v2_vector_set_null(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RowValid(view, 0));
	REQUIRE_FALSE(RowValid(view, 1));
	REQUIRE(RowValid(view, 2));
	REQUIRE(static_cast<const int32_t *>(view.data)[0] == 1);
	REQUIRE(static_cast<const int32_t *>(view.data)[2] == 3);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_set_null recurses into STRUCT fields", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto struct_type =
	    MakeStructType(fx.factory, {"a", "b"}, {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR});
	duckdb_v2_logical_type_handle types[1] = {struct_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&struct_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle field_a = nullptr;
	duckdb_v2_vector_handle field_b = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &field_a, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(vec, 1, &field_b, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *a_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(field_a, &a_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < 3; i++) {
		static_cast<int32_t *>(a_raw)[i] = static_cast<int32_t>(i * 10);
		REQUIRE(V2VectorAssignString(field_b, i, "val", 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	}

	REQUIRE(duckdb_v2_vector_set_null(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view parent {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &parent, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RowValid(parent, 0));
	REQUIRE_FALSE(RowValid(parent, 1));
	REQUIRE(RowValid(parent, 2));

	// Both field slots under the NULL row are NULL; other rows keep their values.
	duckdb_v2_vector_view view_a {};
	duckdb_v2_vector_view view_b {};
	REQUIRE(duckdb_v2_vector_get_view(field_a, &view_a, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_view(field_b, &view_b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(RowValid(view_a, 1));
	REQUIRE_FALSE(RowValid(view_b, 1));
	REQUIRE(RowValid(view_a, 0));
	REQUIRE(RowValid(view_b, 2));
	REQUIRE(static_cast<const int32_t *>(view_a.data)[0] == 0);
	REQUIRE(static_cast<const int32_t *>(view_a.data)[2] == 20);
	REQUIRE(Convert(static_cast<const duckdb_v2_varchar_t *>(view_b.data)[2]) == "val");

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_set_null strides ARRAY elements", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto array_type =
	    MakeType(fx.factory, "array", nullptr,
	             {MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR), MakeInt32Value(fx.factory, 3)});
	duckdb_v2_logical_type_handle types[1] = {array_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&array_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle elems = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &elems, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(elems, 6, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < 6; i++) {
		REQUIRE(V2VectorAssignString(elems, i, "elem", 4, nullptr) == DUCKDB_V2_ERROR_NONE);
	}

	REQUIRE(duckdb_v2_vector_set_null(vec, 0, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view parent {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &parent, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(RowValid(parent, 0));
	REQUIRE(RowValid(parent, 1));

	// Row 0 covers element slots [0, 3); row 1's slots [3, 6) stay valid.
	duckdb_v2_vector_view child {};
	REQUIRE(duckdb_v2_vector_get_view(elems, &child, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE_FALSE(RowValid(child, i));
	}
	for (idx_t i = 3; i < 6; i++) {
		REQUIRE(RowValid(child, i));
		REQUIRE(Convert(static_cast<const duckdb_v2_varchar_t *>(child.data)[i]) == "elem");
	}

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_set_null reaches grandchildren through nested STRUCT", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto inner_type = MakeStructType(fx.factory, {"v"}, {DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR});
	std::vector<const char *> outer_names = {"inner"};
	auto outer_type = MakeType(fx.factory, "struct", &outer_names, {MakeTypeValue(fx.factory, inner_type)});
	duckdb_v2_logical_type_destroy(&inner_type);
	duckdb_v2_logical_type_handle types[1] = {outer_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&outer_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle inner = nullptr;
	duckdb_v2_vector_handle leaf = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &inner, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(inner, 0, &leaf, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2VectorAssignString(leaf, 0, "zero", 4, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2VectorAssignString(leaf, 1, "one", 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_set_null(vec, 0, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view inner_view {};
	duckdb_v2_vector_view leaf_view {};
	REQUIRE(duckdb_v2_vector_get_view(inner, &inner_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_view(leaf, &leaf_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(RowValid(inner_view, 0));
	REQUIRE_FALSE(RowValid(leaf_view, 0));
	REQUIRE(RowValid(inner_view, 1));
	REQUIRE(RowValid(leaf_view, 1));

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_set_null leaves LIST children untouched", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {list_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&list_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle elems = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &elems, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(elems, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	void *child_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(elems, &child_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < 3; i++) {
		static_cast<int32_t *>(child_raw)[i] = static_cast<int32_t>(i);
	}
	void *parent_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &parent_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *entries = static_cast<duckdb_v2_list_entry *>(parent_raw);
	entries[0] = {0, 2};
	entries[1] = {2, 1};

	REQUIRE(duckdb_v2_vector_set_null(vec, 0, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view parent {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &parent, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(RowValid(parent, 0));
	REQUIRE(RowValid(parent, 1));

	// LIST is exempt from the invariant: element slots stay valid.
	duckdb_v2_vector_view child {};
	REQUIRE(duckdb_v2_vector_get_view(elems, &child, nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE(RowValid(child, i));
	}

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector_set_null argument validation", "[capi_v2][vector_write]") {
	EnvFixture fx;
	REQUIRE(duckdb_v2_vector_set_null(nullptr, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Row index is bounds-checked against the logical size.
	REQUIRE(duckdb_v2_vector_set_null(vec, 2, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Non-FLAT representations are rejected.
	REQUIRE(duckdb_v2_vector_make_sequence(vec, 1, 1, 2, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_null(vec, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// LIST child management via vector_set_size / vector_get_size on the child
// ---------------------------------------------------------------------------

TEST_CASE("V2: list vector write round-trip", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_logical_type_handle types[1] = {list_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&list_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &child, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Size the child (auto-reserves), then populate it.
	REQUIRE(duckdb_v2_vector_set_size(child, 6, nullptr) == DUCKDB_V2_ERROR_NONE);

	idx_t child_size = 0;
	REQUIRE(duckdb_v2_vector_get_size(child, &child_size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(child_size == 6);

	void *child_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(child, &child_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *child_data = static_cast<int32_t *>(child_raw);
	child_data[0] = 10;
	child_data[1] = 20;
	child_data[2] = 30;
	child_data[3] = 40;
	child_data[4] = 50;
	child_data[5] = 60;

	void *parent_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &parent_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *entries = static_cast<duckdb_v2_list_entry *>(parent_raw);
	entries[0] = {0, 2};
	entries[1] = {2, 1};
	entries[2] = {3, 3};

	duckdb_v2_vector_view parent_view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &parent_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *read_entries = static_cast<const duckdb_v2_list_entry *>(parent_view.data);
	REQUIRE(read_entries[0].offset == 0);
	REQUIRE(read_entries[0].length == 2);
	REQUIRE(read_entries[2].offset == 3);
	REQUIRE(read_entries[2].length == 3);

	duckdb_v2_vector_view child_view {};
	REQUIRE(duckdb_v2_vector_get_view(child, &child_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *read_child = static_cast<const int32_t *>(child_view.data);
	REQUIRE(read_child[0] == 10);
	REQUIRE(read_child[5] == 60);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: list child set_size auto-reserves", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_logical_type_handle types[1] = {list_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&list_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &child, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Set child size to 5000 without reserving first — should auto-reserve.
	REQUIRE(duckdb_v2_vector_set_size(child, 5000, nullptr) == DUCKDB_V2_ERROR_NONE);

	idx_t child_size = 0;
	REQUIRE(duckdb_v2_vector_get_size(child, &child_size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(child_size == 5000);

	// Writing to the child should not crash.
	void *child_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(child, &child_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<int32_t *>(child_raw)[4999] = 42;

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// Struct vector write
// ---------------------------------------------------------------------------

TEST_CASE("V2: struct vector write via children", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto struct_type =
	    MakeStructType(fx.factory, {"a", "b"}, {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR});

	duckdb_v2_logical_type_handle types[1] = {struct_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&struct_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	// Sizing a STRUCT vector propagates the size to its field vectors.
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle field_a = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &field_a, nullptr) == DUCKDB_V2_ERROR_NONE);
	void *a_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(field_a, &a_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<int32_t *>(a_raw)[0] = 100;
	static_cast<int32_t *>(a_raw)[1] = 200;

	duckdb_v2_vector_handle field_b = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 1, &field_b, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2VectorAssignString(field_b, 0, "hello", 5, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2VectorAssignString(field_b, 1, "world", 5, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view view_a {};
	REQUIRE(duckdb_v2_vector_get_view(field_a, &view_a, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(static_cast<const int32_t *>(view_a.data)[0] == 100);
	REQUIRE(static_cast<const int32_t *>(view_a.data)[1] == 200);

	duckdb_v2_vector_view view_b {};
	REQUIRE(duckdb_v2_vector_get_view(field_b, &view_b, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *b_data = static_cast<const duckdb_v2_varchar_t *>(view_b.data);
	REQUIRE(Convert(b_data[0]) == "hello");

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// End-to-end: FLAT integer vector write + read round-trip
// ---------------------------------------------------------------------------

TEST_CASE("V2: flat integer write + read round-trip", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *data = static_cast<int32_t *>(raw);
	for (int32_t i = 0; i < 100; i++) {
		data[i] = i * 7;
	}
	REQUIRE(duckdb_v2_vector_set_size(vec, 100, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.count == 100);
	REQUIRE(view.sel == nullptr);
	auto *rdata = static_cast<const int32_t *>(view.data);
	for (idx_t i = 0; i < 100; i++) {
		REQUIRE(rdata[i] == static_cast<int32_t>(i * 7));
	}

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// Chunk survives after type handles are destroyed
// ---------------------------------------------------------------------------

TEST_CASE("V2: chunk outlives type handles", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<int32_t *>(raw)[0] = 999;

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(static_cast<const int32_t *>(view.data)[0] == 999);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_CASE("V2: data_chunk_create zero columns", "[capi_v2][vector_write]") {
	EnvFixture fx;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_logical_type_handle empty_types[1] = {nullptr};

	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, empty_types, 0, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk != nullptr);

	idx_t vec_count = 99;
	REQUIRE(duckdb_v2_data_chunk_get_vector_count(chunk, &vec_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vec_count == 0);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: vector with zero rows", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_set_size(vec, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t size = 99;
	REQUIRE(duckdb_v2_vector_get_size(vec, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// Incremental list append (unknown total size)
// ---------------------------------------------------------------------------

TEST_CASE("V2: incremental list append", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_logical_type_handle types[1] = {list_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&list_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &child, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *parent_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &parent_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *entries = static_cast<duckdb_v2_list_entry *>(parent_raw);

	// Row 0: [1, 2, 3] — grow the child (auto-reserves), then write.
	idx_t offset = 99;
	REQUIRE(duckdb_v2_vector_get_size(child, &offset, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(offset == 0);
	REQUIRE(duckdb_v2_vector_set_size(child, 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *child_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(child, &child_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *child_data = static_cast<int32_t *>(child_raw);
	child_data[0] = 1;
	child_data[1] = 2;
	child_data[2] = 3;
	entries[0] = {0, 3};

	// Row 1: [4, 5]
	REQUIRE(duckdb_v2_vector_get_size(child, &offset, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(offset == 3);
	REQUIRE(duckdb_v2_vector_set_size(child, 5, nullptr) == DUCKDB_V2_ERROR_NONE);
	child_data[3] = 4;
	child_data[4] = 5;
	entries[1] = {3, 2};

	duckdb_v2_vector_view child_view {};
	REQUIRE(duckdb_v2_vector_get_view(child, &child_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *read = static_cast<const int32_t *>(child_view.data);
	REQUIRE(read[0] == 1);
	REQUIRE(read[4] == 5);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// LIST<VARCHAR>
// ---------------------------------------------------------------------------

TEST_CASE("V2: LIST<VARCHAR> write", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);

	duckdb_v2_logical_type_handle types[1] = {list_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&list_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &child, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_vector_set_size(child, 2, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2VectorAssignString(child, 0, "alpha", 5, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::string long_str(200, 'z');
	REQUIRE(V2VectorAssignString(child, 1, long_str.c_str(), long_str.size(), nullptr) == DUCKDB_V2_ERROR_NONE);

	void *parent_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &parent_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<duckdb_v2_list_entry *>(parent_raw)[0] = {0, 2};

	duckdb_v2_vector_view child_view {};
	REQUIRE(duckdb_v2_vector_get_view(child, &child_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *arr = static_cast<const duckdb_v2_varchar_t *>(child_view.data);

	REQUIRE(Convert(arr[0]) == "alpha");
	REQUIRE(Convert(arr[1]).len == 200);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// MAP write
// ---------------------------------------------------------------------------

TEST_CASE("V2: MAP write via child vectors", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto map_type = MakeMapType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);

	duckdb_v2_logical_type_handle types[1] = {map_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&map_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	// sizing the entries STRUCT sizes both the keys and the values
	duckdb_v2_vector_handle kv = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &kv, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(kv, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle keys = nullptr;
	duckdb_v2_vector_handle values = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(kv, 0, &keys, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(kv, 1, &values, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *keys_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(keys, &keys_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<int32_t *>(keys_raw)[0] = 1;
	static_cast<int32_t *>(keys_raw)[1] = 2;

	REQUIRE(V2VectorAssignString(values, 0, "one", 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2VectorAssignString(values, 1, "two", 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *parent_raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &parent_raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<duckdb_v2_list_entry *>(parent_raw)[0] = {0, 2};

	duckdb_v2_vector_view key_view {};
	REQUIRE(duckdb_v2_vector_get_view(keys, &key_view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(static_cast<const int32_t *>(key_view.data)[0] == 1);
	REQUIRE(static_cast<const int32_t *>(key_view.data)[1] == 2);

	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto rendered = Render(cell);
	duckdb_v2_value_destroy(&cell);
	REQUIRE(rendered == "{1=one, 2=two}");

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ---------------------------------------------------------------------------
// Multiple primitive types
// ---------------------------------------------------------------------------

TEST_CASE("V2: write multiple primitive types", "[capi_v2][vector_write]") {
	EnvFixture fx;
	auto bool_t = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BOOLEAN);
	auto i8_t = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_TINYINT);
	auto i16_t = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_SMALLINT);
	auto i64_t = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto f32_t = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_FLOAT);
	auto f64_t = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE);
	duckdb_v2_logical_type_handle types[6] = {bool_t, i8_t, i16_t, i64_t, f32_t, f64_t};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 6, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&bool_t);
	duckdb_v2_logical_type_destroy(&i8_t);
	duckdb_v2_logical_type_destroy(&i16_t);
	duckdb_v2_logical_type_destroy(&i64_t);
	duckdb_v2_logical_type_destroy(&f32_t);
	duckdb_v2_logical_type_destroy(&f64_t);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	auto write_and_check = [&](idx_t col, auto write_val) {
		using T = decltype(write_val);
		duckdb_v2_vector_handle vec = nullptr;
		REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, col, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);
		void *raw = nullptr;
		REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
		static_cast<T *>(raw)[0] = write_val;
		duckdb_v2_vector_view view {};
		REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(static_cast<const T *>(view.data)[0] == write_val);
	};

	write_and_check(0, true);
	write_and_check(1, static_cast<int8_t>(-42));
	write_and_check(2, static_cast<int16_t>(1000));
	write_and_check(3, static_cast<int64_t>(123456789012LL));
	write_and_check(4, 3.14f);
	write_and_check(5, 2.718281828);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Single-cell value bridge: vector_get_value / vector_set_value
// ===========================================================================

namespace {

// Owned INTEGER chunk with one column; caller destroys.
duckdb_v2_data_chunk_handle MakeIntChunk(duckdb_v2_factory_handle factory, duckdb_v2_vector_handle *out_vec) {
	auto int_type = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	auto vec_rc = duckdb_v2_data_chunk_get_vector(chunk, 0, out_vec, nullptr);
	if (vec_rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	REQUIRE(vec_rc == DUCKDB_V2_ERROR_NONE);
	return chunk;
}

int32_t V2CellI32(duckdb_v2_vector_handle vec, idx_t row) {
	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, row, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	return ConsumeValue<int32_t>(cell);
}

} // namespace

TEST_CASE("V2: vector_get_value reads FLAT and CONSTANT rows", "[capi_v2][vector_write][cell]") {
	EnvFixture fx;
	duckdb_v2_vector_handle vec = nullptr;
	auto chunk = MakeIntChunk(fx.factory, &vec);
	REQUIRE(duckdb_v2_vector_set_size(vec, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *data = static_cast<int32_t *>(raw);
	data[0] = 10;
	data[1] = 20;
	data[2] = 30;

	REQUIRE(V2CellI32(vec, 0) == 10);
	REQUIRE(V2CellI32(vec, 2) == 30);

	// Row bounds are checked against the logical size.
	auto cell = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 3, &cell, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(cell == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	// CONSTANT: every logical row reads the single value.
	duckdb_v2_value_handle forty_two = MakeInt32Value(fx.factory, 42);
	REQUIRE(duckdb_v2_vector_make_constant(vec, forty_two, 5, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&forty_two);
	REQUIRE(V2CellI32(vec, 0) == 42);
	REQUIRE(V2CellI32(vec, 4) == 42);
	REQUIRE(duckdb_v2_vector_get_value(vec, 5, &cell, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_data_chunk_destroy(&chunk);
}

TEST_CASE("V2: vector_get_value resolves DICTIONARY rows through the selection", "[capi_v2][vector_write][cell]") {
	// Same internal fixture shape as the DICTIONARY view test: a FLAT
	// backing vector sliced by a non-identity sel, with one NULLed slot.
	duckdb::Vector flat(duckdb::LogicalType::INTEGER);
	auto *fd = duckdb::FlatVector::GetDataMutable<int32_t>(flat);
	fd[0] = 10;
	fd[1] = 20;
	fd[2] = 30;
	duckdb::FlatVector::SetNull(flat, 1, true);
	duckdb::SelectionVector sel(4);
	sel.set_index(0, 2);
	sel.set_index(1, 0);
	sel.set_index(2, 2);
	sel.set_index(3, 1);
	duckdb::Vector dict(flat, sel, 4);
	auto handle = reinterpret_cast<duckdb_v2_vector_handle>(&dict);

	REQUIRE(V2CellI32(handle, 0) == 30);
	REQUIRE(V2CellI32(handle, 1) == 10);
	REQUIRE(V2CellI32(handle, 2) == 30);
	// Logical row 3 resolves to the NULLed physical slot.
	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(handle, 3, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(cell, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	duckdb_v2_value_destroy(&cell);
	REQUIRE(duckdb_v2_vector_get_value(handle, 4, &cell, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: vector_get_value is the VARIANT cell path", "[capi_v2][vector_write][cell]") {
	EnvFixture f;
	QueryResult r;

	REQUIRE(Query(f.conn, "SELECT 42::VARIANT AS v", &r) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(cell, &t, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(t, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_VARIANT);
	duckdb_v2_logical_type_destroy(&t);
	auto text = Render(cell);
	REQUIRE(std::string(text).find("42") != std::string::npos);
	duckdb_v2_value_destroy(&cell);
	duckdb_v2_data_chunk_destroy(&chunk);
}

TEST_CASE("V2: vector_set_value writes FLAT cells with casts and NULLs", "[capi_v2][vector_write][cell]") {
	EnvFixture fx;
	auto bigint_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_logical_type_handle types[1] = {bigint_type};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&bigint_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	// An INTEGER value is cast to the vector's BIGINT on write.
	duckdb_v2_value_handle small = MakeInt32Value(fx.factory, 7);
	REQUIRE(duckdb_v2_vector_set_value(vec, 0, small, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&small);

	// A NULL value clears the row's validity.
	duckdb_v2_logical_type_handle bigint_v2 = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, 0, &bigint_v2,
	                                      nullptr);
	duckdb_v2_value_handle null_value = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, bigint_v2, &null_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&bigint_v2);
	REQUIRE(duckdb_v2_vector_set_value(vec, 1, null_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&null_value);

	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<int64_t>(cell) == 7);
	duckdb_v2_value_destroy(&cell);
	REQUIRE(duckdb_v2_vector_get_value(vec, 1, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(cell, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	duckdb_v2_value_destroy(&cell);

	// An uncastable value surfaces the conversion error.
	duckdb_v2_value_handle bad = MakeVarcharValue(fx.factory, "abc");
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_set_value(vec, 2, bad, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&bad);

	duckdb_v2_data_chunk_destroy(&chunk);
}

TEST_CASE("V2: vector_set_value refuses non-FLAT vectors and bad rows", "[capi_v2][vector_write][cell]") {
	EnvFixture fx;
	duckdb_v2_vector_handle vec = nullptr;
	auto chunk = MakeIntChunk(fx.factory, &vec);

	duckdb_v2_value_handle value = MakeInt32Value(fx.factory, 1);

	// Out-of-range row on a FLAT vector.
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_value(vec, 2, value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A CONSTANT vector is not row-addressable; flatten first.
	REQUIRE(duckdb_v2_vector_make_constant(vec, value, 4, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_set_value(vec, 0, value, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(duckdb_v2_vector_flatten(vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_value(vec, 0, value, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2CellI32(vec, 0) == 1);

	// Null-arg refusals.
	REQUIRE(duckdb_v2_vector_set_value(nullptr, 0, value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_set_value(vec, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_handle out_cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(nullptr, 0, &out_cell, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_value_destroy(&value);
	duckdb_v2_data_chunk_destroy(&chunk);
}

TEST_CASE("V2: constant LIST vector via make_constant + single-cell round trip", "[capi_v2][vector_write][cell]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {list_type};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Build the LIST value [1, 2] and make the vector constant over it.
	duckdb_v2_value_handle elems[2] = {nullptr, nullptr};
	elems[0] = MakeInt32Value(fx.factory, 1);
	elems[1] = MakeInt32Value(fx.factory, 2);
	duckdb_v2_value_handle list_value = nullptr;
	rc = duckdb_v2_value_create_list(fx.factory, nullptr, elems, 2, &list_value, nullptr);
	duckdb_v2_value_destroy(&elems[0]);
	duckdb_v2_value_destroy(&elems[1]);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_make_constant(vec, list_value, 3, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_VECTOR_TYPE vtype = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &vtype, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vtype == DUCKDB_V2_VECTOR_TYPE_CONSTANT);

	// Every logical row reads back the same list.
	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 2, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t child_count = 0;
	REQUIRE(duckdb_v2_value_get_child_count(cell, &child_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(child_count == 2);
	duckdb_v2_value_handle elem = nullptr;
	REQUIRE(duckdb_v2_value_get_child(cell, 1, &elem, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<int32_t>(elem) == 2);
	duckdb_v2_value_destroy(&elem);
	duckdb_v2_value_destroy(&cell);

	// The type-mismatch hardening: an INTEGER value cannot constant a LIST vector.
	duckdb_v2_value_handle wrong = MakeInt32Value(fx.factory, 9);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_make_constant(vec, wrong, 3, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&wrong);

	duckdb_v2_value_destroy(&list_value);
	duckdb_v2_logical_type_destroy(&list_type);
	duckdb_v2_data_chunk_destroy(&chunk);
}

TEST_CASE("V2: nested cells round trip through set_value / get_value", "[capi_v2][vector_write][cell]") {
	EnvFixture fx;
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {list_type};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle elems[2] = {nullptr, nullptr};
	elems[0] = MakeInt32Value(fx.factory, 5);
	elems[1] = MakeInt32Value(fx.factory, 6);
	duckdb_v2_value_handle full = nullptr;
	rc = duckdb_v2_value_create_list(fx.factory, nullptr, elems, 2, &full, nullptr);
	duckdb_v2_value_destroy(&elems[0]);
	duckdb_v2_value_destroy(&elems[1]);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle empty = nullptr;
	auto elem_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(duckdb_v2_value_create_list(fx.factory, elem_type, nullptr, 0, &empty, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&elem_type);
	duckdb_v2_logical_type_destroy(&list_type);

	REQUIRE(duckdb_v2_vector_set_value(vec, 0, full, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_value(vec, 1, empty, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&full);
	duckdb_v2_value_destroy(&empty);

	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t count = 0;
	REQUIRE(duckdb_v2_value_get_child_count(cell, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 2);
	duckdb_v2_value_handle elem = nullptr;
	REQUIRE(duckdb_v2_value_get_child(cell, 0, &elem, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<int32_t>(elem) == 5);
	duckdb_v2_value_destroy(&elem);
	duckdb_v2_value_destroy(&cell);

	REQUIRE(duckdb_v2_vector_get_value(vec, 1, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_get_child_count(cell, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 0);
	duckdb_v2_value_destroy(&cell);

	duckdb_v2_data_chunk_destroy(&chunk);
}
#endif
} // namespace test_capi_v2
