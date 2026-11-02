#include "test_capi_v2.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 qualified name tests. A qualified name is a path of identifier parts
// whose last element is the object; partial qualification is a shorter path,
// never an empty placeholder part.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_qname_handle QNameParse(const char *text) {
	duckdb_v2_qname_handle name = nullptr;
	auto text_str = Convert(text);
	REQUIRE(duckdb_v2_qname_parse(&text_str, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name != nullptr);
	return name;
}

duckdb_v2_qname_handle QNameCreate(const std::vector<const char *> &parts) {
	std::vector<duckdb_v2_identifier_t> views;
	for (auto *part : parts) {
		views.push_back(Convert(part));
	}
	duckdb_v2_qname_handle name = nullptr;
	REQUIRE(duckdb_v2_qname_create(views.empty() ? nullptr : views.data(), views.size(), &name, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	return name;
}

std::vector<std::string> QNameParts(duckdb_v2_qname_handle name) {
	idx_t count = 0;
	REQUIRE(duckdb_v2_qname_get_part_count(name, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::vector<std::string> out;
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_identifier_t part = {nullptr, 0};
		REQUIRE(duckdb_v2_qname_get_part(name, i, &part, nullptr) == DUCKDB_V2_ERROR_NONE);
		out.push_back(Convert(part));
	}
	return out;
}

std::string QNameRender(duckdb_v2_qname_handle name) {
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto out = RenderText(
	    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_qname_render(name, buf, cap, len, nullptr); }, rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

bool QNameEquals(duckdb_v2_qname_handle left, duckdb_v2_qname_handle right) {
	bool result = false;
	REQUIRE(duckdb_v2_qname_equals(left, right, &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	return result;
}

uint64_t QNameHash(duckdb_v2_qname_handle name) {
	uint64_t hash = 0;
	REQUIRE(duckdb_v2_qname_hash(name, &hash, nullptr) == DUCKDB_V2_ERROR_NONE);
	return hash;
}

} // namespace

TEST_CASE("V2 qname: create and inspect", "[capi_v2][qname]") {
	auto one = QNameCreate({"tbl"});
	REQUIRE(QNameParts(one) == std::vector<std::string> {"tbl"});

	auto two = QNameCreate({"sch", "tbl"});
	REQUIRE(QNameParts(two) == std::vector<std::string> {"sch", "tbl"});

	auto three = QNameCreate({"cat", "sch", "tbl"});
	REQUIRE(QNameParts(three) == std::vector<std::string> {"cat", "sch", "tbl"});

	// Reading past the end is refused rather than silently empty.
	duckdb_v2_identifier_t part = {nullptr, 0};
	REQUIRE(duckdb_v2_qname_get_part(one, 1, &part, nullptr) == DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);

	duckdb_v2_qname_destroy(&one);
	duckdb_v2_qname_destroy(&two);
	duckdb_v2_qname_destroy(&three);
}

TEST_CASE("V2 qname: parse", "[capi_v2][qname]") {
	auto plain = QNameParse("tbl");
	REQUIRE(QNameParts(plain) == std::vector<std::string> {"tbl"});

	auto qualified = QNameParse("cat.sch.tbl");
	REQUIRE(QNameParts(qualified) == std::vector<std::string> {"cat", "sch", "tbl"});

	// A quoted part may contain dots, and doubled quotes are one quote.
	auto quoted = QNameParse("\"we.ird\".tbl");
	REQUIRE(QNameParts(quoted) == std::vector<std::string> {"we.ird", "tbl"});
	auto escaped = QNameParse("\"a\"\"b\"");
	REQUIRE(QNameParts(escaped) == std::vector<std::string> {"a\"b"});

	duckdb_v2_qname_destroy(&plain);
	duckdb_v2_qname_destroy(&qualified);
	duckdb_v2_qname_destroy(&quoted);
	duckdb_v2_qname_destroy(&escaped);
}

TEST_CASE("V2 qname: render round-trips through parse", "[capi_v2][qname]") {
	// Quoting is applied only where the identifier requires it.
	auto plain = QNameCreate({"cat", "sch", "tbl"});
	REQUIRE(QNameRender(plain) == "cat.sch.tbl");

	auto needs_quotes = QNameCreate({"we.ird", "tbl"});
	auto rendered = QNameRender(needs_quotes);
	REQUIRE(rendered == "\"we.ird\".tbl");

	auto reparsed = QNameParse(rendered.c_str());
	REQUIRE(QNameEquals(reparsed, needs_quotes));

	duckdb_v2_qname_destroy(&plain);
	duckdb_v2_qname_destroy(&needs_quotes);
	duckdb_v2_qname_destroy(&reparsed);
}

TEST_CASE("V2 qname: equality is case-insensitive and agrees with hash", "[capi_v2][qname]") {
	auto lower = QNameCreate({"cat", "sch", "tbl"});
	auto upper = QNameCreate({"CAT", "Sch", "TBL"});
	auto shorter = QNameCreate({"sch", "tbl"});
	auto different = QNameCreate({"cat", "sch", "other"});

	REQUIRE(QNameEquals(lower, upper));
	REQUIRE(QNameHash(lower) == QNameHash(upper));
	// Fewer parts is a different name, not a partial match.
	REQUIRE_FALSE(QNameEquals(lower, shorter));
	REQUIRE_FALSE(QNameEquals(lower, different));

	duckdb_v2_qname_destroy(&lower);
	duckdb_v2_qname_destroy(&upper);
	duckdb_v2_qname_destroy(&shorter);
	duckdb_v2_qname_destroy(&different);
}

TEST_CASE("V2 qname: construction refusals", "[capi_v2][qname]") {
	duckdb_v2_qname_handle name = nullptr;

	// Between one and three parts, none empty.
	REQUIRE(duckdb_v2_qname_create(nullptr, 0, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(name == nullptr);

	duckdb_v2_identifier_t four[4] = {Convert("a"), Convert("b"), Convert("c"), Convert("d")};
	REQUIRE(duckdb_v2_qname_create(four, 4, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_identifier_t with_empty[2] = {Convert("a"), Convert("")};
	REQUIRE(duckdb_v2_qname_create(with_empty, 2, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_identifier_t with_invalid_utf8[2] = {Convert("a"), Convert("\x80")};
	REQUIRE(duckdb_v2_qname_create(with_invalid_utf8, 2, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_identifier_t one[1] = {Convert("a")};
	REQUIRE(duckdb_v2_qname_create(nullptr, 1, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_create(one, 1, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Reject unusable text, invalid UTF-8 and excess parts.
	auto text_str = Convert("");
	REQUIRE(duckdb_v2_qname_parse(&text_str, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto text_str2 = Convert("a.\x80");
	REQUIRE(duckdb_v2_qname_parse(&text_str2, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(name == nullptr);
	auto text_str3 = Convert("a.b.c.d");
	REQUIRE(duckdb_v2_qname_parse(&text_str3, &name, nullptr) != DUCKDB_V2_ERROR_NONE);
	auto text_str4 = Convert("\"unterminated");
	REQUIRE(duckdb_v2_qname_parse(&text_str4, &name, nullptr) != DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 qname: null arguments and destroy null-safety", "[capi_v2][qname]") {
	auto name = QNameCreate({"tbl"});

	idx_t count = 0;
	duckdb_v2_identifier_t part = {nullptr, 0};
	idx_t length = 0;
	bool result = false;
	uint64_t hash = 0;
	REQUIRE(duckdb_v2_qname_get_part_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_get_part_count(name, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_get_part(nullptr, 0, &part, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_get_part(name, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_render(nullptr, nullptr, 0, &length, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_render(name, nullptr, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_equals(nullptr, name, &result, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_equals(name, nullptr, &result, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_equals(name, name, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_hash(nullptr, &hash, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_hash(name, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A buffer that cannot hold the text plus its terminator is refused, with the required length reported.
	char small[2] = {'\0', '\0'};
	length = 0;
	REQUIRE(duckdb_v2_qname_render(name, small, 1, &length, nullptr) == DUCKDB_V2_ERROR_INPUT_OBJECT_SIZE);
	REQUIRE(length == 3);

	REQUIRE(duckdb_v2_qname_destroy(&name) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name == nullptr);
	REQUIRE(duckdb_v2_qname_destroy(&name) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_qname_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
