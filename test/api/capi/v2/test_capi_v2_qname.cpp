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

duckdb_v2_qname_handle QNameParse(std::string_view text) {
	duckdb_v2_qname_handle name = nullptr;
	REQUIRE(duckdb_v2_qname_parse(text.data(), text.size(), &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(name != nullptr);
	return name;
}

duckdb_v2_qname_handle QNameCreate(const std::vector<const char *> &parts) {
	StrArray views;
	for (auto *part : parts) {
		views.push_back(std::string_view(part));
	}
	duckdb_v2_qname_handle name = nullptr;
	REQUIRE(duckdb_v2_qname_create(views.ptrs(), views.lengths(), views.size(), &name, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	return name;
}

std::vector<std::string> QNameParts(duckdb_v2_qname_handle name) {
	idx_t count = 0;
	REQUIRE(duckdb_v2_qname_get_part_count(name, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::vector<std::string> out;
	for (idx_t i = 0; i < count; i++) {
		const char *part = nullptr;
		idx_t part_length = 0;
		REQUIRE(duckdb_v2_qname_get_part(name, i, &part, &part_length, nullptr) == DUCKDB_V2_ERROR_NONE);
		out.push_back(std::string(part, part_length));
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
	const char *part = nullptr;
	idx_t part_length = 0;
	REQUIRE(duckdb_v2_qname_get_part(one, 1, &part, &part_length, nullptr) == DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);

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
	REQUIRE(duckdb_v2_qname_create(nullptr, nullptr, 0, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(name == nullptr);

	StrArray four = {std::string_view("a"), std::string_view("b"), std::string_view("c"), std::string_view("d")};
	REQUIRE(duckdb_v2_qname_create(four.ptrs(), four.lengths(), 4, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	StrArray with_empty = {std::string_view("a"), std::string_view("")};
	REQUIRE(duckdb_v2_qname_create(with_empty.ptrs(), with_empty.lengths(), 2, &name, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	StrArray with_invalid_utf8 = {std::string_view("a"), std::string_view("\x80")};
	REQUIRE(duckdb_v2_qname_create(with_invalid_utf8.ptrs(), with_invalid_utf8.lengths(), 2, &name, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	StrArray one = {std::string_view("a")};
	REQUIRE(duckdb_v2_qname_create(nullptr, nullptr, 1, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_create(one.ptrs(), nullptr, 1, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_create(one.ptrs(), one.lengths(), 1, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	StrArray null_with_length;
	null_with_length.push_back(nullptr, 1);
	REQUIRE(duckdb_v2_qname_create(null_with_length.ptrs(), null_with_length.lengths(), 1, &name, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(name == nullptr);

	// Reject unusable text, invalid UTF-8 and excess parts.
	std::string_view text_str = "";
	REQUIRE(duckdb_v2_qname_parse(text_str.data(), text_str.size(), &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	std::string_view text_str2 = "a.\x80";
	REQUIRE(duckdb_v2_qname_parse(text_str2.data(), text_str2.size(), &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(name == nullptr);
	std::string_view text_str3 = "a.b.c.d";
	REQUIRE(duckdb_v2_qname_parse(text_str3.data(), text_str3.size(), &name, nullptr) != DUCKDB_V2_ERROR_NONE);
	std::string_view text_str4 = "\"unterminated";
	REQUIRE(duckdb_v2_qname_parse(text_str4.data(), text_str4.size(), &name, nullptr) != DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 qname: null arguments and destroy null-safety", "[capi_v2][qname]") {
	auto name = QNameCreate({"tbl"});

	idx_t count = 0;
	const char *part = nullptr;
	idx_t part_length = 0;
	idx_t length = 0;
	bool result = false;
	uint64_t hash = 0;
	REQUIRE(duckdb_v2_qname_get_part_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_get_part_count(name, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_get_part(nullptr, 0, &part, &part_length, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_qname_get_part(name, 0, nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
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

TEST_CASE("V2 qname: create reads each part by its length", "[capi_v2][qname]") {
	// The parts share one buffer and none is null-terminated.
	const char buffer[] = {'c', 'a', 't', 's', 'c', 'h', 't', 'b', 'l'};
	const char *parts[3] = {buffer, buffer + 3, buffer + 6};
	const idx_t parts_lengths[3] = {3, 3, 3};
	duckdb_v2_qname_handle name = nullptr;
	REQUIRE(duckdb_v2_qname_create(parts, parts_lengths, 3, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(QNameRender(name) == "cat.sch.tbl");
	duckdb_v2_qname_destroy(&name);
}

} // namespace test_capi_v2
