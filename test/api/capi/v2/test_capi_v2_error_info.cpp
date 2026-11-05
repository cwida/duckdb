#include "test_capi_v2.hpp"

// ---------------------------------------------------------------------------
// V2 error_info: a minimal, flat surface -- get_code, get_text, get_raw_message
// (plus setters/destroy). No get_type, no get_position: the long tail (type name,
// position, candidates) comes from errors_as_json. get_raw_message is the body
// with the "<Type> Error: " prefix stripped, not derivable from get_text (no type
// name to rebuild the prefix). The parse boundary is not special: statement_iterator_next
// renders a parse error through the engine's public ProcessError, so errors_as_json
// governs parse errors too (LINE/caret off, JSON on).
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
namespace {

// Runs a query expected to fail at prepare time; returns the err handle (caller
// destroys). A real result slot is passed (a NULL out_result is itself rejected).
duckdb_v2_error_info_handle FailingQuery(duckdb_v2_connection_handle conn, std::string_view sql,
                                         DUCKDB_V2_ERROR expected_code) {
	duckdb_v2_error_info_handle err = nullptr;
	duckdb_v2_result_handle r = nullptr;
	auto rc = Query(conn, sql, &r, &err);
	REQUIRE(rc == expected_code);
	REQUIRE(r == nullptr);
	REQUIRE(err != nullptr);
	return err;
}

} // namespace

// ===========================================================================
// Binder error: get_raw_message is the unprefixed body, get_text contains it.
// ===========================================================================

TEST_CASE("V2 error: get_raw_message on a binder error", "[capi_v2][error]") {
	EnvFixture fx;

	auto err = FailingQuery(fx.conn, "SELECT * FROM no_such_table", DUCKDB_V2_ERROR_DATABASE_CATALOG);

	const char *raw = nullptr;
	idx_t raw_length = 0;
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, &raw, &raw_length) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(raw != nullptr);
	REQUIRE(std::string(raw, raw_length).find("no_such_table") != std::string::npos);
	// The body carries no "<Type> Error: " prefix.
	REQUIRE(std::string(raw, raw_length).rfind("Catalog Error:", 0) != 0);

	// get_text is the full, prefixed message; the body is a suffix of it.
	const char *text = nullptr;
	idx_t text_length = 0;
	REQUIRE(duckdb_v2_error_info_get_text(err, &text, &text_length) == DUCKDB_V2_ERROR_NONE);
	auto full = std::string(text, text_length);
	REQUIRE(full.rfind("Catalog Error:", 0) == 0);
	REQUIRE(full.find(std::string(raw, raw_length)) != std::string::npos);

	duckdb_v2_error_info_destroy(&err);
}

// ===========================================================================
// A directly-set message has no body: get_raw_message returns {NULL, 0}.
// ===========================================================================

TEST_CASE("V2 error: directly-set messages have no raw body", "[capi_v2][error]") {
	duckdb_v2_error_info_handle err = nullptr;
	SetErrorInfo(&err, DUCKDB_V2_ERROR_API, "boom");
	REQUIRE(err != nullptr);

	const char *raw = nullptr;
	idx_t raw_length = 0;
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, &raw, &raw_length) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(raw == nullptr);
	REQUIRE(raw_length == 0);

	// set_text leaves no raw body either.
	std::string_view text_str = "reset";
	REQUIRE(duckdb_v2_error_info_set_text(err, text_str.data(), text_str.size()) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, &raw, &raw_length) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(raw == nullptr);
	REQUIRE(raw_length == 0);

	// set an error code
	REQUIRE(duckdb_v2_error_info_set_code(err, DUCKDB_V2_ERROR_CONFIGURATION_PERMISSION) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_ERROR out_err_code;
	REQUIRE(duckdb_v2_error_info_get_code(err, &out_err_code) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(out_err_code == DUCKDB_V2_ERROR_CONFIGURATION_PERMISSION);

	duckdb_v2_error_info_destroy(&err);
}

// ===========================================================================
// set_text clears the body, so a prior failure's body never leaks on slot reuse.
// ===========================================================================

TEST_CASE("V2 error: raw body does not leak across slot reuse", "[capi_v2][error]") {
	EnvFixture fx;

	duckdb_v2_error_info_handle err = nullptr;
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM no_such_table", &r, &err) == DUCKDB_V2_ERROR_DATABASE_CATALOG);
	REQUIRE(r == nullptr);
	REQUIRE(err != nullptr);
	const char *raw = nullptr;
	idx_t raw_length = 0;
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, &raw, &raw_length) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(raw != nullptr);

	// Reuse the slot with a directly-set message: the raw body is cleared.
	std::string_view text_str = "manual";
	REQUIRE(duckdb_v2_error_info_set_text(err, text_str.data(), text_str.size()) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, &raw, &raw_length) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(raw == nullptr);
	REQUIRE(raw_length == 0);

	duckdb_v2_error_info_destroy(&err);
}

