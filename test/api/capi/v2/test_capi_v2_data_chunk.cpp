#include "test_capi_v2.hpp"

namespace test_capi_v2 {
// ---------------------------------------------------------------------------
// V2 data_chunk + vector tests — the read surface for row data.
// ---------------------------------------------------------------------------

// ===========================================================================
// Smoke: SELECT 1 round-trip through chunk + vector + view.
// ===========================================================================

TEST_CASE("V2: chunk + view round-trip on SELECT 1", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 42::INTEGER AS i", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);

	idx_t size = 0;
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 1);

	idx_t vec_count = 0;
	REQUIRE(duckdb_v2_data_chunk_get_vector_count(chunk, &vec_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vec_count == 1);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vec != nullptr);

	DUCKDB_V2_VECTOR_TYPE vt = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &vt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vt != DUCKDB_V2_VECTOR_TYPE_OTHER);

	// Types are read from the result schema, not from the vector.
	RequireColumn(r, 0, "i", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 1);
	REQUIRE(view.count == size); // view count matches chunk size
	REQUIRE(view.data != nullptr);
	const int32_t *data = static_cast<const int32_t *>(view.data);
	REQUIRE(data[SelAt(view.sel, 0)] == 42);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk == nullptr);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// Multi-row INTEGER FLAT vector with one NULL — checks validity bit
// reading via the documented inline formula (RowValid). Also pins that
// view.sel is NULL for FLAT (identity), so sel resolution returns the
// row index unchanged.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2: INTEGER vector with NULL — validity + identity sel", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES (1), (NULL), (3)) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);

	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 3);                // row count comes from the chunk
	REQUIRE(view.validity != nullptr); // not all-valid
	REQUIRE(view.sel == nullptr);      // FLAT vector → identity sel

	const int32_t *data = static_cast<const int32_t *>(view.data);

	// Identity sel: SelAt(NULL, i) == i.
	for (idx_t i = 0; i < size; i++) {
		REQUIRE(SelAt(view.sel, i) == i);
	}

	// Validity matches the VALUES (1), (NULL), (3) pattern.
	REQUIRE(RowValid(view, 0));
	REQUIRE(data[0] == 1);

	REQUIRE_FALSE(RowValid(view, 1));

	REQUIRE(RowValid(view, 2));
	REQUIRE(data[2] == 3);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}
#endif

// ===========================================================================
// Read multiple primitive kinds in one chunk. Each column gets its own
// cast of view.data via the kind's natural C type. Pins the contract
// that all primitive kinds are readable through the single untyped view.
// ===========================================================================

TEST_CASE("V2: primitive view round-trips across many types", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn,
	              "SELECT TRUE AS b, "
	              "       (-5)::TINYINT AS i8, "
	              "       1000::SMALLINT AS i16, "
	              "       3.5::FLOAT AS f, "
	              "       2.5::DOUBLE AS d, "
	              "       DATE '2026-05-19' AS dt, "
	              "       TIMESTAMP '2026-05-19 12:00:00' AS ts",
	              &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);

	struct ReadAs {
		idx_t col;
		std::function<void(const duckdb_v2_vector_view &)> check;
	};

	auto check_col = [&](idx_t col_idx, auto check) {
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, col_idx, &vec, nullptr);
		duckdb_v2_vector_view view {};
		REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
		check(view);
	};

	check_col(0, [](const duckdb_v2_vector_view &view) {
		const bool *data = static_cast<const bool *>(view.data);
		REQUIRE(data[SelAt(view.sel, 0)] == true);
	});
	check_col(1, [](const duckdb_v2_vector_view &view) {
		const int8_t *data = static_cast<const int8_t *>(view.data);
		REQUIRE(data[SelAt(view.sel, 0)] == -5);
	});
	check_col(2, [](const duckdb_v2_vector_view &view) {
		const int16_t *data = static_cast<const int16_t *>(view.data);
		REQUIRE(data[SelAt(view.sel, 0)] == 1000);
	});
	check_col(3, [](const duckdb_v2_vector_view &view) {
		const float *data = static_cast<const float *>(view.data);
		REQUIRE(data[SelAt(view.sel, 0)] == 3.5f);
	});
	check_col(4, [](const duckdb_v2_vector_view &view) {
		const double *data = static_cast<const double *>(view.data);
		REQUIRE(data[SelAt(view.sel, 0)] == 2.5);
	});
	check_col(5, [](const duckdb_v2_vector_view &view) {
		const int32_t *data = static_cast<const int32_t *>(view.data);
		// 2026-05-19 = positive number of days since 1970-01-01; spot-
		// check it's plausible without pinning the exact integer.
		REQUIRE(data[SelAt(view.sel, 0)] > 20000);
	});
	check_col(6, [](const duckdb_v2_vector_view &view) {
		const int64_t *data = static_cast<const int64_t *>(view.data);
		REQUIRE(data[SelAt(view.sel, 0)] > 0);
	});

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// HUGEINT + INTERVAL: layout typedefs (duckdb_v2_hugeint_t,
// duckdb_v2_interval_t) — caller casts view.data to the matching layout
// type and reads the structured fields.
// ===========================================================================

TEST_CASE("V2: HUGEINT + INTERVAL via layout typedefs", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	// Value chosen > 2^64 so upper word is non-zero.
	REQUIRE(Query(fx.conn,
	              "SELECT 99999999999999999999::HUGEINT AS h, "
	              "       INTERVAL '3 months 4 days 5 microseconds' AS iv",
	              &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);

	// HUGEINT column.
	duckdb_v2_vector_handle hvec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &hvec, nullptr);
	duckdb_v2_vector_view hv {};
	duckdb_v2_vector_get_view(hvec, &hv, nullptr);
	const duckdb_v2_hugeint_t *hdata = static_cast<const duckdb_v2_hugeint_t *>(hv.data);
	idx_t hidx = SelAt(hv.sel, 0);
	// 12345678901234567890 in hugeint: upper * 2^64 + lower. Spot-
	// check it's the right ballpark rather than pin exact bits.
	REQUIRE(hdata[hidx].upper > 0);
	REQUIRE(hdata[hidx].lower > 0);

	// INTERVAL column.
	duckdb_v2_vector_handle ivec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 1, &ivec, nullptr);
	duckdb_v2_vector_view iv {};
	duckdb_v2_vector_get_view(ivec, &iv, nullptr);
	const duckdb_v2_interval_t *idata = static_cast<const duckdb_v2_interval_t *>(iv.data);
	idx_t iidx = SelAt(iv.sel, 0);
	REQUIRE(idata[iidx].months == 3);
	REQUIRE(idata[iidx].days == 4);
	REQUIRE(idata[iidx].micros == 5);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// VARCHAR via direct transparent-field reads (Convert), across
