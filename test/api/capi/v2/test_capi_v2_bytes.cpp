#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// Tests for the transparent duckdb_v2_bytes layout: read its fields directly
// (via the StrInlined / StrLength / StrData helpers below) and cross-validate
// against the V1 string_t accessors (duckdb_string_is_inlined, _t_length,
// _t_data) and the V2 wire-codec decoders (bit/bignum_decode).
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
namespace {

#ifdef DUCKDB_DEBUG_NO_INLINE
constexpr bool STRINGS_ARE_IS_INLINED = false;
#else
constexpr bool STRINGS_ARE_IS_INLINED = true;
#endif

// Direct field reads on the transparent layout. The length field overlaps in
// both union members, so it is read through inlined regardless of form.
bool StrInlined(const duckdb_v2_bytes *s) {
	return (s->value.inlined.length <= DUCKDB_V2_BYTES_INLINE_LENGTH) && STRINGS_ARE_IS_INLINED;
}
uint32_t StrLength(const duckdb_v2_bytes *s) {
	return s->value.inlined.length;
}
const char *StrData(const duckdb_v2_bytes *s) {
	return StrInlined(s) ? s->value.inlined.inlined : s->value.pointer.ptr;
}

// BIT wire format: data[0] is the padding-bit count; data[1..] is the payload.
uint8_t BitPadding(const duckdb_v2_bytes *s) {
	return StrLength(s) == 0 ? 0 : static_cast<uint8_t>(StrData(s)[0]);
}
uint64_t BitCount(const duckdb_v2_bytes *s) {
	uint32_t len = StrLength(s);
	return len == 0 ? 0 : static_cast<uint64_t>(len - 1) * 8 - static_cast<uint8_t>(StrData(s)[0]);
}
const uint8_t *BitGetData(const duckdb_v2_bytes *s) {
	return reinterpret_cast<const uint8_t *>(StrData(s)) + 1;
}

// BIGNUM sign encoding: MSB of data[0] clear = negative, set = positive.
bool BignumNegative(const duckdb_v2_bytes *s) {
	return StrLength(s) == 0 ? false : (static_cast<uint8_t>(StrData(s)[0]) & 0x80) == 0;
}

struct V2InlineFixture {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	duckdb_v2_connection_handle conn = nullptr;
	V2InlineFixture() {
		duckdb_v2_environment_create(&env, nullptr);
		OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr);
		duckdb_v2_connection_create(instance, &conn, nullptr);
	}
	~V2InlineFixture() {
		duckdb_v2_connection_destroy(&conn);
		duckdb_v2_instance_destroy(&instance);
		duckdb_v2_environment_destroy(&env);
	}
};

// Executes a single-column query, asserts row count, and holds chunk 0's view.
struct InlQueryRows {
	duckdb_v2_result_handle r = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_vector_view view {};
	idx_t size = 0;

	InlQueryRows(duckdb_v2_connection_handle conn, const char *sql, idx_t expected_rows) {
		REQUIRE(Query(conn, sql, &r, nullptr) == DUCKDB_V2_ERROR_NONE);
		chunk = StepChunk(r);
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		REQUIRE(size == expected_rows);
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		duckdb_v2_vector_get_view(vec, &view, nullptr);
	}
	~InlQueryRows() {
		duckdb_v2_data_chunk_destroy(&chunk);
		duckdb_v2_result_destroy(&r);
	}
	template <typename T>
	const T *as() const {
		return static_cast<const T *>(view.data);
	}
};

} // namespace

// ===========================================================================
// Transparent string reads vs V1 equivalents.
// "short" (5 chars, inlined) and repeat('x', 50) (pointer form).
// ===========================================================================

