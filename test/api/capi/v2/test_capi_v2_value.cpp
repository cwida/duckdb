#include "test_capi_v2.hpp"

#include "duckdb.h" // V1 C API: used only by the [v1_v2_bridge] cross-API parity pin.

// ---------------------------------------------------------------------------
// V2 value tests — primitive surface.
//
// Same identity-cast invariant as the logical_type bridge: both V1
// duckdb_value and V2 duckdb_v2_value_handle are heap-allocated duckdb::Value
// cast to void *. We rely on it to round-trip a V1-built fixture through V2
// destroy in one place (the V1-built leaf test), the same way the
// logical_type tests reuse V1 fixtures. If a wrapper is added later, this
// file must change.
//
// Borrow contract being verified throughout: the string value_get_varchar and
// value_get_blob hand back stays valid until value_destroy is called. That
// holds for BIGNUM too — what it borrows is the opaque storage, which
// bignum_decode then translates into a magnitude in a buffer the caller owns.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
namespace {

duckdb_v2_value_handle V1ValueToV2(duckdb_value v) {
	return reinterpret_cast<duckdb_v2_value_handle>(v);
}

} // namespace

// ===========================================================================
// Lifecycle: destroy null-safety
// ===========================================================================

TEST_CASE("V2: value destroy is null-safe", "[capi_v2][value][lifecycle]") {
	REQUIRE(duckdb_v2_value_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle already_null = nullptr;
	REQUIRE(duckdb_v2_value_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(already_null == nullptr);
}

// ===========================================================================
// NULL construction + is_null + get_logical_type ownership
// ===========================================================================

TEST_CASE("V2: value_create_null carries the borrowed type", "[capi_v2][value][null]") {
	EnvFixture fx;
	duckdb_v2_logical_type_handle int_type = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &int_type, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, int_type, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(v != nullptr);

	// Input type is borrowed: destroying it must not affect the NULL value.
	REQUIRE(duckdb_v2_logical_type_destroy(&int_type) == DUCKDB_V2_ERROR_NONE);

	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(v, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);

	duckdb_v2_logical_type_handle out_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(v, &out_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(out_type != nullptr);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(out_type, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	// Caller owns the returned logical type.
	REQUIRE(duckdb_v2_logical_type_destroy(&out_type) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_value_destroy(&v) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: value_create_null rejects null type / null out", "[capi_v2][value][null]") {
	EnvFixture fx;
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);

	duckdb_v2_logical_type_handle int_type = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &int_type,
	                                      nullptr);
	REQUIRE(duckdb_v2_value_create_null(fx.factory, int_type, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&int_type);
}

TEST_CASE("V2: value constructors reject a null payload pointer", "[capi_v2][value][null]") {
	EnvFixture fx;
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_hugeint(fx.factory, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_uhugeint(fx.factory, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_interval(fx.factory, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, nullptr, 18, 3, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);
}

TEST_CASE("V2: value_is_null distinguishes NULL from non-NULL", "[capi_v2][value][null]") {
	EnvFixture fx;
	duckdb_v2_value_handle v = nullptr;
	v = MakeInt32Value(fx.factory, 7);
	bool is_null = true;
	REQUIRE(duckdb_v2_value_is_null(v, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(!is_null);
	duckdb_v2_value_destroy(&v);
}

TEST_CASE("V2: value_is_null / value_get_logical_type / value_destroy null guards", "[capi_v2][value][null]") {
	EnvFixture fx;
	bool b = false;
	REQUIRE(duckdb_v2_value_is_null(nullptr, &b, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_logical_type_handle lt = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(nullptr, &lt, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(lt == nullptr);

	duckdb_v2_value_handle v = nullptr;
	v = MakeInt32Value(fx.factory, 1);
	REQUIRE(duckdb_v2_value_is_null(v, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_get_logical_type(v, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&v);
}

// ===========================================================================
// Typed constructors + typed getters
//
// One pair per built-in whose payload is a plain C scalar or byte string.
// The constructors are scoped to a connection (a context in a bind callback);
// the getters need no scope, and read exactly their own type id.
// ===========================================================================

namespace {

// Builds a value through its typed constructor, reads it back through the
// matching typed getter, and optionally pins the rendering. Reads everything
// first, then destroys, then asserts: a failing REQUIRE in between would leak.
template <class T, class MAKE>
void RequireTypedRoundTrip(MAKE make, T payload, const char *expected_text = nullptr) {
	duckdb_v2_value_handle v = nullptr;
	auto create_rc = make(payload, &v);

	T out {};
	auto read_rc = create_rc == DUCKDB_V2_ERROR_NONE ? GetTypedValue(v, &out) : create_rc;
	auto render_rc = DUCKDB_V2_ERROR_NONE;
	std::string rendered;
	if (expected_text && create_rc == DUCKDB_V2_ERROR_NONE) {
		rendered = RenderText(
		    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_value_to_string(v, buf, cap, len, nullptr); },
		    render_rc);
	}
	duckdb_v2_value_destroy(&v);

	REQUIRE(create_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(read_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::memcmp(&out, &payload, sizeof(T)) == 0);
	if (expected_text) {
		REQUIRE(render_rc == DUCKDB_V2_ERROR_NONE);
		REQUIRE(rendered == expected_text);
	}
}

} // namespace

TEST_CASE("V2: typed scalars round-trip through their constructor / getter pair", "[capi_v2][value][typed]") {
	EnvFixture fx;
	auto factory = fx.factory;

	RequireTypedRoundTrip<bool>(
	    [&](bool x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_bool(factory, x, v, nullptr); }, true,
	    "true");
	RequireTypedRoundTrip<int8_t>(
	    [&](int8_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_tinyint(factory, x, v, nullptr); }, -5,
	    "-5");
	RequireTypedRoundTrip<int16_t>(
	    [&](int16_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_smallint(factory, x, v, nullptr); },
	    -1234, "-1234");
	RequireTypedRoundTrip<int32_t>(
	    [&](int32_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_int(factory, x, v, nullptr); },
	    -123456, "-123456");
	RequireTypedRoundTrip<int64_t>(
	    [&](int64_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_bigint(factory, x, v, nullptr); },
	    -1234567890123ll, "-1234567890123");
	RequireTypedRoundTrip<uint8_t>(
	    [&](uint8_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_utinyint(factory, x, v, nullptr); },
	    200, "200");
	RequireTypedRoundTrip<uint16_t>(
	    [&](uint16_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_usmallint(factory, x, v, nullptr); },
	    60000, "60000");
	RequireTypedRoundTrip<uint32_t>(
	    [&](uint32_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_uint(factory, x, v, nullptr); },
	    4000000000u, "4000000000");
	RequireTypedRoundTrip<uint64_t>(
	    [&](uint64_t x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_ubigint(factory, x, v, nullptr); },
	    18000000000000000000ull, "18000000000000000000");
	RequireTypedRoundTrip<float>(
	    [&](float x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_float(factory, x, v, nullptr); }, 1.5f,
	    "1.5");
	RequireTypedRoundTrip<double>(
	    [&](double x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_double(factory, x, v, nullptr); },
	    -2.25, "-2.25");
}

TEST_CASE("V2: HUGEINT and UHUGEINT carry both 128-bit halves", "[capi_v2][value][typed]") {
	EnvFixture fx;
	auto factory = fx.factory;
	auto make_hugeint = [&](duckdb_v2_hugeint_t x, duckdb_v2_value_handle *v) {
		return duckdb_v2_value_create_hugeint(factory, &x, v, nullptr);
	};
	auto make_uhugeint = [&](duckdb_v2_uhugeint_t x, duckdb_v2_value_handle *v) {
		return duckdb_v2_value_create_uhugeint(factory, &x, v, nullptr);
	};

	RequireTypedRoundTrip<duckdb_v2_hugeint_t>(make_hugeint, {42, 0}, "42");
	// Negative: all-ones lower with upper -1 is the two's-complement -1.
	RequireTypedRoundTrip<duckdb_v2_hugeint_t>(make_hugeint, {0xFFFFFFFFFFFFFFFFull, -1}, "-1");
	RequireTypedRoundTrip<duckdb_v2_uhugeint_t>(make_uhugeint, {7, 1}, "18446744073709551623");
}

TEST_CASE("V2: VARCHAR round-trips its bytes and borrows them", "[capi_v2][value][typed][borrow]") {
	EnvFixture fx;
	// An embedded NUL: the payload is a byte range, not a C string.
	const char raw[4] = {'a', '\0', 'b', 'c'};
	duckdb_v2_value_handle v = nullptr;
	auto value_str = duckdb_v2_str {raw, 4};
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &value_str, &v, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The borrow is stable: two reads hand back the same pointer.
	duckdb_v2_str first = {nullptr, 0};
	duckdb_v2_str second = {nullptr, 0};
	REQUIRE(duckdb_v2_value_get_varchar(v, &first, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(first.len == 4);
	REQUIRE(std::memcmp(first.ptr, raw, 4) == 0);
	REQUIRE(duckdb_v2_value_get_varchar(v, &second, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(second.ptr == first.ptr);
	duckdb_v2_value_destroy(&v);

	// Empty: a null pointer is valid when the length is 0.
	auto value_str2 = duckdb_v2_str {nullptr, 0};
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &value_str2, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<std::string>(v).empty());

	// The engine rejects invalid UTF-8 at construction.
	const char bad_utf8[2] = {'\xC0', '\x00'};
	duckdb_v2_value_handle bad = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto value_str3 = duckdb_v2_str {bad_utf8, 2};
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &value_str3, &bad, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(bad == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2: BLOB takes any bytes and reads back through get_blob", "[capi_v2][value][typed][borrow]") {
	EnvFixture fx;
	const char blob_bytes[5] = {'\x00', '\xFF', '\x10', '\x00', '\x7F'};
	auto v = MakeBlobValue(fx.factory, blob_bytes, 5);
	REQUIRE(ConsumeBlob(v) == std::string(blob_bytes, 5));

	auto empty = MakeBlobValue(fx.factory, nullptr, 0);
	REQUIRE(ConsumeBlob(empty).empty());

	// VARCHAR is text and has its own getter; the two do not stand in for
	// each other.
	auto text = MakeVarcharValue(fx.factory, "abc");
	duckdb_v2_str out = {nullptr, 0};
	REQUIRE(duckdb_v2_value_get_blob(text, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&text);
}

TEST_CASE("V2: typed getters refuse a mismatched type and a NULL value", "[capi_v2][value][typed]") {
	EnvFixture fx;
	// A mismatched type converts through the default cast set, so an INTEGER
	// reads fine as a BIGINT.
	auto v = MakeInt32Value(fx.factory, 42);
	int64_t wide = 0;
	REQUIRE(duckdb_v2_value_get_bigint(v, &wide, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(wide == 42);

	// VARCHAR is the exception: it hands back a pointer into the value, and a
	// converted copy would not outlive the call, so it stays exact.
	duckdb_v2_str str = {nullptr, 0};
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_get_varchar(v, &str, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&v);

	// A conversion the engine cannot do is the error.
	auto text = MakeVarcharValue(fx.factory, "not a number");
	int32_t narrow_out = 0;
	REQUIRE(duckdb_v2_value_get_int(text, &narrow_out, nullptr) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&text);

	// Reading is non-destructive: the value is unchanged by a converting read.
	auto probe_value = MakeInt32Value(fx.factory, 7);
	int64_t as_bigint = 0;
	REQUIRE(duckdb_v2_value_get_bigint(probe_value, &as_bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
	int32_t still_int = 0;
	REQUIRE(duckdb_v2_value_get_int(probe_value, &still_int, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(still_int == 7);
	duckdb_v2_value_destroy(&probe_value);

	// A NULL has no payload to hand back.
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_value_handle null_value = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, int_type, &null_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	int32_t narrow = 7;
	REQUIRE(duckdb_v2_value_get_int(null_value, &narrow, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&null_value);
	duckdb_v2_logical_type_destroy(&int_type);

	// Composites have no scalar payload; descent is value_get_child.
	auto elem_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_value_handle list = nullptr;
	REQUIRE(duckdb_v2_value_create_list(fx.factory, elem_type, nullptr, 0, &list, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_get_int(list, &narrow, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&list);
	duckdb_v2_logical_type_destroy(&elem_type);
}

TEST_CASE("V2: typed constructors and getters null-arg refusals", "[capi_v2][value][typed]") {
	EnvFixture fx;
	duckdb_v2_value_handle v = nullptr;

	// The scope handle is mandatory: it is what binds the value to a catalog.
	REQUIRE(duckdb_v2_value_create_int(nullptr, 1, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);
	REQUIRE(duckdb_v2_value_create_int(nullptr, 1, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 1, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A null byte range is only valid empty.
	auto value_str = duckdb_v2_str {nullptr, 4};
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &value_str, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);

	int32_t out = 0;
	REQUIRE(duckdb_v2_value_get_int(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto probe = MakeInt32Value(fx.factory, 1);
	REQUIRE(duckdb_v2_value_get_int(probe, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_str str = {nullptr, 0};
	REQUIRE(duckdb_v2_value_get_varchar(nullptr, &str, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_get_blob(probe, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&probe);
}

// ===========================================================================
// Temporal constructors + getters: each carries its own unit, and no calendar
// conversion happens at the boundary.
// ===========================================================================

namespace {

// Builds through the typed constructor, reads back through the matching
// getter, and pins the rendering, so the unit is checked from both sides.
template <class T, class MAKE, class READ>
void RequireTemporalRoundTrip(MAKE make, READ read, T payload, const char *expected_text) {
	duckdb_v2_value_handle v = nullptr;
	auto create_rc = make(payload, &v);
	T out {};
	auto read_rc = create_rc == DUCKDB_V2_ERROR_NONE ? read(v, &out) : create_rc;
	auto render_rc = DUCKDB_V2_ERROR_NONE;
	std::string rendered;
	if (create_rc == DUCKDB_V2_ERROR_NONE) {
		rendered = RenderText(
		    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_value_to_string(v, buf, cap, len, nullptr); },
		    render_rc);
	}
	duckdb_v2_value_destroy(&v);
	REQUIRE(create_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(read_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::memcmp(&out, &payload, sizeof(T)) == 0);
	REQUIRE(render_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rendered == expected_text);
}

} // namespace

#define V2_TEMPORAL_ROUND_TRIP(factory, kind, type, payload, text)                                                     \
	RequireTemporalRoundTrip<type>(                                                                                    \
	    [&](type x, duckdb_v2_value_handle *v) { return duckdb_v2_value_create_##kind(factory, x, v, nullptr); },      \
	    [](duckdb_v2_value_handle v, type *out) { return duckdb_v2_value_get_##kind(v, out, nullptr); }, payload,      \
	    text)

TEST_CASE("V2: temporal values round-trip through their constructor / getter pair", "[capi_v2][value][temporal]") {
	EnvFixture fx;
	auto factory = fx.factory;

	// DATE is int32 days since the epoch; 19797 = 2024-03-15.
	V2_TEMPORAL_ROUND_TRIP(factory, date, int32_t, 19797, "2024-03-15");

	// The TIME family counts from midnight in its own unit.
	const int64_t micros = ((12 * 60 + 34) * 60 + 56) * 1000000LL;
	V2_TEMPORAL_ROUND_TRIP(factory, time, int64_t, micros, "12:34:56");
	V2_TEMPORAL_ROUND_TRIP(factory, time_ns, int64_t, micros * 1000, "12:34:56");

	// The TIMESTAMP family counts from the epoch in its own unit, so the same
	// wall clock is a different integer in each.
	const int64_t day = 19797LL * 24 * 60 * 60;
	V2_TEMPORAL_ROUND_TRIP(factory, timestamp_sec, int64_t, day + 45296, "2024-03-15 12:34:56");
	V2_TEMPORAL_ROUND_TRIP(factory, timestamp_ms, int64_t, (day + 45296) * 1000LL, "2024-03-15 12:34:56");
	V2_TEMPORAL_ROUND_TRIP(factory, timestamp, int64_t, (day + 45296) * 1000000LL, "2024-03-15 12:34:56");
	V2_TEMPORAL_ROUND_TRIP(factory, timestamp_ns, int64_t, (day + 45296) * 1000000000LL, "2024-03-15 12:34:56");

	// INTERVAL is the (months, days, micros) triple.
	duckdb_v2_interval_t iv {1, 2, 3000000};
	RequireTemporalRoundTrip<duckdb_v2_interval_t>(
	    [&](duckdb_v2_interval_t x, duckdb_v2_value_handle *v) {
		    return duckdb_v2_value_create_interval(factory, &x, v, nullptr);
	    },
	    [](duckdb_v2_value_handle v, duckdb_v2_interval_t *out) {
		    return duckdb_v2_value_get_interval(v, out, nullptr);
	    },
	    iv, "1 month 2 days 00:00:03");
}

TEST_CASE("V2: TIME_TZ carries the packed time and offset", "[capi_v2][value][temporal]") {
	EnvFixture fx;
	// Micros in the high 40 bits, a biased offset in the low 24: the offset is
	// stored as MAX_OFFSET - seconds so the packed integer sorts.
	constexpr int32_t MAX_OFFSET = 16 * 60 * 60 - 1;
	const int64_t micros = ((12 * 60 + 34) * 60 + 56) * 1000000LL;
	const int32_t offset_seconds = 2 * 60 * 60;
	const uint64_t packed =
	    (static_cast<uint64_t>(micros) << 24) | (static_cast<uint64_t>(MAX_OFFSET - offset_seconds) & 0xFFFFFF);

	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_time_tz(fx.factory, packed, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(v) == "12:34:56+02");
	uint64_t out = 0;
	REQUIRE(duckdb_v2_value_get_time_tz(v, &out, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(out == packed);
	duckdb_v2_value_destroy(&v);
}

TEST_CASE("V2: temporal getters refuse a mismatched unit and a NULL value", "[capi_v2][value][temporal]") {
	EnvFixture fx;
	// The units are distinct types, so a TIMESTAMP is not readable as a
	// TIMESTAMP_NS even though both are int64: exactly why they are separate
	// entry points.
	// Units convert rather than reinterpret: every TIMESTAMP variant is stored
	// as an int64, so only the type id can tell micros from nanos.
	const int64_t micros = (19797LL * 24 * 60 * 60 + 45296) * 1000000LL;
	duckdb_v2_value_handle ts = nullptr;
	REQUIRE(duckdb_v2_value_create_timestamp(fx.factory, micros, &ts, nullptr) == DUCKDB_V2_ERROR_NONE);
	int64_t nanos = 0;
	REQUIRE(duckdb_v2_value_get_timestamp_ns(ts, &nanos, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(nanos == micros * 1000);
	// And a TIMESTAMP read as a DATE keeps the day, dropping the time.
	int32_t day_number = 0;
	REQUIRE(duckdb_v2_value_get_date(ts, &day_number, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(day_number == 19797);
	duckdb_v2_value_destroy(&ts);

	// A NULL has no payload to hand back.
	auto date_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DATE);
	duckdb_v2_value_handle null_date = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, date_type, &null_date, nullptr) == DUCKDB_V2_ERROR_NONE);
	int32_t days = 7;
	REQUIRE(duckdb_v2_value_get_date(null_date, &days, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&null_date);
	duckdb_v2_logical_type_destroy(&date_type);

	// The scope handle and out slots are mandatory, as everywhere else.
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_date(nullptr, 0, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_date(nullptr, 0, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_date(fx.factory, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_get_date(nullptr, &days, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto probe = MakeInt32Value(fx.factory, 1);
	REQUIRE(duckdb_v2_value_get_date(probe, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&probe);
}

// ===========================================================================
// The kinds outside the typed set: built through value_cast from a VARCHAR,
// which is the engine's own construction path for them.
// ===========================================================================

TEST_CASE("V2: temporal and UUID values are built from text", "[capi_v2][value][cast]") {
	EnvFixture fx;
	struct {
		DUCKDB_V2_LOGICAL_TYPE_ID id;
		const char *text;
	} cases[] = {
	    {DUCKDB_V2_LOGICAL_TYPE_ID_DATE, "2024-03-15"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIME, "12:34:56"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIME_NS, "12:34:56.123456789"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP, "2024-03-15 12:34:56"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_SEC, "2024-03-15 12:34:56"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_MS, "2024-03-15 12:34:56.123"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_TIMESTAMP_NS, "2024-03-15 12:34:56.123456789"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_INTERVAL, "1 month 2 days 00:00:03"},
	    {DUCKDB_V2_LOGICAL_TYPE_ID_UUID, "47183823-2574-4bfd-b411-99ed177d3e43"},
	};
	for (auto &c : cases) {
		auto v = MakeValueFromText(fx.factory, c.id, c.text);
		REQUIRE(Render(v) == c.text);
		duckdb_v2_value_destroy(&v);
	}
}

TEST_CASE("V2: DECIMAL values are built from text and read through a cast", "[capi_v2][value][cast][decimal]") {
	EnvFixture fx;
	auto t = MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 6)});
	auto v = MakeValueFromText(fx.factory, t, "123456789012.345678");
	REQUIRE(Render(v) == "123456789012.345678");

	// DECIMAL has no getter of its own; cast to one that does.
	auto double_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE);
	duckdb_v2_value_handle as_double = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, v, double_type, &as_double, nullptr) == DUCKDB_V2_ERROR_NONE);
	const double read_back = ConsumeValue<double>(as_double);
	REQUIRE(read_back > 123456789012.345 - 1e-3);
	REQUIRE(read_back < 123456789012.345 + 1e-3);
	duckdb_v2_logical_type_destroy(&double_type);

	// A value outside the declared width is a cast failure, not a silent
	// truncation.
	auto narrow =
	    MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 4), MakeInt32Value(fx.factory, 2)});
	auto text = MakeVarcharValue(fx.factory, "12.34");
	duckdb_v2_value_handle fits = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, text, narrow, &fits, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(fits) == "12.34");
	duckdb_v2_value_destroy(&fits);
	duckdb_v2_value_destroy(&text);

	auto too_wide = MakeVarcharValue(fx.factory, "300.00");
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, too_wide, narrow, &out, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(out == nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&too_wide);

	duckdb_v2_value_destroy(&v);
	duckdb_v2_logical_type_destroy(&narrow);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: BIT values are built from text and read as storage bytes", "[capi_v2][value][cast][bit]") {
	EnvFixture fx;
	auto v = MakeValueFromText(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIT, "10101");
	REQUIRE(Render(v) == "10101");
	// The wire form is the padding-header byte plus the data bytes: five bits
	// leave three padding bits in one data byte.
	auto storage = ConsumeBlob(v);
	REQUIRE(storage.size() == 2);
	REQUIRE(static_cast<uint8_t>(storage[0]) == 3);
}

TEST_CASE("V2: V1-built leaf values read through the typed getters", "[capi_v2][value][typed][v1_v2_bridge]") {
	// Same identity-cast invariant as before: V1 and V2 value handles are
	// both bare heap duckdb::Value.
	auto v1_int = V1ValueToV2(duckdb_create_int64(-77));
	REQUIRE(ConsumeValue<int64_t>(v1_int) == -77);
	duckdb_v2_value_destroy(&v1_int);

	const uint8_t blob_bytes[3] = {1, 0, 2};
	auto v1_blob = V1ValueToV2(duckdb_create_blob(blob_bytes, 3));
	REQUIRE(ConsumeBlob(v1_blob) == std::string("\x01\x00\x02", 3));
	duckdb_v2_value_destroy(&v1_blob);
}

TEST_CASE("V2: value_to_string null handle / short buffer", "[capi_v2][value][to_string]") {
	EnvFixture fx;
	char buf[64] = {};
	idx_t len = 0;
	REQUIRE(duckdb_v2_value_to_string(nullptr, buf, sizeof(buf), &len, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	auto v = MakeInt32Value(fx.factory, 7);
	// out_length carries the answer, so it is mandatory in both modes.
	REQUIRE(duckdb_v2_value_to_string(v, buf, sizeof(buf), nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Sizing excludes the terminator; the buffer must still have room for it.
	REQUIRE(duckdb_v2_value_to_string(v, nullptr, 0, &len, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(len == 1);
	REQUIRE(duckdb_v2_value_to_string(v, buf, len, &len, nullptr) == DUCKDB_V2_ERROR_INPUT_OBJECT_SIZE);
	REQUIRE(len == 1);
	REQUIRE(duckdb_v2_value_to_string(v, buf, len + 1, &len, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(buf) == "7");
	duckdb_v2_value_destroy(&v);
}

// ===========================================================================
// BIGNUM — the codec pair translates between a magnitude + sign and the
// opaque storage bytes. Neither direction allocates on the caller's behalf.
// ===========================================================================

namespace {

// Encodes a magnitude + sign into storage bytes.
std::vector<uint8_t> BignumEncode(const uint8_t *magnitude, idx_t length, bool is_negative) {
	idx_t storage_len = 0;
	REQUIRE(duckdb_v2_bignum_encode(magnitude, length, is_negative, nullptr, 0, &storage_len, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	std::vector<uint8_t> storage(storage_len);
	REQUIRE(duckdb_v2_bignum_encode(magnitude, length, is_negative, storage.data(), storage.size(), &storage_len,
	                                nullptr) == DUCKDB_V2_ERROR_NONE);
	return storage;
}

// Decodes storage bytes back into an owned magnitude + sign.
std::vector<uint8_t> BignumDecode(const uint8_t *storage, idx_t storage_len, bool &out_is_negative) {
	idx_t mag_len = 0;
	REQUIRE(duckdb_v2_bignum_decode(storage, storage_len, nullptr, 0, &mag_len, &out_is_negative, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	std::vector<uint8_t> magnitude(mag_len);
	REQUIRE(duckdb_v2_bignum_decode(storage, storage_len, magnitude.data(), magnitude.size(), &mag_len,
	                                &out_is_negative, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(mag_len == magnitude.size());
	return magnitude;
}

// The storage bytes of a BIGNUM value, read through the byte-string getter.
std::vector<uint8_t> BignumMagnitude(duckdb_v2_value_handle v, bool &out_is_negative) {
	duckdb_v2_str storage = {nullptr, 0};
	REQUIRE(duckdb_v2_value_get_blob(v, &storage, nullptr) == DUCKDB_V2_ERROR_NONE);
	return BignumDecode(reinterpret_cast<const uint8_t *>(storage.ptr), storage.len, out_is_negative);
}

} // namespace

TEST_CASE("V2: bignum codec round-trips magnitude bytes and the sign flag", "[capi_v2][value][bignum]") {
	struct {
		std::vector<uint8_t> magnitude;
		bool is_negative;
	} cases[] = {
	    // 0x010203 = 66051, both signs.
	    {{0x01, 0x02, 0x03}, false},
	    {{0x01, 0x02, 0x03}, true},
	    // {0x80, 0x00} forces the encoder to bit-invert into {0x7f, 0xff}
	    // internally, while still reporting the original magnitude on read. A
	    // reader naively assuming "stored bytes == magnitude" would see 0x7fff
	    // (32767, positive) instead of -32768.
	    {{0x80, 0x00}, true},
	    // Core's encoding requires at least one data byte; zero is one 0x00.
	    {{0x00}, false},
	};
	for (auto &c : cases) {
		auto storage = BignumEncode(c.magnitude.data(), c.magnitude.size(), c.is_negative);
		bool is_negative = !c.is_negative;
		auto out = BignumDecode(storage.data(), storage.size(), is_negative);
		REQUIRE(is_negative == c.is_negative);
		REQUIRE(out == c.magnitude);
	}
}

TEST_CASE("V2: BIGNUM values are built from text and decode to a magnitude", "[capi_v2][value][bignum]") {
	EnvFixture fx;
	struct {
		const char *text;
		std::vector<uint8_t> magnitude;
		bool is_negative;
	} cases[] = {
	    {"66051", {0x01, 0x02, 0x03}, false},
	    {"-66051", {0x01, 0x02, 0x03}, true},
	    {"-32768", {0x80, 0x00}, true},
	    {"0", {0x00}, false},
	};
	for (auto &c : cases) {
		auto v = MakeValueFromText(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGNUM, c.text);
		REQUIRE(Render(v) == c.text);
		bool is_negative = !c.is_negative;
		auto magnitude = BignumMagnitude(v, is_negative);
		duckdb_v2_value_destroy(&v);
		REQUIRE(is_negative == c.is_negative);
		REQUIRE(magnitude == c.magnitude);
	}
}

TEST_CASE("V2: value_get_blob refuses a NULL BIGNUM", "[capi_v2][value][bignum]") {
	EnvFixture fx;
	// BIGNUM reads back as storage bytes, but a NULL has no payload to hand back.
	auto bn_lt = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGNUM);
	duckdb_v2_value_handle nv = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, bn_lt, &nv, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_str out = {nullptr, 0};
	REQUIRE(duckdb_v2_value_get_blob(nv, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&nv);
	duckdb_v2_logical_type_destroy(&bn_lt);
}

// ===========================================================================
// DECIMAL, UUID, BIT and BIGNUM: the kinds whose payload needs more than a
// plain scalar.
// ===========================================================================

TEST_CASE("V2: DECIMAL takes its backing integer plus width and scale", "[capi_v2][value][decimal]") {
	EnvFixture fx;
	struct {
		int64_t scaled;
		uint8_t width;
		uint8_t scale;
		const char *text;
	} cases[] = {
	    {1234, 4, 2, "12.34"},                              // int16 tier
	    {123456789, 9, 4, "12345.6789"},                    // int32 tier
	    {123456789012345678, 18, 6, "123456789012.345678"}, // int64 tier
	    {-1234, 9, 2, "-12.34"},
	};
	for (auto &c : cases) {
		duckdb_v2_hugeint_t payload {static_cast<uint64_t>(c.scaled), c.scaled < 0 ? -1 : 0};
		duckdb_v2_value_handle v = nullptr;
		REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &payload, c.width, c.scale, &v, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(Render(v) == c.text);
		duckdb_v2_value_destroy(&v);
	}

	// Width 38 sits in the int128 tier, which the narrower path cannot carry.
	duckdb_v2_hugeint_t wide {123456789012345ull, 0};
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &wide, 38, 10, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(v) == "12345.6789012345");
	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(v, &t, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(t) == "DECIMAL(38,10)");
	duckdb_v2_logical_type_destroy(&t);
	duckdb_v2_value_destroy(&v);

	// The getter hands back the payload plus the width and scale it was built
	// with. The integer getters cannot stand in for it: a DECIMAL is not a
	// SMALLINT, whatever its storage tier.
	duckdb_v2_value_handle narrow = nullptr;
	duckdb_v2_hugeint_t scaled {1234, 0};
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &scaled, 4, 2, &narrow, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_hugeint_t read {};
	uint8_t read_width = 0;
	uint8_t read_scale = 0;
	REQUIRE(duckdb_v2_value_get_decimal(narrow, &read, &read_width, &read_scale, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(read.lower == 1234);
	REQUIRE(read.upper == 0);
	REQUIRE(read_width == 4);
	REQUIRE(read_scale == 2);
	// The integer getters convert, so they report the numeric value with the
	// fraction dropped -- 12.34 reads as 12 whatever the storage tier, never
	// as the backing 1234. Storage is what value_get_decimal is for.
	int16_t as_smallint = 0;
	REQUIRE(duckdb_v2_value_get_smallint(narrow, &as_smallint, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(as_smallint == 12);
	int64_t as_bigint = 0;
	REQUIRE(duckdb_v2_value_get_bigint(narrow, &as_bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(as_bigint == 12);
	duckdb_v2_value_destroy(&narrow);

	// Width and scale are gated: an out-of-range pair would build a broken
	// type rather than a bad value.
	duckdb_v2_hugeint_t one {1, 0};
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &one, 0, 0, &out, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &one, 39, 0, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &one, 4, 5, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A value too wide for the tier the width selects is refused rather than
	// silently truncated.
	duckdb_v2_hugeint_t huge {0, 1};
	REQUIRE(duckdb_v2_value_create_decimal(fx.factory, &huge, 4, 2, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// The scope handle is mandatory, as everywhere else.
	REQUIRE(duckdb_v2_value_create_decimal(nullptr, &one, 4, 2, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_decimal(nullptr, &one, 4, 2, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2: UUID takes its internal 128-bit form", "[capi_v2][value][uuid]") {
	EnvFixture fx;
	// The storage form, not the canonical byte order: the high bit is flipped
	// so the integer sorts. Rendering still shows the canonical 36 chars.
	duckdb_v2_hugeint_t raw {0x1234, 0x5678};
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_uuid(fx.factory, &raw, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	const auto canonical = Render(v);
	REQUIRE(canonical.size() == 36);
	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(v, &t, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(t) == "UUID");
	duckdb_v2_logical_type_destroy(&t);
	duckdb_v2_value_destroy(&v);

	// Round-trips against the cast path: the same storage from its text form.
	auto from_text = MakeValueFromText(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_UUID, canonical.c_str());
	REQUIRE(Render(from_text) == canonical);
	duckdb_v2_value_destroy(&from_text);
}

TEST_CASE("V2: BIT takes its wire bytes", "[capi_v2][value][bit]") {
	EnvFixture fx;
	// A padding-header byte plus the data bytes. The padding bits are the
	// leading bits of the first data byte, so five bits of 10101 sit in the low
	// five of 0b000'10101.
	const char wire[2] = {3, '\x15'};
	duckdb_v2_value_handle v = nullptr;
	auto value_str = duckdb_v2_str {wire, 2};
	REQUIRE(duckdb_v2_value_create_bit(fx.factory, &value_str, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(v) == "10101");
	// The same bytes come back through the byte-string getter.
	REQUIRE(ConsumeBlob(v) == std::string(wire, 2));

	// The header byte is mandatory.
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	auto value_str2 = duckdb_v2_str {nullptr, 0};
	REQUIRE(duckdb_v2_value_create_bit(fx.factory, &value_str2, &out, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2: BIGNUM takes its storage bytes from the codec", "[capi_v2][value][bignum]") {
	EnvFixture fx;
	// The constructor closes the codec loop: encode a magnitude, build the
	// value from those bytes, read them back, decode to the same magnitude.
	const uint8_t magnitude[] = {0x01, 0x02, 0x03};
	auto storage = BignumEncode(magnitude, 3, false);
	duckdb_v2_str bytes {reinterpret_cast<const char *>(storage.data()), storage.size()};
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_bignum(fx.factory, &bytes, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(v) == "66051");
	bool is_negative = true;
	auto out_magnitude = BignumMagnitude(v, is_negative);
	duckdb_v2_value_destroy(&v);
	REQUIRE(!is_negative);
	REQUIRE(out_magnitude == std::vector<uint8_t>(magnitude, magnitude + 3));

	// Negatives are stored bit-inverted, so the storage bytes are not the
	// magnitude; the codec is what translates.
	auto negative = BignumEncode(magnitude, 3, true);
	duckdb_v2_str neg_bytes {reinterpret_cast<const char *>(negative.data()), negative.size()};
	REQUIRE(duckdb_v2_value_create_bignum(fx.factory, &neg_bytes, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(v) == "-66051");
	duckdb_v2_value_destroy(&v);

	// A header with no magnitude byte is not addressable storage.
	const char short_bytes[3] = {0, 0, 0};
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	auto value_str = duckdb_v2_str {short_bytes, 3};
	REQUIRE(duckdb_v2_value_create_bignum(fx.factory, &value_str, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
}

// ===========================================================================
// Error info propagation: a sample failure path attaches an info handle.
// ===========================================================================

TEST_CASE("V2: failure path populates error info", "[capi_v2][value][error]") {
	EnvFixture fx;
	duckdb_v2_value_handle v = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto value_str = duckdb_v2_str {nullptr, 4};
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &value_str, &v, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(err != nullptr);
	duckdb_v2_str msg = {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(err, &msg) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(msg.len > 0);
	duckdb_v2_error_info_destroy(&err);
}

// ===========================================================================
// TYPE values (a logical type carried as a value)
// ===========================================================================

TEST_CASE("V2: TYPE value wraps and unwraps a logical type", "[capi_v2][value][type_value]") {
	EnvFixture fx;
	duckdb_v2_logical_type_handle int_type = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &int_type, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_type(fx.factory, int_type, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(v != nullptr);
	// Input type is borrowed: destroying it must not affect the value.
	REQUIRE(duckdb_v2_logical_type_destroy(&int_type) == DUCKDB_V2_ERROR_NONE);

	bool is_null = true;
	REQUIRE(duckdb_v2_value_is_null(v, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(!is_null);

	// The value's own type is TYPE.
	duckdb_v2_logical_type_handle value_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(v, &value_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(value_type, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_TYPE);
	duckdb_v2_logical_type_destroy(&value_type);

	// Unwrap: an owned copy equal to the wrapped type.
	duckdb_v2_logical_type_handle unwrapped = nullptr;
	REQUIRE(duckdb_v2_value_get_type(v, &unwrapped, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(unwrapped != nullptr);
	duckdb_v2_logical_type_handle expected = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &expected,
	                                      nullptr);
	bool equal = false;
	REQUIRE(duckdb_v2_logical_type_is_equal(unwrapped, expected, &equal, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(equal);
	duckdb_v2_logical_type_destroy(&expected);
	duckdb_v2_logical_type_destroy(&unwrapped);

	// The unwrapped copy is independent of the value.
	REQUIRE(duckdb_v2_value_get_type(v, &unwrapped, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&v);
	DUCKDB_V2_LOGICAL_TYPE_ID unwrapped_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(unwrapped, &unwrapped_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(unwrapped_id == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&unwrapped);
}

TEST_CASE("V2: TYPE value round-trips a composite type", "[capi_v2][value][type_value]") {
	EnvFixture fx;
	// Nested composite: STRUCT(a INTEGER[], b VARCHAR) survives the value's
	// internal serialize/deserialize round trip.
	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	std::vector<const char *> names = {"a", "b"};
	auto t =
	    MakeType(fx.factory, "struct", &names,
	             {MakeTypeValue(fx.factory, list_type), MakeTypeValue(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
	duckdb_v2_logical_type_destroy(&list_type);

	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_type(fx.factory, t, &v, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_logical_type_handle unwrapped = nullptr;
	REQUIRE(duckdb_v2_value_get_type(v, &unwrapped, nullptr) == DUCKDB_V2_ERROR_NONE);
	bool equal = false;
	REQUIRE(duckdb_v2_logical_type_is_equal(t, unwrapped, &equal, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(equal);

	duckdb_v2_logical_type_destroy(&unwrapped);
	duckdb_v2_value_destroy(&v);
	duckdb_v2_logical_type_destroy(&t);
}

TEST_CASE("V2: TYPE value to_string renders the type text", "[capi_v2][value][type_value][to_string]") {
	EnvFixture fx;
	auto t = MakeType(fx.factory, "decimal", nullptr, {MakeInt32Value(fx.factory, 18), MakeInt32Value(fx.factory, 3)});
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_type(fx.factory, t, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&t);

	auto str = Render(v);
	REQUIRE(str == "DECIMAL(18,3)");
	duckdb_v2_value_destroy(&v);
}

TEST_CASE("V2: value_get_type rejects non-TYPE and NULL TYPE values", "[capi_v2][value][type_value]") {
	EnvFixture fx;
	// Wrong kind: an INTEGER value is not a TYPE value.
	duckdb_v2_value_handle int_value = MakeInt32Value(fx.factory, 42);
	duckdb_v2_logical_type_handle out = reinterpret_cast<duckdb_v2_logical_type_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_get_type(int_value, &out, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&int_value);

	// NULL TYPE value: no payload to unwrap. The TYPE logical type is taken
	// from a TYPE value's own type (create_from_id does not construct it).
	duckdb_v2_logical_type_handle int_type = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &int_type,
	                                      nullptr);
	duckdb_v2_value_handle type_value = nullptr;
	REQUIRE(duckdb_v2_value_create_type(fx.factory, int_type, &type_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_logical_type_handle type_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(type_value, &type_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&type_value);

	duckdb_v2_value_handle null_type_value = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, type_type, &null_type_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&type_type);
	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(null_type_value, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	out = reinterpret_cast<duckdb_v2_logical_type_handle>(0x1);
	REQUIRE(duckdb_v2_value_get_type(null_type_value, &out, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&null_type_value);
}

TEST_CASE("V2: value_create_type / value_get_type null-arg refusals", "[capi_v2][value][type_value]") {
	EnvFixture fx;
	duckdb_v2_value_handle v = nullptr;
	REQUIRE(duckdb_v2_value_create_type(fx.factory, nullptr, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(v == nullptr);

	duckdb_v2_logical_type_handle int_type = nullptr;
	duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &int_type,
	                                      nullptr);
	REQUIRE(duckdb_v2_value_create_type(fx.factory, int_type, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// The scope handle is mandatory too.
	REQUIRE(duckdb_v2_value_create_type(nullptr, int_type, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_type(nullptr, int_type, &v, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_logical_type_handle out = nullptr;
	REQUIRE(duckdb_v2_value_get_type(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_value_create_type(fx.factory, int_type, &v, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_get_type(v, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&v);
	duckdb_v2_logical_type_destroy(&int_type);
}

// ===========================================================================
// Composite construction + descent
//
// One typed constructor per kind, scoped to a connection (a context inside a
// bind callback). The child type is resolved from the children the way a list
// literal resolves it, so a set of mixed-but-compatible types widens rather
// than failing; an explicit type overrides that, and is what the empty forms
// take.
// ===========================================================================

namespace {

duckdb_v2_value_handle V2I32(duckdb_v2_factory_handle factory, int32_t x) {
	return MakeInt32Value(factory, x);
}

duckdb_v2_value_handle V2Varchar(duckdb_v2_factory_handle factory, const char *s) {
	return MakeVarcharValue(factory, s);
}

duckdb_v2_value_handle V2NullOf(duckdb_v2_factory_handle factory, DUCKDB_V2_LOGICAL_TYPE_ID id) {
	duckdb_v2_logical_type_handle t = nullptr;
	duckdb_v2_factory_create_type_from_id(factory, id, nullptr, nullptr, 0, &t, nullptr);
	duckdb_v2_value_handle v = nullptr;
	auto rc = duckdb_v2_value_create_null(factory, t, &v, nullptr);
	duckdb_v2_logical_type_destroy(&t);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return v;
}

// Runs a composite constructor over borrowed children, destroying them after.
// Reads the result first, then destroys, then asserts, so a failing REQUIRE
// cannot leak.
template <class CALL>
duckdb_v2_value_handle V2Build(std::vector<duckdb_v2_value_handle> children, CALL call) {
	duckdb_v2_value_handle v = nullptr;
	auto rc = call(children.empty() ? nullptr : children.data(), children.size(), &v, nullptr);
	for (auto &c : children) {
		duckdb_v2_value_destroy(&c);
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(v != nullptr);
	return v;
}

// Same, expecting failure: checks out nulling + err population.
template <class CALL>
DUCKDB_V2_ERROR V2BuildErr(std::vector<duckdb_v2_value_handle> children, CALL call) {
	auto v = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = call(children.empty() ? nullptr : children.data(), children.size(), &v, &err);
	for (auto &c : children) {
		duckdb_v2_value_destroy(&c);
	}
	const bool out_nulled = (v == nullptr);
	const bool err_set = (err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(rc != DUCKDB_V2_ERROR_NONE);
	REQUIRE(out_nulled);
	REQUIRE(err_set);
	return rc;
}

// The per-kind call shapes, with the element / entry type left to inference.
auto ListCall(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle child_type = nullptr) {
	return [factory, child_type](const duckdb_v2_value_handle *children, idx_t count, duckdb_v2_value_handle *out,
	                             duckdb_v2_error_info_handle *err) {
		return duckdb_v2_value_create_list(factory, child_type, children, count, out, err);
	};
}

auto ArrayCall(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle child_type = nullptr) {
	return [factory, child_type](const duckdb_v2_value_handle *children, idx_t count, duckdb_v2_value_handle *out,
	                             duckdb_v2_error_info_handle *err) {
		return duckdb_v2_value_create_array(factory, child_type, children, count, out, err);
	};
}

auto TupleCall(duckdb_v2_factory_handle factory) {
	return [factory](const duckdb_v2_value_handle *children, idx_t count, duckdb_v2_value_handle *out,
	                 duckdb_v2_error_info_handle *err) {
		return duckdb_v2_value_create_tuple(factory, children, count, out, err);
	};
}

// STRUCT and MAP take parallel arrays, so they get their own shapes.
duckdb_v2_value_handle V2Struct(duckdb_v2_factory_handle factory, const std::vector<const char *> &names,
                                std::vector<duckdb_v2_value_handle> children) {
	std::vector<duckdb_v2_identifier_t> name_views;
	for (auto *n : names) {
		name_views.push_back(Convert(n));
	}
	duckdb_v2_value_handle v = nullptr;
	auto rc = duckdb_v2_value_create_struct(factory, name_views.empty() ? nullptr : name_views.data(),
	                                        children.empty() ? nullptr : children.data(), children.size(), &v, nullptr);
	for (auto &c : children) {
		duckdb_v2_value_destroy(&c);
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(v != nullptr);
	return v;
}

duckdb_v2_value_handle V2Map(duckdb_v2_factory_handle factory, std::vector<duckdb_v2_value_handle> keys,
                             std::vector<duckdb_v2_value_handle> values) {
	duckdb_v2_value_handle v = nullptr;
	auto rc = duckdb_v2_value_create_map(factory, nullptr, nullptr, keys.empty() ? nullptr : keys.data(),
	                                     values.empty() ? nullptr : values.data(), keys.size(), &v, nullptr);
	for (auto &k : keys) {
		duckdb_v2_value_destroy(&k);
	}
	for (auto &val : values) {
		duckdb_v2_value_destroy(&val);
	}
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(v != nullptr);
	return v;
}

idx_t V2ChildCount(duckdb_v2_value_handle v) {
	idx_t count = 99;
	REQUIRE(duckdb_v2_value_get_child_count(v, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	return count;
}

duckdb_v2_value_handle V2Child(duckdb_v2_value_handle v, idx_t index) {
	duckdb_v2_value_handle child = nullptr;
	REQUIRE(duckdb_v2_value_get_child(v, index, &child, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(child != nullptr);
	return child;
}

// Renders child `index` via value_to_string (type-agnostic comparisons).
std::string V2ChildText(duckdb_v2_value_handle v, idx_t index) {
	auto child = V2Child(v, index);
	auto out = Render(child);
	duckdb_v2_value_destroy(&child);
	return out;
}

// The value's own type, rendered.
std::string V2TypeOf(duckdb_v2_value_handle v) {
	duckdb_v2_logical_type_handle t = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(v, &t, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto text = Render(t);
	duckdb_v2_logical_type_destroy(&t);
	return text;
}

} // namespace

TEST_CASE("V2: LIST round-trips elements and NULLs", "[capi_v2][value][composite]") {
	EnvFixture fx;
	auto list =
	    V2Build({V2I32(fx.factory, 1), V2NullOf(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER), V2I32(fx.factory, 3)},
	            ListCall(fx.factory));
	REQUIRE(V2ChildCount(list) == 3);
	REQUIRE(V2TypeOf(list) == "INTEGER[]");
	REQUIRE(V2ChildText(list, 0) == "1");
	REQUIRE(V2ChildText(list, 2) == "3");
	auto middle = V2Child(list, 1);
	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(middle, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	duckdb_v2_value_destroy(&middle);
	// Children are owned copies: the parent survives destroying them.
	REQUIRE(V2ChildCount(list) == 3);
	duckdb_v2_value_destroy(&list);
}

TEST_CASE("V2: LIST resolves the element type across the elements", "[capi_v2][value][composite]") {
	EnvFixture fx;
	// Mixed widths widen to the common type rather than truncating to the
	// first element's: the rule a list literal follows.
	auto widened = V2Build({V2I32(fx.factory, 7), MakeInt64Value(fx.factory, 1LL << 40)}, ListCall(fx.factory));
	REQUIRE(V2TypeOf(widened) == "BIGINT[]");
	REQUIRE(V2ChildText(widened, 1) == "1099511627776");
	duckdb_v2_value_destroy(&widened);

	// An explicit element type overrides the resolution, and the elements are
	// cast to it.
	auto bigint_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto declared = V2Build({V2I32(fx.factory, 7)}, ListCall(fx.factory, bigint_type));
	REQUIRE(V2TypeOf(declared) == "BIGINT[]");
	auto child = V2Child(declared, 0);
	REQUIRE(ConsumeValue<int64_t>(child) == 7);
	duckdb_v2_value_destroy(&declared);

	// The empty list takes its element type the same way.
	auto empty = V2Build({}, ListCall(fx.factory, bigint_type));
	REQUIRE(V2ChildCount(empty) == 0);
	REQUIRE(V2TypeOf(empty) == "BIGINT[]");
	duckdb_v2_value_handle child_out = nullptr;
	REQUIRE(duckdb_v2_value_get_child(empty, 0, &child_out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&empty);
	duckdb_v2_logical_type_destroy(&bigint_type);

	// Without one there is nothing to resolve.
	REQUIRE(V2BuildErr({}, ListCall(fx.factory)) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Elements with no common type surface the engine's error.
	REQUIRE(V2BuildErr({V2I32(fx.factory, 1), V2Varchar(fx.factory, "abc")}, ListCall(fx.factory)) !=
	        DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2: ARRAY is sized by its element count", "[capi_v2][value][composite]") {
	EnvFixture fx;
	auto arr = V2Build({V2I32(fx.factory, 1), V2I32(fx.factory, 2), V2I32(fx.factory, 3)}, ArrayCall(fx.factory));
	REQUIRE(V2ChildCount(arr) == 3);
	REQUIRE(V2TypeOf(arr) == "INTEGER[3]");
	REQUIRE(V2ChildText(arr, 1) == "2");
	duckdb_v2_value_destroy(&arr);

	// The engine's minimum array size is 1, so there is no empty ARRAY, with
	// or without a declared element type.
	REQUIRE(V2BuildErr({}, ArrayCall(fx.factory)) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(V2BuildErr({}, ArrayCall(fx.factory, int_type)) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&int_type);
}

TEST_CASE("V2: STRUCT takes parallel names and fields", "[capi_v2][value][composite]") {
	EnvFixture fx;
	auto s = V2Struct(fx.factory, {"id", "label"}, {V2I32(fx.factory, 42), V2Varchar(fx.factory, "joe")});
	REQUIRE(V2ChildCount(s) == 2);
	REQUIRE(V2TypeOf(s) == "STRUCT(id INTEGER, \"label\" VARCHAR)");
	auto id_child = V2Child(s, 0);
	REQUIRE(ConsumeValue<int32_t>(id_child) == 42);
	REQUIRE(V2ChildText(s, 1) == "joe");
	duckdb_v2_value_destroy(&s);

	// Each field keeps its own type, so a NULL field is a typed NULL.
	auto with_null = V2Struct(fx.factory, {"id", "label"},
	                          {V2I32(fx.factory, 1), V2NullOf(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});
	auto null_field = V2Child(with_null, 1);
	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(null_field, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	duckdb_v2_value_destroy(&null_field);
	duckdb_v2_value_destroy(&with_null);

	// The empty struct is a real type.
	auto empty = V2Struct(fx.factory, {}, {});
	REQUIRE(V2ChildCount(empty) == 0);
	REQUIRE(Render(empty) == "{}");
	duckdb_v2_value_destroy(&empty);
}

TEST_CASE("V2: TUPLE takes positional fields", "[capi_v2][value][composite]") {
	EnvFixture fx;
	auto t = V2Build({V2I32(fx.factory, 42), V2Varchar(fx.factory, "joe")}, TupleCall(fx.factory));
	REQUIRE(V2ChildCount(t) == 2);
	REQUIRE(V2TypeOf(t) == "TUPLE(INTEGER, VARCHAR)");
	auto first = V2Child(t, 0);
	REQUIRE(ConsumeValue<int32_t>(first) == 42);
	REQUIRE(V2ChildText(t, 1) == "joe");
	duckdb_v2_value_destroy(&t);

	// The empty tuple is a real type too.
	auto empty = V2Build({}, TupleCall(fx.factory));
	REQUIRE(V2ChildCount(empty) == 0);
	REQUIRE(Render(empty) == "()");
	duckdb_v2_value_destroy(&empty);
}

TEST_CASE("V2: MAP takes parallel keys and values", "[capi_v2][value][composite]") {
	EnvFixture fx;
	auto map = V2Map(fx.factory, {V2Varchar(fx.factory, "a"), V2Varchar(fx.factory, "b")},
	                 {V2I32(fx.factory, 1), V2I32(fx.factory, 2)});
	// Children alternate key, value on the way back out.
	REQUIRE(V2ChildCount(map) == 4);
	REQUIRE(V2TypeOf(map) == "MAP(VARCHAR, INTEGER)");
	REQUIRE(V2ChildText(map, 0) == "a");
	REQUIRE(V2ChildText(map, 1) == "1");
	REQUIRE(V2ChildText(map, 2) == "b");
	REQUIRE(V2ChildText(map, 3) == "2");
	duckdb_v2_value_destroy(&map);

	// Key and value types resolve across their own arrays independently.
	auto widened = V2Map(fx.factory, {V2Varchar(fx.factory, "k")}, {MakeInt64Value(fx.factory, 9)});
	REQUIRE(V2TypeOf(widened) == "MAP(VARCHAR, BIGINT)");
	duckdb_v2_value_destroy(&widened);

	// The empty map takes its key and value types explicitly.
	auto key_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	auto value_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_value_handle empty = nullptr;
	REQUIRE(duckdb_v2_value_create_map(fx.factory, key_type, value_type, nullptr, nullptr, 0, &empty, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(V2ChildCount(empty) == 0);
	REQUIRE(V2TypeOf(empty) == "MAP(VARCHAR, INTEGER)");
	duckdb_v2_value_destroy(&empty);

	// Without them there is nothing to resolve.
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	REQUIRE(duckdb_v2_value_create_map(fx.factory, nullptr, nullptr, nullptr, nullptr, 0, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	duckdb_v2_logical_type_destroy(&key_type);
	duckdb_v2_logical_type_destroy(&value_type);

	// Duplicate and NULL keys are rejected by the engine.
	duckdb_v2_value_handle dup_keys[2] = {V2Varchar(fx.factory, "a"), V2Varchar(fx.factory, "a")};
	duckdb_v2_value_handle dup_values[2] = {V2I32(fx.factory, 1), V2I32(fx.factory, 2)};
	REQUIRE(duckdb_v2_value_create_map(fx.factory, nullptr, nullptr, dup_keys, dup_values, 2, &out, nullptr) !=
	        DUCKDB_V2_ERROR_NONE);
	for (auto &k : dup_keys) {
		duckdb_v2_value_destroy(&k);
	}
	for (auto &v : dup_values) {
		duckdb_v2_value_destroy(&v);
	}

	duckdb_v2_value_handle null_key[1] = {V2NullOf(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)};
	duckdb_v2_value_handle one_value[1] = {V2I32(fx.factory, 1)};
	REQUIRE(duckdb_v2_value_create_map(fx.factory, nullptr, nullptr, null_key, one_value, 1, &out, nullptr) !=
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&null_key[0]);
	duckdb_v2_value_destroy(&one_value[0]);
}

TEST_CASE("V2: composite constructors invalid-arg refusals", "[capi_v2][value][composite]") {
	EnvFixture fx;
	duckdb_v2_value_handle out = nullptr;

	// The scope handle and the out slot are mandatory everywhere.
	REQUIRE(duckdb_v2_value_create_list(nullptr, nullptr, nullptr, 0, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_list(nullptr, nullptr, nullptr, 0, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_create_tuple(fx.factory, nullptr, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A count with no array, and a null child inside one.
	REQUIRE(duckdb_v2_value_create_tuple(fx.factory, nullptr, 1, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	const duckdb_v2_value_handle holed[1] = {nullptr};
	REQUIRE(duckdb_v2_value_create_tuple(fx.factory, holed, 1, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);

	// STRUCT needs its names array whenever it has fields.
	auto one = V2I32(fx.factory, 1);
	const duckdb_v2_value_handle fields[1] = {one};
	REQUIRE(duckdb_v2_value_create_struct(fx.factory, nullptr, fields, 1, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	// Field names must contain valid UTF-8.
	const duckdb_v2_identifier_t invalid_names[1] = {Convert("\x80")};
	REQUIRE(duckdb_v2_value_create_struct(fx.factory, invalid_names, fields, 1, &out, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	duckdb_v2_value_destroy(&one);
}

TEST_CASE("V2: value_get_child_count is 0 for primitives and NULL composites", "[capi_v2][value][composite]") {
	EnvFixture fx;
	auto primitive = V2I32(fx.factory, 42);
	REQUIRE(V2ChildCount(primitive) == 0);
	duckdb_v2_value_handle child = nullptr;
	REQUIRE(duckdb_v2_value_get_child(primitive, 0, &child, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(child == nullptr);
	duckdb_v2_value_destroy(&primitive);

	auto list_type = MakeListType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_value_handle null_list = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, list_type, &null_list, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&list_type);
	REQUIRE(V2ChildCount(null_list) == 0);
	REQUIRE(duckdb_v2_value_get_child(null_list, 0, &child, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&null_list);

	// Null-arg refusals.
	idx_t count = 0;
	REQUIRE(duckdb_v2_value_get_child_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto v = V2I32(fx.factory, 1);
	REQUIRE(duckdb_v2_value_get_child_count(v, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_get_child(nullptr, 0, &child, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_get_child(v, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&v);
}

// ===========================================================================
// value_cast: generic conversion; the UNION / ENUM construction path
// ===========================================================================

namespace {

// Cast helper: casts straight from the connection.
// Returns the owned result.
duckdb_v2_value_handle V2CastValue(duckdb_v2_factory_handle factory, duckdb_v2_value_handle value,
                                   duckdb_v2_logical_type_handle target) {
	duckdb_v2_value_handle out = nullptr;
	REQUIRE(duckdb_v2_value_cast(factory, value, target, &out, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(out != nullptr);
	return out;
}

} // namespace

TEST_CASE("V2: value_cast converts across types and from text", "[capi_v2][value][cast]") {
	EnvFixture f;

	// Widening numeric cast.
	auto small = V2I32(f.factory, 42);
	duckdb_v2_logical_type_handle bigint_type = nullptr;
	duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, 0,
	                                      &bigint_type, nullptr);
	auto widened = V2CastValue(f.factory, small, bigint_type);
	REQUIRE(ConsumeValue<int64_t>(widened) == 42);
	duckdb_v2_value_destroy(&widened);
	duckdb_v2_value_destroy(&small);
	duckdb_v2_logical_type_destroy(&bigint_type);

	// Text to DATE.
	auto date_text = V2Varchar(f.factory, "2024-03-15");
	duckdb_v2_logical_type_handle date_type = nullptr;
	duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DATE, nullptr, nullptr, 0, &date_type,
	                                      nullptr);
	auto date = V2CastValue(f.factory, date_text, date_type);
	auto rendered = Render(date);
	REQUIRE(rendered == "2024-03-15");
	duckdb_v2_value_destroy(&date);
	duckdb_v2_value_destroy(&date_text);
	duckdb_v2_logical_type_destroy(&date_type);

	// Text to a composite: with a VARCHAR built through the leaf codec this
	// constructs any value from text.
	auto list_text = V2Varchar(f.factory, "[1, 2, 3]");
	auto list_type = MakeListType(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto list = V2CastValue(f.factory, list_text, list_type);
	REQUIRE(V2ChildCount(list) == 3);
	REQUIRE(V2ChildText(list, 2) == "3");
	duckdb_v2_value_destroy(&list);
	duckdb_v2_value_destroy(&list_text);
	duckdb_v2_logical_type_destroy(&list_type);

	// A failing cast surfaces the conversion error and nulls the out param.
	auto bad = V2Varchar(f.factory, "abc");
	duckdb_v2_logical_type_handle int_type = nullptr;
	duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &int_type,
	                                      nullptr);
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, bad, int_type, &out, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_value_destroy(&bad);
}

TEST_CASE("V2: UNION values build via value_cast and descend as tag + member", "[capi_v2][value][cast][union]") {
	EnvFixture f;
	std::vector<const char *> names = {"i", "s"};
	auto union_type = MakeType(f.factory, "union", &names,
	                           {MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER),
	                            MakeTypeValue(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR)});

	// Member to union: the engine selects the matching member.
	auto int_member = V2I32(f.factory, 42);
	auto u = V2CastValue(f.factory, int_member, union_type);
	duckdb_v2_value_destroy(&int_member);
	REQUIRE(V2ChildCount(u) == 2);
	// [0] = the tag as UTINYINT; [1] = the active member. A union value
	// holds only its active member (unlike the vector module's
	// [1..N] = all members convention).
	auto tag = V2Child(u, 0);
	REQUIRE(ConsumeValue<uint8_t>(tag) == 0);
	duckdb_v2_value_destroy(&tag);
	auto member = V2Child(u, 1);
	REQUIRE(ConsumeValue<int32_t>(member) == 42);
	duckdb_v2_value_destroy(&member);
	REQUIRE(duckdb_v2_value_get_child(u, 2, &member, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_value_destroy(&u);

	// The other member selects tag 1.
	auto str_member = V2Varchar(f.factory, "x");
	auto u2 = V2CastValue(f.factory, str_member, union_type);
	duckdb_v2_value_destroy(&str_member);
	tag = V2Child(u2, 0);
	REQUIRE(ConsumeValue<uint8_t>(tag) == 1);
	duckdb_v2_value_destroy(&tag);
	REQUIRE(V2ChildText(u2, 1) == "x");
	duckdb_v2_value_destroy(&u2);
	duckdb_v2_logical_type_destroy(&union_type);
}

TEST_CASE("V2: ENUM values build via value_cast from VARCHAR", "[capi_v2][value][cast][enum]") {
	EnvFixture f;
	auto enum_type = MakeType(
	    f.factory, "enum", nullptr,
	    {MakeVarcharValue(f.factory, "sad"), MakeVarcharValue(f.factory, "ok"), MakeVarcharValue(f.factory, "happy")});

	auto text = V2Varchar(f.factory, "happy");
	auto e = V2CastValue(f.factory, text, enum_type);
	duckdb_v2_value_destroy(&text);
	duckdb_v2_logical_type_handle vt = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(e, &vt, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	duckdb_v2_logical_type_get_id(vt, &id, nullptr);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_ENUM);
	duckdb_v2_logical_type_destroy(&vt);
	auto rendered = Render(e);
	REQUIRE(rendered == "happy");

	// And back to text through the cast machinery.
	duckdb_v2_logical_type_handle varchar_type = nullptr;
	duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, nullptr, nullptr, 0,
	                                      &varchar_type, nullptr);
	auto back = V2CastValue(f.factory, e, varchar_type);
	REQUIRE(ConsumeValue<std::string>(back) == "happy");
	duckdb_v2_value_destroy(&back);
	duckdb_v2_logical_type_destroy(&varchar_type);
	duckdb_v2_value_destroy(&e);

	// A string outside the dictionary fails the cast.
	auto bad = V2Varchar(f.factory, "angry");
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, bad, enum_type, &out, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&bad);
	duckdb_v2_logical_type_destroy(&enum_type);
}

TEST_CASE("V2: value_cast null-arg refusals", "[capi_v2][value][cast]") {
	EnvFixture f;
	duckdb_v2_value_handle out = nullptr;
	auto v = V2I32(f.factory, 1);
	duckdb_v2_logical_type_handle int_type = nullptr;
	duckdb_v2_factory_create_type_from_id(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &int_type,
	                                      nullptr);

	// A null connection is refused without any scope.
	REQUIRE(duckdb_v2_value_cast(nullptr, v, int_type, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_value_cast(f.factory, nullptr, int_type, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_cast(f.factory, v, nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_value_cast(f.factory, v, int_type, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_value_destroy(&v);
}
// ===========================================================================
// VARIANT: reached through value_cast, in both directions. There is no
// unwrapping getter -- the cast machinery is the whole access path.
// ===========================================================================

TEST_CASE("V2: VARIANT cells are read by casting them", "[capi_v2][value][variant]") {
	EnvFixture f;
	QueryResult r;

	REQUIRE(Query(f.conn, "SELECT 42::VARIANT AS v, NULL::VARIANT AS n", &r) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle value_vec = nullptr;
	duckdb_v2_vector_handle null_vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &value_vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 1, &null_vec, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_value_handle box = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(value_vec, 0, &box, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_handle box_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(box, &box_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID box_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(box_type, &box_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(box_id == DUCKDB_V2_LOGICAL_TYPE_ID_VARIANT);
	duckdb_v2_logical_type_destroy(&box_type);

	// Cast to the type the cell actually carries.
	auto int_type = MakeType(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_value_handle as_int = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, box, int_type, &as_int, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<int32_t>(as_int) == 42);

	// Or to text, which works whatever the cell holds.
	auto varchar_type = MakeType(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_value_handle as_text = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, box, varchar_type, &as_text, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<std::string>(as_text) == "42");

	// A NULL cell casts to a NULL of the target type rather than failing.
	duckdb_v2_value_handle null_box = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(null_vec, 0, &null_box, nullptr) == DUCKDB_V2_ERROR_NONE);
	bool is_null = false;
	REQUIRE(duckdb_v2_value_is_null(null_box, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	duckdb_v2_value_handle null_int = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, null_box, int_type, &null_int, nullptr) == DUCKDB_V2_ERROR_NONE);
	is_null = false;
	REQUIRE(duckdb_v2_value_is_null(null_int, &is_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(is_null);
	duckdb_v2_value_destroy(&null_int);

	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_logical_type_destroy(&varchar_type);
	duckdb_v2_value_destroy(&null_box);
	duckdb_v2_value_destroy(&box);
	duckdb_v2_data_chunk_destroy(&chunk);
}

TEST_CASE("V2: VARIANT values are built by casting into them", "[capi_v2][value][variant]") {
	EnvFixture f;
	duckdb_v2_logical_type_handle variant_type = nullptr;
	auto text_str = Convert("VARIANT");
	REQUIRE(duckdb_v2_factory_create_type_from_text(f.factory, &text_str, &variant_type, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);

	// In: any value casts to VARIANT. Out: back to the same type.
	auto original = MakeInt32Value(f.factory, 42);
	duckdb_v2_value_handle boxed = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, original, variant_type, &boxed, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_handle boxed_type = nullptr;
	REQUIRE(duckdb_v2_value_get_logical_type(boxed, &boxed_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID boxed_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(boxed_type, &boxed_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(boxed_id == DUCKDB_V2_LOGICAL_TYPE_ID_VARIANT);
	duckdb_v2_logical_type_destroy(&boxed_type);

	auto int_type = MakeType(f.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_value_handle unboxed = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, boxed, int_type, &unboxed, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeValue<int32_t>(unboxed) == 42);

	// A cell holding text does not become a number just because it is asked to.
	auto text = MakeVarcharValue(f.factory, "abc");
	duckdb_v2_value_handle text_box = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, text, variant_type, &text_box, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_cast(f.factory, text_box, int_type, &out, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(out == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_error_info_destroy(&err);

	duckdb_v2_value_destroy(&text_box);
	duckdb_v2_value_destroy(&text);
	duckdb_v2_value_destroy(&boxed);
	duckdb_v2_value_destroy(&original);
	duckdb_v2_logical_type_destroy(&int_type);
	duckdb_v2_logical_type_destroy(&variant_type);
}

namespace {

// POINT (42 1337) as little-endian WKB
const uint8_t POINT_WKB[] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x45, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE4, 0x94, 0x40};

} // namespace

TEST_CASE("V2: GEOMETRY values cast to and from WKB BLOBs", "[capi_v2][value][cast][geometry]") {
	EnvFixture fx;
	auto geom_type = MakeType(fx.factory, "geometry", nullptr, {});
	auto blob_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);
	const std::string expected_wkb(reinterpret_cast<const char *>(POINT_WKB), sizeof(POINT_WKB));

	// The byte-string getter does not read GEOMETRY directly; cast to BLOB to get the WKB
	auto geom = MakeValueFromText(fx.factory, geom_type, "POINT(42 1337)");
	duckdb_v2_str str = {nullptr, 0};
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_value_get_blob(geom, &str, &err) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_destroy(&err);

	duckdb_v2_value_handle wkb = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, geom, blob_type, &wkb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeBlob(wkb) == expected_wkb);
	duckdb_v2_value_destroy(&geom);

	// WKB to GEOMETRY
	auto blob = MakeBlobValue(fx.factory, POINT_WKB, sizeof(POINT_WKB));
	duckdb_v2_value_handle from_wkb = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, blob, geom_type, &from_wkb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(from_wkb) == "POINT (42 1337)");
	duckdb_v2_value_destroy(&from_wkb);
	duckdb_v2_value_destroy(&blob);

	// Invalid WKB is a cast failure
	auto truncated = MakeBlobValue(fx.factory, POINT_WKB, 5);
	auto out = reinterpret_cast<duckdb_v2_value_handle>(0x1);
	REQUIRE(duckdb_v2_value_cast(fx.factory, truncated, geom_type, &out, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(out == nullptr);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&truncated);

	duckdb_v2_logical_type_destroy(&blob_type);
	duckdb_v2_logical_type_destroy(&geom_type);
}

TEST_CASE("V2: GEOMETRY vector cells read and write as WKB", "[capi_v2][value][cast][geometry]") {
	EnvFixture fx;
	auto geom_type = MakeType(fx.factory, "geometry", nullptr, {});
	auto blob_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);

	duckdb_v2_logical_type_handle types[1] = {geom_type};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, 1, nullptr) == DUCKDB_V2_ERROR_NONE);

	// A WKB BLOB value is cast to GEOMETRY on write
	auto blob = MakeBlobValue(fx.factory, POINT_WKB, sizeof(POINT_WKB));
	REQUIRE(duckdb_v2_vector_set_value(vec, 0, blob, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&blob);

	// Invalid WKB is rejected on write
	auto truncated = MakeBlobValue(fx.factory, POINT_WKB, 5);
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_vector_set_value(vec, 0, truncated, &err) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_value_destroy(&truncated);

	// The cell reads back as GEOMETRY, and casts to its WKB
	duckdb_v2_value_handle cell = nullptr;
	REQUIRE(duckdb_v2_vector_get_value(vec, 0, &cell, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Render(cell) == "POINT (42 1337)");
	duckdb_v2_value_handle wkb = nullptr;
	REQUIRE(duckdb_v2_value_cast(fx.factory, cell, blob_type, &wkb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ConsumeBlob(wkb) == std::string(reinterpret_cast<const char *>(POINT_WKB), sizeof(POINT_WKB)));
	duckdb_v2_value_destroy(&cell);

	REQUIRE(duckdb_v2_data_chunk_destroy(&chunk) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&blob_type);
	duckdb_v2_logical_type_destroy(&geom_type);
}

} // namespace test_capi_v2