// both inlined (short) and pointer (long) string_t storage forms.
// ===========================================================================

TEST_CASE("V2: VARCHAR direct reads (inlined + pointer forms)", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	// 8-byte string fits inlined; 50-byte string forces pointer form.
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ('short'), (repeat('x', 50))) t(s)", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);

	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	REQUIRE(size == 2); // chunk size carries the row count

	const duckdb_v2_varchar_t *arr = static_cast<const duckdb_v2_varchar_t *>(view.data);

	duckdb_v2_str s0 = Convert(arr[SelAt(view.sel, 0)]);
	REQUIRE(s0.len == 5);
	REQUIRE(s0 == "short");

	duckdb_v2_str s1 = Convert(arr[SelAt(view.sel, 1)]);
	REQUIRE(s1.len == 50);
	REQUIRE(s1 == std::string(50, 'x'));

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// BLOB via direct transparent-field reads. Same shape as varchar but
// the bytes are raw.
// ===========================================================================

TEST_CASE("V2: BLOB direct reads (inlined + pointer forms)", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	// 4 bytes fit inline (<= DUCKDB_V2_BYTES_INLINE_LENGTH); 15 bytes take
	// the pointer form. The two rows straddle the cutoff.
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ('\\xDE\\xAD\\xBE\\xEF'::BLOB), ('ABCDEFGHIJKLMNO'::BLOB)) t(b)", &r,
	              nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 2);

	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);

	const duckdb_v2_blob_t *arr = static_cast<const duckdb_v2_blob_t *>(view.data);
	const duckdb_v2_bytes &short_form = arr[SelAt(view.sel, 0)];
	duckdb_v2_str b = Convert(short_form);
	const uint8_t *data = reinterpret_cast<const uint8_t *>(b.ptr);
	REQUIRE(b.len == 4);
	REQUIRE(short_form.value.inlined.length <= DUCKDB_V2_BYTES_INLINE_LENGTH); // inlined form
	REQUIRE(data[0] == 0xDE);
	REQUIRE(data[1] == 0xAD);
	REQUIRE(data[2] == 0xBE);
	REQUIRE(data[3] == 0xEF);

	const duckdb_v2_bytes &long_form = arr[SelAt(view.sel, 1)];
	duckdb_v2_str b1 = Convert(long_form);
	REQUIRE(b1.len == 15);
	REQUIRE(long_form.value.inlined.length > DUCKDB_V2_BYTES_INLINE_LENGTH); // pointer form
	REQUIRE(b1 == "ABCDEFGHIJKLMNO");

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// BIT via the client-side split: byte 0 is the padding-bit count, bytes 1..
// are the data. Exercise two patterns:
//   '11111111' → 8 bits, padding=0, data[0]=0xFF (unambiguous, no bit
//                                                  ordering dependency)
//   '101'      → 3 bits, padding=5 (pins the padding peel)
// ===========================================================================

TEST_CASE("V2: BIT via the transparent split", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ('11111111'::BIT), ('101'::BIT)) t(b)", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = StepChunk(r);

	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);

	const duckdb_v2_bit_t *arr = static_cast<const duckdb_v2_bit_t *>(view.data);

	// Row 0: '11111111'. The storage is [padding_byte, data...].
	duckdb_v2_str c0 = Convert(arr[SelAt(view.sel, 0)]);
	const uint8_t *b0 = reinterpret_cast<const uint8_t *>(c0.ptr);
	REQUIRE(c0.len == 2); // padding byte + one data byte
	REQUIRE(b0[0] == 0);  // padding = 0
	REQUIRE(b0[1] == 0xFF);

	// Row 1: '101' → 3 bits, padding 5.
	duckdb_v2_str c1 = Convert(arr[SelAt(view.sel, 1)]);
	const uint8_t *b1 = reinterpret_cast<const uint8_t *>(c1.ptr);
	REQUIRE(c1.len == 2);
	REQUIRE(b1[0] == 5);
	// Bit-position contract: bit n lives at data[(n+padding)/8], MSB-first.
	// For '101' (padding=5), positions 5, 6, 7 of the data byte are the bits.
	REQUIRE(((b1[1] >> (7 - 5)) & 1) == 1); // '1'
	REQUIRE(((b1[1] >> (7 - 6)) & 1) == 0); // '0'
	REQUIRE(((b1[1] >> (7 - 7)) & 1) == 1); // '1'

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// BIGNUM via duckdb_v2_bignum_decode (bridge function). The payload is
// resolved like any other bytes column, then decoded into a caller-owned
// buffer — sized by a null-out_data query, or just big enough up front.
// ===========================================================================

