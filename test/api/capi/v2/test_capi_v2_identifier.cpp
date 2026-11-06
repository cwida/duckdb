#include "test_capi_v2.hpp"

#include <string>

// ---------------------------------------------------------------------------
// V2 identifier tests: rendering names into SQL through the engine's own
// quoting rules.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// Renders a name through the two-call text protocol.
std::string RenderQuoted(std::string_view name, DUCKDB_V2_ERROR &rc) {
	return RenderText(
	    [&](char *buf, idx_t cap, idx_t *len) {
		    return duckdb_v2_identifier_render_quoted(name.data(), name.size(), buf, cap, len, nullptr);
	    },
	    rc);
}

std::string RenderQuoted(std::string_view name) {
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto out = RenderQuoted(name, rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

} // namespace

TEST_CASE("V2 identifier: render quotes only when required", "[capi_v2][identifier]") {
	// Legal bare identifiers pass through, casing preserved.
	REQUIRE(RenderQuoted("col") == "col");
	REQUIRE(RenderQuoted("MyCol") == "MyCol");
	REQUIRE(RenderQuoted("a_1") == "a_1");

	// Keywords, spaces, leading digits and embedded quotes are quoted and escaped.
	REQUIRE(RenderQuoted("select") == "\"select\"");
	REQUIRE(RenderQuoted("my col") == "\"my col\"");
	REQUIRE(RenderQuoted("1abc") == "\"1abc\"");
	REQUIRE(RenderQuoted("a\"b") == "\"a\"\"b\"");
}

TEST_CASE("V2 identifier: rendered names round-trip through the parser", "[capi_v2][identifier]") {
	EnvFixture fx;

	// A quoted name is usable verbatim in SQL and comes back as the original name.
	const char *names[] = {"plain", "Mixed Case", "select", "we\"ird", "1abc"};
	for (auto name : names) {
		ExecSQL(fx.conn, ("CREATE TABLE " + RenderQuoted(name) + " (i INTEGER)").c_str());
		duckdb_v2_result_handle result = nullptr;
		REQUIRE(Query(fx.conn, ("SELECT * FROM " + RenderQuoted(name)).c_str(), &result) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_result_destroy(&result);
	}
}

TEST_CASE("V2 identifier: buffer protocol and null arguments", "[capi_v2][identifier]") {
	std::string_view view = "my col";
	idx_t length = 0;

	// A null buffer reports the length only.
	REQUIRE(duckdb_v2_identifier_render_quoted(view.data(), view.size(), nullptr, 0, &length, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(length == 8);

	// A buffer without room for the terminator is refused, reporting the required length.
	char small[8];
	length = 0;
	REQUIRE(duckdb_v2_identifier_render_quoted(view.data(), view.size(), small, sizeof(small), &length, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_OBJECT_SIZE);
	REQUIRE(length == 8);

	// A big enough buffer receives the text and a terminator.
	char big[9];
	REQUIRE(duckdb_v2_identifier_render_quoted(view.data(), view.size(), big, sizeof(big), &length, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(big) == "\"my col\"");

	// A null name view with a non-zero length, or a null length slot, is an input error.
	REQUIRE(duckdb_v2_identifier_render_quoted(nullptr, 3, nullptr, 0, &length, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_identifier_render_quoted(view.data(), view.size(), nullptr, 0, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 identifier: names must be valid UTF-8", "[capi_v2][identifier]") {
	idx_t length = 0;
	std::string_view name_str = "\x80";
	REQUIRE(duckdb_v2_identifier_render_quoted(name_str.data(), name_str.size(), nullptr, 0, &length, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	const std::string trailing_invalid("ok\0\x80", 4);
	auto name_str2 = std::string_view(trailing_invalid);
	REQUIRE(duckdb_v2_identifier_render_quoted(name_str2.data(), name_str2.size(), nullptr, 0, &length, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	std::string_view name_str3 = "caf\xc3\xa9";
	REQUIRE(duckdb_v2_identifier_render_quoted(name_str3.data(), name_str3.size(), nullptr, 0, &length, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