TEST_CASE("V2 bytes layout: direct reads match V1 equivalents", "[capi_v2][bytes_layout]") {
	V2InlineFixture fx;
	InlQueryRows qr(fx.conn, "SELECT * FROM (VALUES ('short'), (repeat('x', 50))) t(s)", 2);
	const duckdb_v2_bytes *arr = qr.as<duckdb_v2_bytes>();

	for (idx_t row = 0; row < qr.size; row++) {
		idx_t phys = SelAt(qr.view.sel, row);
		const duckdb_v2_bytes *v2s = &arr[phys];
		// V1 uses the same binary layout; cast is safe.
		duckdb_string_t v1s = *reinterpret_cast<const duckdb_string_t *>(v2s);

		REQUIRE(StrInlined(v2s) == duckdb_string_is_inlined(v1s));
		REQUIRE(StrLength(v2s) == duckdb_string_t_length(v1s));
		// duckdb_string_t_data wants a mutable pointer; compare through a local
		// copy instead of casting away const. Inlined data lives in the value,
		// so compare bytes; pointer-form data must be the same address.
		duckdb_string_t v1s_copy = v1s;
		if (duckdb_string_is_inlined(v1s)) {
			REQUIRE(std::memcmp(StrData(v2s), duckdb_string_t_data(&v1s_copy), StrLength(v2s)) == 0);
		} else {
			REQUIRE(StrData(v2s) == duckdb_string_t_data(&v1s_copy));
		}
	}

	// Pin expected values for the two rows.
	{
		REQUIRE((!STRINGS_ARE_IS_INLINED || StrInlined(&arr[SelAt(qr.view.sel, 0)]))); // "short" fits inline
		REQUIRE_FALSE(StrInlined(&arr[SelAt(qr.view.sel, 1)]));                        // 50-char string does not

		REQUIRE(StrLength(&arr[SelAt(qr.view.sel, 0)]) == 5);
		REQUIRE(StrLength(&arr[SelAt(qr.view.sel, 1)]) == 50);

		REQUIRE(std::string(StrData(&arr[SelAt(qr.view.sel, 0)]), 5) == "short");
		REQUIRE(std::string(StrData(&arr[SelAt(qr.view.sel, 1)]), 50) == std::string(50, 'x'));
	}
}

// ===========================================================================
// Transparent BIT reads (the client-side split: byte 0 is the padding-bit
// count, bytes 1.. are the data).
//   '11111111' → 8 bits, padding=0, data[0]=0xFF
//   '101'      → 3 bits, padding=5
// ===========================================================================

TEST_CASE("V2 bytes layout: BIT reads via the transparent split", "[capi_v2][bytes_layout]") {
	V2InlineFixture fx;
	InlQueryRows qr(fx.conn, "SELECT * FROM (VALUES ('11111111'::BIT), ('101'::BIT)) t(b)", 2);
	const duckdb_v2_bit_t *arr = qr.as<duckdb_v2_bit_t>();

	const duckdb_v2_bytes *r0 = &arr[SelAt(qr.view.sel, 0)];
	REQUIRE(BitPadding(r0) == 0);
	REQUIRE(BitCount(r0) == 8);
	REQUIRE(static_cast<uint8_t>(BitGetData(r0)[0]) == 0xFF);

	const duckdb_v2_bytes *r1 = &arr[SelAt(qr.view.sel, 1)];
	REQUIRE(BitPadding(r1) == 5);
	REQUIRE(BitCount(r1) == 3);
}

// ===========================================================================
// Transparent BIGNUM sign vs bignum_decode neg flag.
//   2^128 - 1  → positive
//   -256       → negative
// ===========================================================================

TEST_CASE("V2 bytes layout: BIGNUM sign matches bignum_decode", "[capi_v2][bytes_layout]") {
	V2InlineFixture fx;
	InlQueryRows qr(fx.conn,
	                "SELECT * FROM (VALUES "
	                "  (340282366920938463463374607431768211455::BIGNUM), "
	                "  (-256::BIGNUM)) t(b)",
	                2);
	const duckdb_v2_bignum_t *arr = qr.as<duckdb_v2_bignum_t>();

	for (idx_t row = 0; row < qr.size; row++) {
		idx_t phys = SelAt(qr.view.sel, row);
		// Resolve the payload the same way as VARCHAR / BLOB, then decode.
		auto storage = Convert(arr[phys]);
		idx_t mag_len = 0;
		bool dec_neg = false;
		// A null out buffer is a size + sign query, which is all this needs.
		REQUIRE(duckdb_v2_bignum_decode(reinterpret_cast<const uint8_t *>(storage.ptr), storage.len, nullptr, 0,
		                                &mag_len, &dec_neg, nullptr) == DUCKDB_V2_ERROR_NONE);

		REQUIRE(BignumNegative(&arr[phys]) == dec_neg);
	}

	// Pin expected signs.
	{
		REQUIRE_FALSE(BignumNegative(&arr[SelAt(qr.view.sel, 0)])); // 2^128-1, positive
		REQUIRE(BignumNegative(&arr[SelAt(qr.view.sel, 1)]));       // -256, negative
	}
}

} // namespace test_capi_v2