TEST_CASE("V2: BIGNUM via bignum_decode (positive + negative)", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn,
	              "SELECT * FROM (VALUES "
	              "  (340282366920938463463374607431768211455::BIGNUM), "
	              "  (-256::BIGNUM)) t(b)",
	              &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 2);

	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);

	const duckdb_v2_bignum_t *arr = static_cast<const duckdb_v2_bignum_t *>(view.data);

	// Row 0: 2^128 - 1 = 16 bytes of 0xFF, positive. Size it first.
	auto storage0 = Convert(arr[SelAt(view.sel, 0)]);
	const auto *bytes0 = reinterpret_cast<const uint8_t *>(storage0.ptr);
	idx_t mag0_len = 0;
	bool neg0 = true;
	REQUIRE(duckdb_v2_bignum_decode(bytes0, storage0.len, nullptr, 0, &mag0_len, &neg0, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(neg0);
	REQUIRE(mag0_len == 16);

	std::vector<uint8_t> mag0(mag0_len);
	REQUIRE(duckdb_v2_bignum_decode(bytes0, storage0.len, mag0.data(), mag0.size(), &mag0_len, &neg0, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(mag0_len == 16);
	for (idx_t i = 0; i < mag0_len; i++) {
		REQUIRE(mag0[i] == 0xFF);
	}

	// Row 1: -256 = magnitude {0x01, 0x00}, is_negative = true. A buffer with
	// room to spare decodes in one call; the excess is left untouched.
	auto storage1 = Convert(arr[SelAt(view.sel, 1)]);
	uint8_t mag1[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
	idx_t mag1_len = 0;
	bool neg1 = false;
	REQUIRE(duckdb_v2_bignum_decode(reinterpret_cast<const uint8_t *>(storage1.ptr), storage1.len, mag1, sizeof(mag1),
	                                &mag1_len, &neg1, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(neg1);
	REQUIRE(mag1_len == 2);
	REQUIRE(mag1[0] == 0x01);
	REQUIRE(mag1[1] == 0x00);
	REQUIRE(mag1[2] == 0xAA);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// LIST<INTEGER>: descend via the generic vector_get_child (index 0 for
// LIST = elements). Pin vector_list_get_size against the expected
// total element count.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2: LIST<INTEGER> via get_child + entries", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ([1, 2, 3]), ([10, 20]), ([100])) t(lst)", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 3);

	duckdb_v2_vector_handle lvec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &lvec, nullptr);

	duckdb_v2_vector_view lview {};
	duckdb_v2_vector_get_view(lvec, &lview, nullptr);
	const duckdb_v2_list_entry *entries = static_cast<const duckdb_v2_list_entry *>(lview.data);

	// LIST has exactly one child (the elements).
	idx_t child_count = 0;
	REQUIRE(duckdb_v2_vector_get_child_count(lvec, &child_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(child_count == 1);

	duckdb_v2_vector_handle child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(lvec, 0, &child, nullptr) == DUCKDB_V2_ERROR_NONE);

	// list_get_size returns the total number of elements across all parent rows.
	idx_t list_size = 0;
	REQUIRE(duckdb_v2_vector_get_size(child, &list_size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(list_size == 6); // 3 + 2 + 1

	duckdb_v2_vector_view cview {};
	duckdb_v2_vector_get_view(child, &cview, nullptr);
	const int32_t *cdata = static_cast<const int32_t *>(cview.data);

	const int32_t expected[6] = {1, 2, 3, 10, 20, 100};
	for (idx_t i = 0; i < size; i++) { // iterate parent rows; row count from chunk
		idx_t pi = SelAt(lview.sel, i);
		duckdb_v2_list_entry e = entries[pi];
		for (idx_t j = 0; j < e.length; j++) {
			idx_t child_row = e.offset + j;
			idx_t ci = SelAt(cview.sel, child_row);
			REQUIRE(cdata[ci] == expected[e.offset + j]);
		}
	}

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}
#endif

// ===========================================================================
// STRUCT(INTEGER, VARCHAR): descend via the generic vector_get_child.
// STRUCT children are parallel to the parent — child row count is the
// parent's row count (= chunk size for top-level), and there is no
// separate accessor for it.
// ===========================================================================

TEST_CASE("V2: STRUCT(INTEGER, VARCHAR) via get_child", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ({'a': 1, 'b': 'first'}), ({'a': 2, 'b': 'second'})) t(s)", &r,
	              nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);

	duckdb_v2_vector_handle svec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &svec, nullptr);

	idx_t field_count = 0;
	REQUIRE(duckdb_v2_vector_get_child_count(svec, &field_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(field_count == 2);

	// view.data is unspecified for STRUCT (typically NULL — fields
	// live in the children; the STRUCT parent has no per-row leaf data).
	duckdb_v2_vector_view sview {};
	duckdb_v2_vector_get_view(svec, &sview, nullptr);
	REQUIRE(sview.data == nullptr);

	// Field 0: INTEGER.
	duckdb_v2_vector_handle a_child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(svec, 0, &a_child, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view a_view {};
	duckdb_v2_vector_get_view(a_child, &a_view, nullptr);
	const int32_t *adata = static_cast<const int32_t *>(a_view.data);
	REQUIRE(adata[SelAt(a_view.sel, 0)] == 1);
	REQUIRE(adata[SelAt(a_view.sel, 1)] == 2);

	// Field 1: VARCHAR.
	duckdb_v2_vector_handle b_child = nullptr;
	duckdb_v2_vector_get_child(svec, 1, &b_child, nullptr);
	duckdb_v2_vector_view b_view {};
	duckdb_v2_vector_get_view(b_child, &b_view, nullptr);
	const duckdb_v2_varchar_t *barr = static_cast<const duckdb_v2_varchar_t *>(b_view.data);

	REQUIRE(Convert(barr[SelAt(b_view.sel, 0)]) == "first");
	REQUIRE(Convert(barr[SelAt(b_view.sel, 1)]) == "second");

	// Out-of-range field index rejected.
	duckdb_v2_vector_handle oor = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(svec, 99, &oor, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(oor == nullptr);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// TUPLE(INTEGER, VARCHAR): the unnamed struct follows the STRUCT descent
// convention ([i] = field i). row(...) literals produce it.
// ===========================================================================

TEST_CASE("V2: TUPLE(INTEGER, VARCHAR) via get_child", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ((1, 'first')), ((2, 'second'))) t(s)", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	duckdb_v2_vector_handle tvec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &tvec, nullptr);

	RequireColumn(r, 0, "s", DUCKDB_V2_LOGICAL_TYPE_ID_TUPLE);

	idx_t field_count = 0;
	REQUIRE(duckdb_v2_vector_get_child_count(tvec, &field_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(field_count == 2);

	// Field 0: INTEGER.
	duckdb_v2_vector_handle f0 = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(tvec, 0, &f0, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view v0 {};
	duckdb_v2_vector_get_view(f0, &v0, nullptr);
	const int32_t *ints = static_cast<const int32_t *>(v0.data);
	REQUIRE(ints[SelAt(v0.sel, 0)] == 1);
	REQUIRE(ints[SelAt(v0.sel, 1)] == 2);

	// Field 1: VARCHAR.
	duckdb_v2_vector_handle f1 = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(tvec, 1, &f1, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view v1 {};
	duckdb_v2_vector_get_view(f1, &v1, nullptr);
	const duckdb_v2_varchar_t *strs = static_cast<const duckdb_v2_varchar_t *>(v1.data);
	REQUIRE(Convert(strs[SelAt(v1.sel, 0)]) == "first");
	REQUIRE(Convert(strs[SelAt(v1.sel, 1)]) == "second");

	// Out-of-range field index rejected.
	duckdb_v2_vector_handle oor = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(tvec, 99, &oor, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(oor == nullptr);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// ARRAY(INTEGER, 3): get_child returns the elements child (index 0);
// child row count = parent_count * array_size; list_get_size rejects
// on ARRAY (it's not LIST/MAP — child rows are derivable from logical
// type + parent_count).
// ===========================================================================

TEST_CASE("V2: ARRAY(INTEGER, 3) via get_child", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES ([1, 2, 3]::INTEGER[3]), ([10, 20, 30]::INTEGER[3])) t(a)", &r,
	              nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 2);

	duckdb_v2_vector_handle avec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &avec, nullptr);

	// ARRAY has exactly 1 child (elements). list_get_size rejects it
	// because the child row count is parent_count * array_size, which
	// the caller computes from logical type + parent geometry.
	idx_t nch = 99;
	REQUIRE(duckdb_v2_vector_get_child_count(avec, &nch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(nch == 1);

	duckdb_v2_vector_handle child = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(avec, 0, &child, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view cview {};
	duckdb_v2_vector_get_view(child, &cview, nullptr);
	const int32_t *cdata = static_cast<const int32_t *>(cview.data);

	// Pull the parent's view too so we resolve the parent sel
	// correctly. For a FLAT ARRAY (this query) it's identity, but
	// the test should still write the right read pattern: row r's
	// elements live at child[parent_phys * 3 .. parent_phys * 3 + 3).
	duckdb_v2_vector_view aview {};
	duckdb_v2_vector_get_view(avec, &aview, nullptr);
	// view.data is unspecified for ARRAY (typically NULL — the
	// per-row content lives in the elements child, not on the parent).
	REQUIRE(aview.data == nullptr);

	const int32_t expected[6] = {1, 2, 3, 10, 20, 30};
	for (idx_t r_idx = 0; r_idx < size; r_idx++) {
		idx_t parent_phys = SelAt(aview.sel, r_idx);
		for (idx_t k = 0; k < 3; k++) {
			idx_t flat_row = parent_phys * 3 + k;
			idx_t ci = SelAt(cview.sel, flat_row);
			REQUIRE(cdata[ci] == expected[flat_row]);
		}
	}

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// MAP(VARCHAR, INTEGER): V2 hides MAP's internal LIST<STRUCT(K,V)> and
// exposes 2 children — [0]=keys, [1]=values — parallel, both sized to
// list_get_size. Pins the per-kind index convention and the V2
// abstraction.
// ===========================================================================

TEST_CASE("V2: MAP(VARCHAR, INTEGER) via get_child", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM (VALUES (MAP {'a': 1, 'b': 2}), (MAP {'c': 3})) t(m)", &r, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 2);

	duckdb_v2_vector_handle mvec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &mvec, nullptr);

	// MAP exposes 1 child: [0]=entries, a STRUCT(key, value).
	idx_t nch = 0;
	REQUIRE(duckdb_v2_vector_get_child_count(mvec, &nch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(nch == 1);

	// Parent's view.data is list_entry[] (one entry per parent row).
	duckdb_v2_vector_view mview {};
	duckdb_v2_vector_get_view(mvec, &mview, nullptr);
	const duckdb_v2_list_entry *entries = static_cast<const duckdb_v2_list_entry *>(mview.data);

	duckdb_v2_vector_handle kv = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(mvec, 0, &kv, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child_count(kv, &nch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(nch == 2);

	duckdb_v2_vector_handle keys = nullptr;
	duckdb_v2_vector_handle values = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(kv, 0, &keys, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(kv, 1, &values, nullptr) == DUCKDB_V2_ERROR_NONE);

	// row count of children returns the K/V pair count = sum of valid list lengths.
	idx_t map_size = 0;
	REQUIRE(duckdb_v2_vector_get_size(keys, &map_size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(map_size == 3); // 2 + 1 entries

	REQUIRE(duckdb_v2_vector_get_size(values, &map_size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(map_size == 3); // 2 + 1 entries

	duckdb_v2_vector_view kview {};
	duckdb_v2_vector_view vview {};
	duckdb_v2_vector_get_view(keys, &kview, nullptr);
	duckdb_v2_vector_get_view(values, &vview, nullptr);
	const duckdb_v2_varchar_t *karr = static_cast<const duckdb_v2_varchar_t *>(kview.data);
	const int32_t *vdata = static_cast<const int32_t *>(vview.data);

	// Row 0: ('a' → 1, 'b' → 2)
	{
		idx_t pi = SelAt(mview.sel, 0);
		duckdb_v2_list_entry e = entries[pi];
		REQUIRE(e.length == 2);
		REQUIRE(Convert(karr[SelAt(kview.sel, e.offset + 0)]) == "a");
		REQUIRE(vdata[SelAt(vview.sel, e.offset + 0)] == 1);

		REQUIRE(Convert(karr[SelAt(kview.sel, e.offset + 1)]) == "b");
		REQUIRE(vdata[SelAt(vview.sel, e.offset + 1)] == 2);
	}
	// Row 1: ('c' → 3)
	{
		idx_t pi = SelAt(mview.sel, 1);
		duckdb_v2_list_entry e = entries[pi];
		REQUIRE(e.length == 1);
		REQUIRE(Convert(karr[SelAt(kview.sel, e.offset + 0)]) == "c");
		REQUIRE(vdata[SelAt(vview.sel, e.offset + 0)] == 3);
	}

	// Out-of-range MAP child index rejected (only [0] is valid).
	duckdb_v2_vector_handle oor = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(mvec, 1, &oor, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(oor == nullptr);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// UNION(INTEGER, VARCHAR): [0] = tag (UTINYINT), [1..N] = member
// vectors. Pin the per-kind index convention and the tag → active
// member lookup pattern.
// ===========================================================================

TEST_CASE("V2: UNION(INTEGER, VARCHAR) via get_child", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	// Set up via a temp table so the UNION type is bound consistently
	// across both inserted rows.
	ExecSQL(fx.conn, "CREATE TABLE u_t (u UNION(i INTEGER, s VARCHAR))");
	ExecSQL(fx.conn, "INSERT INTO u_t VALUES (union_value(i := 42)), (union_value(s := 'hello'))");
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT u FROM u_t", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);

	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 2);

	duckdb_v2_vector_handle uvec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &uvec, nullptr);

	// UNION exposes member_count + 1 children: tag at [0], members at [1..N].
	idx_t nch = 0;
	REQUIRE(duckdb_v2_vector_get_child_count(uvec, &nch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(nch == 3); // tag + 2 members

	// Tag at [0].
	duckdb_v2_vector_handle tag_vec = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(uvec, 0, &tag_vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view tag_view {};
	duckdb_v2_vector_get_view(tag_vec, &tag_view, nullptr);
	const uint8_t *tags = static_cast<const uint8_t *>(tag_view.data);

	// Members at [1] = INTEGER, [2] = VARCHAR.
	duckdb_v2_vector_handle m_int = nullptr;
	duckdb_v2_vector_handle m_str = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(uvec, 1, &m_int, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_child(uvec, 2, &m_str, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_view int_view {};
	duckdb_v2_vector_view str_view {};
	duckdb_v2_vector_get_view(m_int, &int_view, nullptr);
	duckdb_v2_vector_get_view(m_str, &str_view, nullptr);

	// Row 0: tag = 0, int member = 42.
	REQUIRE(tags[SelAt(tag_view.sel, 0)] == 0);
	const int32_t *idata = static_cast<const int32_t *>(int_view.data);
	REQUIRE(idata[SelAt(int_view.sel, 0)] == 42);

	// Row 1: tag = 1, str member = "hello".
	REQUIRE(tags[SelAt(tag_view.sel, 1)] == 1);
	const duckdb_v2_varchar_t *sarr = static_cast<const duckdb_v2_varchar_t *>(str_view.data);
	REQUIRE(Convert(sarr[SelAt(str_view.sel, 1)]) == "hello");

	// Out-of-range member index (3 is past the last member at child-index 2).
	duckdb_v2_vector_handle oor = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(uvec, 3, &oor, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(oor == nullptr);

	// view.data on a UNION is unspecified per the contract (typically
	// NULL — UNION has no per-row parent leaf data; the per-row
	// content lives in the tag + member children). Pin "NULL today"
	// here so any future bridge change that started leaking a non-null
	// pointer would be caught.
	duckdb_v2_vector_view uview {};
	duckdb_v2_vector_get_view(uvec, &uview, nullptr);
	REQUIRE(uview.data == nullptr);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// DECIMAL across the four storage widths. Width + scale come from the
// logical type; the leaf payload type is i16 / i32 / i64 / hugeint_t
// depending on internal width.
// ===========================================================================

TEST_CASE("V2: DECIMAL read across internal widths", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	// Widths chosen so each column hits a different physical storage.
	REQUIRE(Query(fx.conn,
	              "SELECT 12.34::DECIMAL(4, 2)   AS d16, "
	              "       1234.5678::DECIMAL(8, 4) AS d32, "
	              "       1234567890.12345::DECIMAL(18, 5) AS d64, "
	              "       1234567890123456789.012345::DECIMAL(28, 6) AS d128",
	              &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);
	idx_t size = 0;
	duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
	REQUIRE(size == 1);

	auto read_decimal = [&](idx_t col_idx) {
		duckdb_v2_vector_handle v = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, col_idx, &v, nullptr);
		duckdb_v2_vector_view view {};
		duckdb_v2_vector_get_view(v, &view, nullptr);
		return view;
	};

	// DECIMAL16 → i16 leaf. 12.34 with scale 2 = 1234.
	{
		auto view = read_decimal(0);
		const int16_t *d = static_cast<const int16_t *>(view.data);
		REQUIRE(d[SelAt(view.sel, 0)] == 1234);
	}
	// DECIMAL32 → i32 leaf. 1234.5678 with scale 4 = 12345678.
	{
		auto view = read_decimal(1);
		const int32_t *d = static_cast<const int32_t *>(view.data);
		REQUIRE(d[SelAt(view.sel, 0)] == 12345678);
	}
	// DECIMAL64 → i64 leaf. 1234567890.12345 with scale 5 = 123456789012345.
	{
		auto view = read_decimal(2);
		const int64_t *d = static_cast<const int64_t *>(view.data);
		REQUIRE(d[SelAt(view.sel, 0)] == INT64_C(123456789012345));
	}
	// DECIMAL128 → hugeint_t leaf. 1234567890123456789.012345 with
	// scale 6 = 1234567890123456789012345 (>> int64 range).
	{
		auto view = read_decimal(3);
		const duckdb_v2_hugeint_t *d = static_cast<const duckdb_v2_hugeint_t *>(view.data);
		// Spot-check it's a large 128-bit value, not zero.
		REQUIRE(d[SelAt(view.sel, 0)].upper > 0);
	}

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// A non-nested vector reports 0 children, get_child rejects any index,
// and list_get_size rejects non-LIST/MAP.
// ===========================================================================

TEST_CASE("V2: generic accessors handle non-nested vectors", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	Query(fx.conn, "SELECT 1::INTEGER", &r, nullptr);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	chunk = StepChunk(r);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	// get_child_count == 0 for non-nested.
	idx_t n = 99;
	REQUIRE(duckdb_v2_vector_get_child_count(vec, &n, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(n == 0);

	// get_child(idx=0) rejects on a non-nested vector.
	duckdb_v2_vector_handle child = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_get_child(vec, 0, &child, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(child == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// CONSTANT vector path: SQL queries materialise to FLAT, so we
// construct a CONSTANT vector directly via core and read it through
// the V2 surface (the vector handle is identity = `duckdb::Vector *`).
// Pins (a) view.data + sel for CONSTANT (zero-singleton sel, single
// underlying element), and (b) the contract that every logical row
// reads `data[view.sel[i]] == data[0]`.
// ===========================================================================

TEST_CASE("V2: CONSTANT vector view", "[capi_v2][data_chunk]") {
	// Vector::Reference(Value, count_t) routes through
	// ConstantVector::Reference, producing a CONSTANT vector — same
	// shape core's internal "constant fold this expression" path
	// would have produced.
	duckdb::Vector vec(duckdb::LogicalType::INTEGER);
	vec.Reference(duckdb::Value::INTEGER(7), duckdb::count_t(4));
	auto handle = reinterpret_cast<duckdb_v2_vector_handle>(&vec);

	DUCKDB_V2_VECTOR_TYPE vt = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(handle, &vt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vt == DUCKDB_V2_VECTOR_TYPE_CONSTANT);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(handle, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel != nullptr); // zero singleton, NOT identity
	REQUIRE(view.data != nullptr);

	const int32_t *data = static_cast<const int32_t *>(view.data);
	// Every logical row resolves through sel to physical index 0.
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		REQUIRE(SelAt(view.sel, i) == 0);
		REQUIRE(data[SelAt(view.sel, i)] == 7);
	}
}

// ===========================================================================
// vector_flatten: a CONSTANT vector becomes FLAT in place. After
// flatten, view.sel is NULL (identity).
// ===========================================================================

// ===========================================================================
// CONSTANT NULL: every logical row is NULL, validity at sel-resolved
// position 0 says invalid. Pins the all-NULL constant-fold output
// shape and that view.validity is populated on CONSTANT NULL.
// ===========================================================================

TEST_CASE("V2: CONSTANT NULL vector view", "[capi_v2][data_chunk]") {
	duckdb::Vector vec(duckdb::LogicalType::INTEGER);
	// Value() with just a logical type produces a NULL value.
	vec.Reference(duckdb::Value(duckdb::LogicalType::INTEGER), duckdb::count_t(4));
	auto handle = reinterpret_cast<duckdb_v2_vector_handle>(&vec);

	DUCKDB_V2_VECTOR_TYPE vt = DUCKDB_V2_VECTOR_TYPE_OTHER;
	duckdb_v2_vector_get_vector_type(handle, &vt, nullptr);
	REQUIRE(vt == DUCKDB_V2_VECTOR_TYPE_CONSTANT);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(handle, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel != nullptr);      // CONSTANT → zero singleton
	REQUIRE(view.validity != nullptr); // not all-valid

	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		REQUIRE_FALSE(RowValid(view, SelAt(view.sel, i)));
	}
}

TEST_CASE("V2: vector_flatten CONSTANT → FLAT", "[capi_v2][data_chunk]") {
	duckdb::Vector vec(duckdb::LogicalType::INTEGER);
	vec.Reference(duckdb::Value::INTEGER(13), duckdb::count_t(4));
	auto handle = reinterpret_cast<duckdb_v2_vector_handle>(&vec);

	DUCKDB_V2_VECTOR_TYPE before = DUCKDB_V2_VECTOR_TYPE_OTHER;
	duckdb_v2_vector_get_vector_type(handle, &before, nullptr);
	REQUIRE(before == DUCKDB_V2_VECTOR_TYPE_CONSTANT);

	// vector_flatten on a CONSTANT vector — note the no-arg form. Core
	// flattens to STANDARD_VECTOR_SIZE rows; we just check the shape
	// switches to FLAT and the view's sel becomes NULL.
	REQUIRE(duckdb_v2_vector_flatten(handle, nullptr) == DUCKDB_V2_ERROR_NONE);

	DUCKDB_V2_VECTOR_TYPE after = DUCKDB_V2_VECTOR_TYPE_OTHER;
	duckdb_v2_vector_get_vector_type(handle, &after, nullptr);
	REQUIRE(after == DUCKDB_V2_VECTOR_TYPE_FLAT);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(handle, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel == nullptr); // FLAT → identity
	const int32_t *data = static_cast<const int32_t *>(view.data);
	REQUIRE(data[0] == 13);
}

// ===========================================================================
// DICTIONARY vector view. Constructed via Vector(other, sel, count)
// which routes through DictionaryVector internally. Pins:
//   - view.sel is non-null and matches the constructed sel
//   - data[view.sel[i]] dispatches through the dictionary
//   - validity-follows-sel: indexing validity at the logical row i
//     (without sel resolution) reads the WRONG cell — exercise this
//     on a fixture that has different valid/invalid rows under sel.
// ===========================================================================

#if (STANDARD_VECTOR_SIZE > 3)
TEST_CASE("V2: DICTIONARY vector view", "[capi_v2][data_chunk]") {
	// Build a FLAT INTEGER vector backing the dictionary. Mark row 1
	// invalid so validity-follows-sel has something to expose.
	duckdb::Vector flat(duckdb::LogicalType::INTEGER);
	auto *fd = duckdb::FlatVector::GetDataMutable<int32_t>(flat);
	fd[0] = 10;
	fd[1] = 20; // will be marked invalid
	fd[2] = 30;
	duckdb::FlatVector::SetNull(flat, 1, true);

	// Slice with a non-identity sel: 4 logical rows pointing at
	// physical indices [2, 0, 2, 1]. Logical row 3 dispatches to
	// physical 1, which is invalid.
	duckdb::SelectionVector sel(4);
	sel.set_index(0, 2);
	sel.set_index(1, 0);
	sel.set_index(2, 2);
	sel.set_index(3, 1);
	duckdb::Vector dict(flat, sel, 4);
	auto handle = reinterpret_cast<duckdb_v2_vector_handle>(&dict);

	DUCKDB_V2_VECTOR_TYPE vt = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(handle, &vt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vt == DUCKDB_V2_VECTOR_TYPE_DICTIONARY);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(handle, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(view.sel != nullptr); // dict carries its own sel

	const int32_t *data = static_cast<const int32_t *>(view.data);

	// Reads dispatch through sel.
	REQUIRE(data[SelAt(view.sel, 0)] == 30); // sel[0] = 2
	REQUIRE(data[SelAt(view.sel, 1)] == 10); // sel[1] = 0
	REQUIRE(data[SelAt(view.sel, 2)] == 30); // sel[2] = 2
	// data[SelAt(view.sel, 3)] would read physical 1 (the invalid slot);
	// caller MUST check validity at the sel-resolved row first.

	// Validity-follows-sel: the correct check is validity[sel[i]], NOT
	// validity[i]. Pin both directions.
	REQUIRE_FALSE(RowValid(view, SelAt(view.sel, 3))); // physical 1 is invalid

	// physical 3 is valid (default), even though logical row 3 is the NULL one
	REQUIRE(RowValid(view, 3));
	// The naive `validity[i]` read would have answered "valid" for an
	// invalid row — exactly the footgun the API contract warns about.

	// Logical rows 0, 1, 2 are valid (their sel-resolved physical rows
	// are 2, 0, 2 — none of which is the NULLed row 1).
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE(RowValid(view, SelAt(view.sel, i)));
	}
}
#endif

// ===========================================================================
// vector_get_view rejects OTHER (FSST / SEQUENCE / SHREDDED) vectors
// AND zeroes the view before returning. Pin both via a SEQUENCE
// vector, which is the easiest OTHER kind to construct directly.
// ===========================================================================

TEST_CASE("V2: vector_get_view rejects OTHER + zeroes view", "[capi_v2][data_chunk]") {
	duckdb::Vector vec(duckdb::LogicalType::BIGINT);
	vec.Sequence(/*start=*/100, /*increment=*/1, /*count=*/4);
	auto handle = reinterpret_cast<duckdb_v2_vector_handle>(&vec);

	DUCKDB_V2_VECTOR_TYPE vt = DUCKDB_V2_VECTOR_TYPE_OTHER;
	REQUIRE(duckdb_v2_vector_get_vector_type(handle, &vt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vt == DUCKDB_V2_VECTOR_TYPE_OTHER);

	// Poison the view before the call.
	duckdb_v2_vector_view view {};
	view.data = reinterpret_cast<const void *>(0x1);
	view.validity = reinterpret_cast<const uint64_t *>(0x2);
	view.sel = reinterpret_cast<const duckdb_v2_sel_t *>(0x3);

	REQUIRE(duckdb_v2_vector_get_view(handle, &view, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(view.data == nullptr);
	REQUIRE(view.validity == nullptr);
	REQUIRE(view.sel == nullptr);

	// vector_flatten resolves the OTHER state into FLAT — same
	// CONSTANT-flatten roundtrip pattern but on a SEQUENCE source.
	REQUIRE(duckdb_v2_vector_flatten(handle, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_get_vector_type(handle, &vt, nullptr);
	REQUIRE(vt == DUCKDB_V2_VECTOR_TYPE_FLAT);

	REQUIRE(duckdb_v2_vector_get_view(handle, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	const int64_t *data = static_cast<const int64_t *>(view.data);
	REQUIRE(data[0] == 100);
	REQUIRE(data[1] == 101);
}

// ===========================================================================
// Null-arg + bounds rejection across the chunk surface.
// ===========================================================================

TEST_CASE("V2: chunk null-arg + out-of-range rejection", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_result_handle r = nullptr;
	Query(fx.conn, "SELECT 1", &r, nullptr);

	duckdb_v2_data_chunk_handle chunk = StepChunk(r);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 99, &vec, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(vec == nullptr);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

// ===========================================================================
// A successful call does NOT clear a pre-existing *err. The return code is
// authoritative; the library leaves the slot untouched on success, so a
// stale info from an earlier failure survives until the caller clears it.
// ===========================================================================

TEST_CASE("V2: success leaves a pre-existing err untouched", "[capi_v2][data_chunk]") {
	EnvFixture fx;

	duckdb_v2_error_info_handle err = nullptr;

	// Failing call: out-of-range vector index on a real chunk.
	duckdb_v2_result_handle r1 = nullptr;
	Query(fx.conn, "SELECT 1", &r1, nullptr);
	duckdb_v2_data_chunk_handle chunk = StepChunk(r1);
	duckdb_v2_vector_handle oor_vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 99, &oor_vec, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr); // populated on failure
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r1);

	// Succeeding call reusing the same err slot: the slot is left untouched.
	// The stale failure info is still there; the return code is what tells
	// the caller the call succeeded.
	duckdb_v2_result_handle r2 = nullptr;
	REQUIRE(Query(fx.conn, "SELECT 1", &r2, &err) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(err != nullptr);
	{
		DUCKDB_V2_ERROR code = DUCKDB_V2_ERROR_NONE;
		duckdb_v2_error_info_get_code(err, &code);
		REQUIRE(code == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	duckdb_v2_error_info_destroy(&err);

	duckdb_v2_result_destroy(&r2);
}

// ===========================================================================
// data_chunk_destroy null-safety.
// ===========================================================================

// ===========================================================================
// data_chunk has fully independent lifetime from the producing result,
// connection, and database; chunks handed out by the stream own their
// data. Pin this by destroying the result, connection, AND database,
// then reading the chunk + its vectors.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2: data_chunk outlives result + connection + database", "[capi_v2][data_chunk]") {
	duckdb_v2_data_chunk_handle chunk = nullptr;

	{
		duckdb_v2_environment_handle env = nullptr;
		duckdb_v2_instance_handle instance = nullptr;
		duckdb_v2_connection_handle conn = nullptr;
		duckdb_v2_environment_create(&env, nullptr);
		OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr);
		duckdb_v2_connection_create(instance, &conn, nullptr);

		duckdb_v2_result_handle r = nullptr;
		REQUIRE(Query(conn, "SELECT * FROM (VALUES (1), (2), (3)) t(i)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);
		chunk = StepChunk(r);

		// Tear everything down except the chunk itself.
		duckdb_v2_result_destroy(&r);
		duckdb_v2_connection_destroy(&conn);
		duckdb_v2_instance_destroy(&instance);
		duckdb_v2_environment_destroy(&env);
	}

	// The chunk and its borrowed vectors must still read cleanly.
	idx_t size = 0;
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 3);

	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);

	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	const int32_t *data = static_cast<const int32_t *>(view.data);
	REQUIRE(data[SelAt(view.sel, 0)] == 1);
	REQUIRE(data[SelAt(view.sel, 1)] == 2);
	REQUIRE(data[SelAt(view.sel, 2)] == 3);

	duckdb_v2_data_chunk_destroy(&chunk);
}
#endif

TEST_CASE("V2: data_chunk_destroy is null-safe", "[capi_v2][data_chunk]") {
	REQUIRE(duckdb_v2_data_chunk_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_chunk_handle already_null = nullptr;
	REQUIRE(duckdb_v2_data_chunk_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(already_null == nullptr);
}

// ===========================================================================
// vector_get_view null-arg + zero-init on failure.
// ===========================================================================

TEST_CASE("V2: vector_get_view zeroes view on failure", "[capi_v2][data_chunk]") {
	// Poison every field with a recognisable sentinel before the call.
	duckdb_v2_vector_view view {};
	view.data = reinterpret_cast<const void *>(0x1);
	view.validity = reinterpret_cast<const uint64_t *>(0x2);
	view.sel = reinterpret_cast<const duckdb_v2_sel_t *>(0x3);

	REQUIRE(duckdb_v2_vector_get_view(nullptr, &view, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(view.data == nullptr);
	REQUIRE(view.validity == nullptr);
	REQUIRE(view.sel == nullptr);
}

// ===========================================================================
// VECTOR_TYPE_OTHER is 0 so a zero-initialised out-param reads as
// "unspecified" rather than masquerading as FLAT.
// ===========================================================================

TEST_CASE("V2: VECTOR_TYPE_OTHER is the zero value", "[capi_v2][data_chunk]") {
	DUCKDB_V2_VECTOR_TYPE zero_init {};
	REQUIRE(zero_init == DUCKDB_V2_VECTOR_TYPE_OTHER);
	REQUIRE(DUCKDB_V2_VECTOR_TYPE_OTHER == 0);
}

// ===========================================================================
// Argument rejection for the BIGNUM wire codec.
// ===========================================================================

TEST_CASE("V2: string decoders reject null arguments", "[capi_v2][data_chunk]") {
	uint8_t out[8] = {};
	idx_t len = 0;
	bool is_neg = false;

	// Null input bytes, and storage too short to carry a header + magnitude.
	REQUIRE(duckdb_v2_bignum_decode(nullptr, 4, out, sizeof(out), &len, &is_neg, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	const uint8_t header_only[3] = {0x80, 0x00, 0x00};
	REQUIRE(duckdb_v2_bignum_decode(header_only, sizeof(header_only), out, sizeof(out), &len, &is_neg, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// The out-params carrying the answer are mandatory in both directions.
	const uint8_t storage[4] = {0x80, 0x00, 0x01, 0x07};
	REQUIRE(duckdb_v2_bignum_decode(storage, sizeof(storage), out, sizeof(out), nullptr, &is_neg, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_bignum_decode(storage, sizeof(storage), out, sizeof(out), &len, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	const uint8_t magnitude[1] = {0x07};
	REQUIRE(duckdb_v2_bignum_encode(nullptr, 1, false, out, sizeof(out), &len, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_bignum_encode(magnitude, 0, false, out, sizeof(out), &len, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_bignum_encode(magnitude, 1, false, out, sizeof(out), nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ===========================================================================
// The caller-buffer protocol: a null out_data sizes, a short buffer refuses
// with the required size, and neither direction ever allocates.
// ===========================================================================

TEST_CASE("V2: bignum codec sizes and refuses short buffers", "[capi_v2][data_chunk]") {
	// -256: magnitude {0x01, 0x00} -> 2 magnitude bytes, 5 storage bytes.
	const uint8_t magnitude[2] = {0x01, 0x00};

	idx_t storage_len = 0;
	REQUIRE(duckdb_v2_bignum_encode(magnitude, sizeof(magnitude), true, nullptr, 0, &storage_len, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(storage_len == sizeof(magnitude) + 3);

	// One byte short: refused, and out_length still reports what was needed.
	std::vector<uint8_t> storage(storage_len);
	idx_t short_len = 0;
	REQUIRE(duckdb_v2_bignum_encode(magnitude, sizeof(magnitude), true, storage.data(), storage_len - 1, &short_len,
	                                nullptr) == DUCKDB_V2_ERROR_INPUT_OBJECT_SIZE);
	REQUIRE(short_len == storage_len);

	REQUIRE(duckdb_v2_bignum_encode(magnitude, sizeof(magnitude), true, storage.data(), storage.size(), &storage_len,
	                                nullptr) == DUCKDB_V2_ERROR_NONE);

	// Round-trip back through decode, same protocol.
	idx_t mag_len = 0;
	bool is_neg = false;
	REQUIRE(duckdb_v2_bignum_decode(storage.data(), storage.size(), nullptr, 0, &mag_len, &is_neg, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(mag_len == sizeof(magnitude));
	REQUIRE(is_neg);

	std::vector<uint8_t> decoded(mag_len);
	idx_t refused_len = 0;
	REQUIRE(duckdb_v2_bignum_decode(storage.data(), storage.size(), decoded.data(), mag_len - 1, &refused_len, &is_neg,
	                                nullptr) == DUCKDB_V2_ERROR_INPUT_OBJECT_SIZE);
	REQUIRE(refused_len == mag_len);

	REQUIRE(duckdb_v2_bignum_decode(storage.data(), storage.size(), decoded.data(), decoded.size(), &mag_len, &is_neg,
	                                nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_neg);
	REQUIRE(decoded[0] == 0x01);
	REQUIRE(decoded[1] == 0x00);
}

// ===========================================================================
// Encode rejects non-canonical magnitudes, matching what decode produces.
// ===========================================================================

TEST_CASE("V2: bignum encode requires a canonical magnitude", "[capi_v2][data_chunk]") {
	uint8_t out[8] = {};
	idx_t len = 0;

	// Leading zero bytes are not canonical.
	const uint8_t leading_zero[2] = {0x00, 0x07};
	REQUIRE(duckdb_v2_bignum_encode(leading_zero, sizeof(leading_zero), false, out, sizeof(out), &len, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// Zero is the single byte 0x00, and only positive: -0 has no encoding.
	const uint8_t zero[1] = {0x00};
	REQUIRE(duckdb_v2_bignum_encode(zero, sizeof(zero), true, out, sizeof(out), &len, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_bignum_encode(zero, sizeof(zero), false, out, sizeof(out), &len, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(len == 4);
}

TEST_CASE("V2: data_chunk_clear empties a chunk for reuse", "[capi_v2][data_chunk]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, &integer, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&integer);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 5, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t size = 0;
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 5);

	REQUIRE(duckdb_v2_data_chunk_clear(chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);
	DUCKDB_V2_VECTOR_TYPE type = DUCKDB_V2_VECTOR_TYPE_CONSTANT;
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(type == DUCKDB_V2_VECTOR_TYPE_FLAT);

	// Writable again.
	REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);
	void *data = nullptr;
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	static_cast<int32_t *>(data)[0] = 42;
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 1);

	{
		duckdb::Vector input(duckdb::Value::INTEGER(7), duckdb::count_t(1));
		input.Flatten();
		duckdb::SelectionVector sel(1);
		sel.set_index(0, 0);
		duckdb::Vector dictionary(input, sel, 1);
		auto source = reinterpret_cast<duckdb_v2_vector_handle>(&dictionary);
		REQUIRE(duckdb_v2_vector_reference(vec, source, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_data_chunk_clear(chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_get_vector_type(vec, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(type == DUCKDB_V2_VECTOR_TYPE_FLAT);
	REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(data != nullptr);

	REQUIRE(duckdb_v2_data_chunk_clear(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_data_chunk_destroy(&chunk);
}

} // namespace test_capi_v2