// ===========================================================================
// Null-arg rejection.
// ===========================================================================

TEST_CASE("V2 error: get_raw_message rejects null args", "[capi_v2][error]") {
	duckdb_v2_error_info_handle err = nullptr;
	SetErrorInfo(&err, DUCKDB_V2_ERROR_API, "x");

	const char *out = nullptr;
	idx_t out_length = 0;
	REQUIRE(duckdb_v2_error_info_get_raw_text(nullptr, &out, &out_length) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 error: set_text rejects a null view", "[capi_v2][error]") {
	duckdb_v2_error_info_handle err = nullptr;
	SetErrorInfo(&err, DUCKDB_V2_ERROR_API, "x");

	REQUIRE(duckdb_v2_error_info_set_text(err, nullptr, 1) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_error_info_set_text(err, nullptr, 3) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 error: setters on a null info handle are no-ops", "[capi_v2][error]") {
	REQUIRE(duckdb_v2_error_info_set_code(nullptr, DUCKDB_V2_ERROR_API) == DUCKDB_V2_ERROR_NONE);
	std::string_view text_str = "x";
	REQUIRE(duckdb_v2_error_info_set_text(nullptr, text_str.data(), text_str.size()) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_error_info_set_text(nullptr, nullptr, 0) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Default mode: the parse boundary renders the LINE/caret annotation. The parse
// error surfaces from the next() that reaches the failing statement, not from
// parse_sql, but its rendered shape matches the eager path.
// ===========================================================================

TEST_CASE("V2 error: statement_iterator_next renders the location like the eager path", "[capi_v2][error]") {
	EnvFixture fx;

	// parse_sql parses nothing, so it succeeds; the first next() yields "SELECT 1".
	duckdb_v2_statement_iterator_handle iter = nullptr;
	std::string_view sql = "SELECT 1; SELEKT 2";
	REQUIRE(duckdb_v2_parse_sql(fx.conn, sql.data(), sql.size(), true, &iter, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_handle stmt = nullptr;
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(stmt != nullptr);
	duckdb_v2_sql_statement_destroy(&stmt);

	// The next() that reaches "SELEKT 2" raises the parse error.
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, &err) == DUCKDB_V2_ERROR_QUERY_PARSER);
	REQUIRE(stmt == nullptr);
	REQUIRE(err != nullptr);

	const char *text = nullptr;
	idx_t text_length = 0;
	REQUIRE(duckdb_v2_error_info_get_text(err, &text, &text_length) == DUCKDB_V2_ERROR_NONE);
	auto full = std::string(text, text_length);
	INFO("text: " << full);
	REQUIRE(full.rfind("Parser Error:", 0) == 0);
	// The default branch folds the offending line and a caret into the message.
	REQUIRE(full.find("LINE 1") != std::string::npos);
	REQUIRE(full.find("^") != std::string::npos);

	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_statement_iterator_destroy(&iter);
}

// ===========================================================================
// errors_as_json on: the parse boundary emits the JSON form (carries position, type).
// ===========================================================================

TEST_CASE("V2 error: errors_as_json makes the parse boundary emit JSON", "[capi_v2][error]") {
	EnvFixture fx;

	ExecSQL(fx.conn, "SET errors_as_json=true");

	// The single statement fails to parse; the error surfaces from the first next().
	duckdb_v2_statement_iterator_handle iter = nullptr;
	std::string_view sql = "SELEKT 1";
	REQUIRE(duckdb_v2_parse_sql(fx.conn, sql.data(), sql.size(), true, &iter, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_handle stmt = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_statement_iterator_next(iter, &stmt, &err) == DUCKDB_V2_ERROR_QUERY_PARSER);
	REQUIRE(stmt == nullptr);
	REQUIRE(err != nullptr);

	// The body is the bare JSON object carrying the failure position.
	const char *raw = nullptr;
	idx_t raw_length = 0;
	REQUIRE(duckdb_v2_error_info_get_raw_text(err, &raw, &raw_length) == DUCKDB_V2_ERROR_NONE);
	auto raw_str = std::string(raw, raw_length);
	INFO("errors_as_json raw: " << raw_str);
	REQUIRE(raw_str.rfind("{", 0) == 0);
	REQUIRE(raw_str.find("\"position\"") != std::string::npos);

	// The full text carries the same JSON object.
	const char *text = nullptr;
	idx_t text_length = 0;
	REQUIRE(duckdb_v2_error_info_get_text(err, &text, &text_length) == DUCKDB_V2_ERROR_NONE);
	auto full = std::string(text, text_length);
	INFO("errors_as_json text: " << full);
	REQUIRE(full.find("\"position\"") != std::string::npos);

	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_statement_iterator_destroy(&iter);
}

TEST_CASE("V2 error: SetErrorInfo helper", "[capi_v2][error]") {
	SECTION("SetErrorInfo allocates an info and returns the code") {
		duckdb_v2_error_info_handle err = nullptr;
		auto rc = SetErrorInfo(&err, DUCKDB_V2_ERROR_INPUT_INVALID, "bad input");
		REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(err != nullptr);

		const char *msg = nullptr;
		idx_t msg_length = 0;
		REQUIRE(duckdb_v2_error_info_get_text(err, &msg, &msg_length) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(msg != nullptr);
		REQUIRE(std::string_view(msg, msg_length) == "bad input");

		duckdb_v2_error_info_destroy(&err);
		REQUIRE(err == nullptr);
	}

	SECTION("SetErrorInfo with null message produces an empty message") {
		duckdb_v2_error_info_handle err = nullptr;
		auto rc = SetErrorInfo(&err, DUCKDB_V2_ERROR_INPUT_INVALID, nullptr);
		REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(err != nullptr);

		const char *msg = nullptr;
		idx_t msg_length = 0;
		REQUIRE(duckdb_v2_error_info_get_text(err, &msg, &msg_length) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(msg != nullptr);
		REQUIRE(msg_length == 0);

		duckdb_v2_error_info_destroy(&err);
	}

	SECTION("SetErrorInfo preserves arbitrarily long messages") {
		duckdb_v2_error_info_handle err = nullptr;
		std::string long_msg(4096, 'x');
		SetErrorInfo(&err, DUCKDB_V2_ERROR_API, long_msg.c_str());

		const char *msg = nullptr;
		idx_t msg_length = 0;
		duckdb_v2_error_info_get_text(err, &msg, &msg_length);
		REQUIRE(msg_length == long_msg.size());

		duckdb_v2_error_info_destroy(&err);
	}

	SECTION("SetErrorInfo with nullptr err returns the code and allocates nothing") {
		auto rc = SetErrorInfo(nullptr, DUCKDB_V2_ERROR_INPUT_INVALID, "ignored");
		REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	}

	SECTION("SetErrorInfo replaces a pre-existing info's message") {
		duckdb_v2_error_info_handle err = nullptr;
		SetErrorInfo(&err, DUCKDB_V2_ERROR_INPUT_INVALID, "first");
		SetErrorInfo(&err, DUCKDB_V2_ERROR_API, "second");
		REQUIRE(err != nullptr);

		const char *msg = nullptr;
		idx_t msg_length = 0;
		duckdb_v2_error_info_get_text(err, &msg, &msg_length);
		REQUIRE(std::string_view(msg, msg_length) == "second");

		duckdb_v2_error_info_destroy(&err);
	}
}
TEST_CASE("V2 error: error_info_destroy is null-safe", "[capi_v2][error]") {
	SECTION("destroying a null handle is a no-op") {
		duckdb_v2_error_info_handle err = nullptr;
		REQUIRE(duckdb_v2_error_info_destroy(&err) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(err == nullptr);
	}

	SECTION("destroying via a null pointer-to-handle is a no-op") {
		REQUIRE(duckdb_v2_error_info_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	}

	SECTION("detach + destroy preserves info independently of the original slot") {
		duckdb_v2_error_info_handle err = nullptr;
		SetErrorInfo(&err, DUCKDB_V2_ERROR_API, "boom");

		// Transfer ownership out of `err` — the original slot is now detached.
		duckdb_v2_error_info_handle saved = err;
		err = nullptr;

		const char *msg = nullptr;
		idx_t msg_length = 0;
		REQUIRE(duckdb_v2_error_info_get_text(saved, &msg, &msg_length) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(std::string_view(msg, msg_length) == "boom");
		duckdb_v2_error_info_destroy(&saved);
		REQUIRE(saved == nullptr);
	}
}
} // namespace test_capi_v2
