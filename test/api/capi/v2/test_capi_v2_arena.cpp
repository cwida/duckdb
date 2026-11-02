#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 arena write surface: vector_get_arena + arena_allocate.
// Borrow the arena, allocate vector-lifetime bytes, assemble a duckdb_v2_bytes
// over the transparent layout, and place it via the mutable data array.
// Intermediates die before any REQUIRE.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
namespace {

// Build a single-column chunk of the given type and borrow its vector.
struct StringChunk {
	// Logical types are created through a connection, so carry one.
	EnvFixture env;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_vector_handle vec = nullptr;

	explicit StringChunk(DUCKDB_V2_LOGICAL_TYPE_ID id) {
		auto t = MakeType(env.factory, id);
		duckdb_v2_logical_type_handle types[1] = {t};
		auto rc = duckdb_v2_data_chunk_create(env.factory, types, 1, &chunk, nullptr);
		duckdb_v2_logical_type_destroy(&t);
		REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
		// A REQUIRE throw in a ctor skips the dtor: destroy before failing.
		auto vec_rc = duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		if (vec_rc != DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		REQUIRE(vec_rc == DUCKDB_V2_ERROR_NONE);
	}
	~StringChunk() {
		duckdb_v2_data_chunk_destroy(&chunk);
	}
};

// Inline-ness is a direct field read on the transparent layout.
bool IsInlined(const duckdb_v2_bytes &s) {
	return s.value.inlined.length <= DUCKDB_V2_BYTES_INLINE_LENGTH;
}

} // namespace

// ---------------------------------------------------------------------------
// vector_get_arena
// ---------------------------------------------------------------------------

TEST_CASE("V2: vector_get_arena on string-backed kinds", "[capi_v2][arena]") {
	for (auto type : {DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, DUCKDB_V2_LOGICAL_TYPE_ID_BLOB, DUCKDB_V2_LOGICAL_TYPE_ID_BIT,
	                  DUCKDB_V2_LOGICAL_TYPE_ID_BIGNUM}) {
		StringChunk fixture(type);
		duckdb_v2_arena_handle heap = nullptr;
		REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(heap != nullptr);

		// The heap is stable across calls on the same vector.
		duckdb_v2_arena_handle heap2 = nullptr;
		REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap2, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(heap2 == heap);
	}
}

TEST_CASE("V2: vector_get_arena rejects non-string vector", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_arena_handle heap = reinterpret_cast<duckdb_v2_arena_handle>(0x1);
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// out_heap is nulled on the INVALID_INPUT path.
	REQUIRE(heap == nullptr);
}

TEST_CASE("V2: vector_get_arena null args", "[capi_v2][arena]") {
	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(nullptr, &heap, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ---------------------------------------------------------------------------
// arena_allocate: the raw primitive
// ---------------------------------------------------------------------------

TEST_CASE("V2: arena_allocate write-in-place", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(duckdb_v2_vector_set_size(fixture.vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Generate bytes in place (no intermediate buffer), then assemble and place.
	const idx_t len = 100;
	uint8_t *bytes = nullptr;
	REQUIRE(duckdb_v2_arena_allocate(heap, len, &bytes, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(bytes != nullptr);
	std::memset(bytes, 'x', len);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(fixture.vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *slots = static_cast<duckdb_v2_bytes *>(raw);

	duckdb_v2_bytes storage {};
	storage.value.pointer.length = static_cast<uint32_t>(len);
	storage.value.pointer.ptr = reinterpret_cast<char *>(bytes);
	std::memcpy(storage.value.pointer.prefix, bytes, 4);

	slots[0] = storage;

	REQUIRE_FALSE(IsInlined(slots[0]));

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(fixture.vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *arr = static_cast<const duckdb_v2_varchar_t *>(view.data);
	REQUIRE(Convert(arr[0]) == std::string(len, 'x'));
}

TEST_CASE("V2: arena_allocate byte_len 0 is valid", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	// No size gating: 0 bytes is valid and succeeds.
	uint8_t *bytes = nullptr;
	REQUIRE(duckdb_v2_arena_allocate(heap, 0, &bytes, nullptr) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: arena_allocate null args", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Null heap: out_ptr is nulled on the INVALID_INPUT path.
	uint8_t *bytes = reinterpret_cast<uint8_t *>(0x1);
	REQUIRE(duckdb_v2_arena_allocate(nullptr, 4, &bytes, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(bytes == nullptr);
	// Null out_ptr.
	REQUIRE(duckdb_v2_arena_allocate(heap, 4, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ---------------------------------------------------------------------------
// Assemble + place via MakeString (the canonical write path)
// ---------------------------------------------------------------------------

TEST_CASE("V2: inline vs non-inline placement", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(duckdb_v2_vector_set_size(fixture.vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(fixture.vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *slots = static_cast<duckdb_v2_bytes *>(raw);

	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	// Inlined (<= 12 bytes): self-contained, no allocation.
	slots[0] = MakeString(heap, "hi", 2, rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(IsInlined(slots[0]));

	// Non-inlined (> 12 bytes): copied into the heap.
	std::string long_str(100, 'x');
	slots[1] = MakeString(heap, long_str.data(), long_str.size(), rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(IsInlined(slots[1]));

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(fixture.vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *arr = static_cast<const duckdb_v2_varchar_t *>(view.data);
	REQUIRE(Convert(arr[0]) == "hi");
	REQUIRE(Convert(arr[1]) == long_str);
}

TEST_CASE("V2: empty string is inlined", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	// {NULL, 0} and {"", 0} both yield an inlined empty string.
	duckdb_v2_bytes a = MakeString(heap, nullptr, 0, rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_bytes b = MakeString(heap, "", 0, rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	for (auto *s : {&a, &b}) {
		REQUIRE(IsInlined(*s));
		REQUIRE(Convert(*s).len == 0);
	}
}

TEST_CASE("V2: BLOB with embedded nulls (inline + heap)", "[capi_v2][arena]") {
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);
	REQUIRE(duckdb_v2_vector_set_size(fixture.vec, 2, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(fixture.vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *slots = static_cast<duckdb_v2_bytes *>(raw);

	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	// 5-byte blob: inlined, embedded null preserved.
	const char small[] = "\xDE\xAD\x00\xBE\xEF";
	slots[0] = MakeString(heap, small, 5, rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(IsInlined(slots[0]));

	// 20-byte blob with embedded nulls: heap path.
	std::string big(20, '\0');
	big[0] = static_cast<char>(0xDE);
	big[10] = static_cast<char>(0xAD);
	big[19] = static_cast<char>(0xEF);
	slots[1] = MakeString(heap, big.data(), big.size(), rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(IsInlined(slots[1]));

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(fixture.vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *arr = static_cast<const duckdb_v2_blob_t *>(view.data);

	duckdb_v2_str out = Convert(arr[0]);
	auto *out_data = reinterpret_cast<const uint8_t *>(out.ptr);
	REQUIRE(out.len == 5);
	REQUIRE(out_data[0] == 0xDE);
	REQUIRE(out_data[2] == 0x00);
	REQUIRE(out_data[4] == 0xEF);

	out = Convert(arr[1]);
	out_data = reinterpret_cast<const uint8_t *>(out.ptr);
	REQUIRE(out.len == 20);
	REQUIRE(out_data[0] == 0xDE);
	REQUIRE(out_data[5] == 0x00);
	REQUIRE(out_data[10] == 0xAD);
	REQUIRE(out_data[19] == 0xEF);
}

// ---------------------------------------------------------------------------
// Constant vector: the heap surface works the same; placement targets slot 0
// ---------------------------------------------------------------------------

#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2: arena write on constant vector", "[capi_v2][arena]") {
	EnvFixture fx;
	StringChunk fixture(DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);

	duckdb_v2_value_handle value = MakeVarcharValue(fx.factory, "init");
	REQUIRE(duckdb_v2_vector_make_constant(fixture.vec, value, 3, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_destroy(&value) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_arena_handle heap = nullptr;
	REQUIRE(duckdb_v2_vector_get_arena(fixture.vec, &heap, nullptr) == DUCKDB_V2_ERROR_NONE);

	void *raw = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(fixture.vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *slots = static_cast<duckdb_v2_bytes *>(raw);

	// A heap-backed value (> 12 bytes) so the constant path exercises allocate.
	const std::string constant = "a constant value longer than twelve bytes";
	DUCKDB_V2_ERROR rc = DUCKDB_V2_ERROR_NONE;
	slots[0] = MakeString(heap, constant.data(), constant.size(), rc, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(IsInlined(slots[0]));

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(fixture.vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto *arr = static_cast<const duckdb_v2_varchar_t *>(view.data);
	REQUIRE(Convert(arr[SelAt(view.sel, 0)]) == constant);
	REQUIRE(Convert(arr[SelAt(view.sel, 2)]) == constant);
}
#endif

} // namespace test_capi_v2
