#include "test_capi_v2.hpp"

#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/parser/grammar_change.hpp"
#include "duckdb/parser/grammar_extension.hpp"

#include <ostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 tokenizer tests: tokenize_sql and the token iterator. Every test pins the
// exact (type, start, length) triples the engine tokenizer produces, so a
// tokenizer change is visible here.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {
namespace {

struct Tok {
	DUCKDB_V2_TOKEN_TYPE type;
	idx_t start;
	idx_t length;

	bool operator==(const Tok &other) const {
		return type == other.type && start == other.start && length == other.length;
	}
};

std::ostream &operator<<(std::ostream &os, const Tok &tok) {
	return os << "(" << static_cast<int>(tok.type) << ", " << tok.start << ", " << tok.length << ")";
}

using Toks = std::vector<Tok>;

// What tokenize_sql produces for one input: the tokens up to END_OF_INPUT, and the ends_unterminated flag.
struct Lexed {
	Toks tokens;
	bool ends_unterminated;

	bool operator==(const Lexed &other) const {
		return tokens == other.tokens && ends_unterminated == other.ends_unterminated;
	}
};

std::ostream &operator<<(std::ostream &os, const Lexed &lexed) {
	os << (lexed.ends_unterminated ? "unterminated " : "complete ") << "{";
	for (auto &tok : lexed.tokens) {
		os << " " << tok;
	}
	return os << " }";
}

Lexed Complete(Toks tokens) {
	return Lexed {std::move(tokens), false};
}

Lexed Unterminated(Toks tokens) {
	return Lexed {std::move(tokens), true};
}

constexpr auto KEYWORD = DUCKDB_V2_TOKEN_TYPE_KEYWORD;
constexpr auto IDENTIFIER = DUCKDB_V2_TOKEN_TYPE_IDENTIFIER;
constexpr auto STRING = DUCKDB_V2_TOKEN_TYPE_STRING_LITERAL;
constexpr auto NUMBER = DUCKDB_V2_TOKEN_TYPE_NUMBER_LITERAL;
constexpr auto OPERATOR = DUCKDB_V2_TOKEN_TYPE_OPERATOR;
constexpr auto COMMENT = DUCKDB_V2_TOKEN_TYPE_COMMENT;
constexpr auto TERMINATOR = DUCKDB_V2_TOKEN_TYPE_TERMINATOR;
constexpr auto END = DUCKDB_V2_TOKEN_TYPE_END_OF_INPUT;

// Sentinels the out-params are primed with, so a test can tell "written" from "left alone".
constexpr auto STALE_TYPE = static_cast<DUCKDB_V2_TOKEN_TYPE>(99);
constexpr idx_t STALE_IDX = 0xdead;

// One next() call with primed out-params, asserting success.
Tok TokNext(duckdb_v2_token_iterator_handle it) {
	Tok tok {STALE_TYPE, STALE_IDX, STALE_IDX};
	REQUIRE(duckdb_v2_token_iterator_next(it, &tok.type, &tok.start, &tok.length, nullptr) == DUCKDB_V2_ERROR_NONE);
	return tok;
}

// Drains an iterator up to END_OF_INPUT, which is pinned at (len, 0) and not
// included in the result. Every real token must lie inside the input.
Toks TokDrain(duckdb_v2_token_iterator_handle it, idx_t len) {
	Toks out;
	for (idx_t guard = 0; guard <= len + 1; guard++) {
		auto tok = TokNext(it);
		if (tok.type == END) {
			REQUIRE(tok.start == len);
			REQUIRE(tok.length == 0);
			return out;
		}
		REQUIRE(tok.start < len);
		REQUIRE(tok.start + tok.length <= len);
		out.push_back(tok);
	}
	FAIL("iterator yielded more tokens than the input has bytes");
	return out;
}

// The ends_unterminated flag, primed so a test can tell "written" from "left alone".
bool EndsUnterminated(duckdb_v2_token_iterator_handle it) {
	bool out = true;
	REQUIRE(duckdb_v2_token_iterator_ends_unterminated(it, &out, nullptr) == DUCKDB_V2_ERROR_NONE);
	return out;
}

// Tokenize a length-delimited view, drain, destroy. The flag is read before and after draining and must agree.
Lexed TokenizeAll(duckdb_v2_connection_handle conn, duckdb_v2_str sql) {
	duckdb_v2_token_iterator_handle it = nullptr;
	REQUIRE(duckdb_v2_tokenize_sql(conn, &sql, &it, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(it != nullptr);
	auto before = EndsUnterminated(it);
	auto tokens = TokDrain(it, sql.len);
	auto after = EndsUnterminated(it);
	REQUIRE(duckdb_v2_token_iterator_destroy(&it) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(it == nullptr);
	REQUIRE(before == after);
	return Lexed {std::move(tokens), after};
}

Lexed TokenizeAll(duckdb_v2_connection_handle conn, const std::string &sql) {
	return TokenizeAll(conn, Convert(sql));
}

// A grammar extension that adds one unreserved keyword and nothing else, so a
// connection selecting it classifies that word differently from the base grammar.
class TokenizerTestKeywordExtension final : public duckdb::GrammarExtension {
public:
	TokenizerTestKeywordExtension() : GrammarExtension("tokenizer_test_keyword", "adds the keyword ANSWER") {
	}
	duckdb::vector<duckdb::GrammarChange> GetChanges() const override {
		duckdb::vector<duckdb::GrammarChange> changes;
		changes.push_back(duckdb::GrammarChange::AddChoice("UnreservedKeyword", "'ANSWER'"));
		return changes;
	}
};

} // namespace

// ===========================================================================
// Classification
// ===========================================================================

TEST_CASE("V2 tokenizer: all seven lexical classes in one statement", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	std::string sql = "SELECT \"my col\", 'it''s', $$d$$, 1e5, a <> b -- c\n/* d */;";
	REQUIRE(sql.size() == 58);
	Toks expected {
	    {KEYWORD, 0, 6},     // SELECT
	    {IDENTIFIER, 7, 8},  // "my col", quotes included
	    {OPERATOR, 15, 1},   // ,
	    {STRING, 17, 7},     // 'it''s', delimiters and doubled quote included
	    {OPERATOR, 24, 1},   // ,
	    {STRING, 26, 5},     // $$d$$
	    {OPERATOR, 31, 1},   // ,
	    {NUMBER, 33, 3},     // 1e5
	    {OPERATOR, 36, 1},   // ,
	    {IDENTIFIER, 38, 1}, // a
	    {OPERATOR, 40, 2},   // <>
	    {IDENTIFIER, 43, 1}, // b
	    {COMMENT, 45, 5},    // -- c\n, the newline is part of a line comment
	    {COMMENT, 50, 7},    // /* d */
	    {TERMINATOR, 57, 1}, // ;
	};
	REQUIRE(TokenizeAll(fx.conn, sql) == Complete(expected));
}

TEST_CASE("V2 tokenizer: keyword versus identifier follows the grammar", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	// Casing does not matter for keywords; a non-keyword word is an identifier.
	REQUIRE(TokenizeAll(fx.conn, "select Select foo") ==
	        Complete({{KEYWORD, 0, 6}, {KEYWORD, 7, 6}, {IDENTIFIER, 14, 3}}));
	// Quoting a keyword makes it an identifier.
	REQUIRE(TokenizeAll(fx.conn, "\"select\"") == Complete({{IDENTIFIER, 0, 8}}));
}

TEST_CASE("V2 tokenizer: parameters", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	// The tokenizer pushes the '$' of a parameter as its own operator token.
	Toks expected {
	    {KEYWORD, 0, 6},     // SELECT
	    {OPERATOR, 7, 1},    // $
	    {NUMBER, 8, 1},      // 1
	    {OPERATOR, 9, 1},    // ,
	    {OPERATOR, 11, 1},   // $
	    {IDENTIFIER, 12, 3}, // foo
	};
	REQUIRE(TokenizeAll(fx.conn, "SELECT $1, $foo") == Complete(expected));
}

TEST_CASE("V2 tokenizer: operators and the trailing-plus trimming rule", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	// An operator run cannot end in '+' unless it contains a special character, so '+-' splits.
	REQUIRE(TokenizeAll(fx.conn, "a+-b") ==
	        Complete({{IDENTIFIER, 0, 1}, {OPERATOR, 1, 1}, {OPERATOR, 2, 1}, {IDENTIFIER, 3, 1}}));
	// '~' is special, so '~+' stays one operator.
	REQUIRE(TokenizeAll(fx.conn, "a~+b") == Complete({{IDENTIFIER, 0, 1}, {OPERATOR, 1, 2}, {IDENTIFIER, 3, 1}}));
	// Multi-character operators stay whole.
	REQUIRE(TokenizeAll(fx.conn, "a->>b") == Complete({{IDENTIFIER, 0, 1}, {OPERATOR, 1, 3}, {IDENTIFIER, 4, 1}}));
	REQUIRE(TokenizeAll(fx.conn, "x::int") == Complete({{IDENTIFIER, 0, 1}, {OPERATOR, 1, 2}, {KEYWORD, 3, 3}}));
	// Parentheses and commas are operator tokens, one byte each.
	REQUIRE(TokenizeAll(fx.conn, "f(1,2)") == Complete({{IDENTIFIER, 0, 1},
	                                                    {OPERATOR, 1, 1},
	                                                    {NUMBER, 2, 1},
	                                                    {OPERATOR, 3, 1},
	                                                    {NUMBER, 4, 1},
	                                                    {OPERATOR, 5, 1}}));
}

// ===========================================================================
// Statement boundaries and comments
// ===========================================================================

TEST_CASE("V2 tokenizer: terminators only at statement level", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	std::string sql = "SELECT 1; SELECT ';'; -- ;\nSELECT 2;";
	REQUIRE(sql.size() == 36);
	Toks expected {
	    {KEYWORD, 0, 6},     // SELECT
	    {NUMBER, 7, 1},      // 1
	    {TERMINATOR, 8, 1},  // ;
	    {KEYWORD, 10, 6},    // SELECT
	    {STRING, 17, 3},     // ';' is a string, not a terminator
	    {TERMINATOR, 20, 1}, // ;
	    {COMMENT, 22, 5},    // -- ;\n, the ; is inside the comment
	    {KEYWORD, 27, 6},    // SELECT
	    {NUMBER, 34, 1},     // 2
	    {TERMINATOR, 35, 1}, // ;
	};
	REQUIRE(TokenizeAll(fx.conn, sql) == Complete(expected));
}

TEST_CASE("V2 tokenizer: multi-statement input with a trailing comment", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	std::string sql = "SELECT 1; SELECT 2 -- x\n";
	REQUIRE(sql.size() == 24);
	Toks expected {
	    {KEYWORD, 0, 6},    // SELECT
	    {NUMBER, 7, 1},     // 1
	    {TERMINATOR, 8, 1}, // ;
	    {KEYWORD, 10, 6},   // SELECT
	    {NUMBER, 17, 1},    // 2
	    {COMMENT, 19, 5},   // -- x\n
	};
	REQUIRE(TokenizeAll(fx.conn, sql) == Complete(expected));
}

TEST_CASE("V2 tokenizer: line comment endings", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	// No newline: the comment runs to the end of the input, and END_OF_INPUT follows at len.
	REQUIRE(TokenizeAll(fx.conn, "SELECT 1 -- end") ==
	        Unterminated({{KEYWORD, 0, 6}, {NUMBER, 7, 1}, {COMMENT, 9, 6}}));
	// \r\n: the comment ends after the \r; the \n is whitespace.
	REQUIRE(TokenizeAll(fx.conn, "SELECT 1 -- x\r\n2") ==
	        Complete({{KEYWORD, 0, 6}, {NUMBER, 7, 1}, {COMMENT, 9, 5}, {NUMBER, 15, 1}}));
}

// ===========================================================================
// Malformed input never fails
// ===========================================================================

TEST_CASE("V2 tokenizer: unterminated tokens run to the end of the input", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	REQUIRE(TokenizeAll(fx.conn, "SELECT 'abc") == Unterminated({{KEYWORD, 0, 6}, {STRING, 7, 4}}));
	REQUIRE(TokenizeAll(fx.conn, "SELECT \"abc") == Unterminated({{KEYWORD, 0, 6}, {IDENTIFIER, 7, 4}}));
	REQUIRE(TokenizeAll(fx.conn, "SELECT /* abc") == Unterminated({{KEYWORD, 0, 6}, {COMMENT, 7, 6}}));
	REQUIRE(TokenizeAll(fx.conn, "SELECT $tag$abc") == Unterminated({{KEYWORD, 0, 6}, {STRING, 7, 8}}));

	// The closed counterparts, each ending the input on its closing delimiter.
	REQUIRE(TokenizeAll(fx.conn, "SELECT 'abc'") == Complete({{KEYWORD, 0, 6}, {STRING, 7, 5}}));
	REQUIRE(TokenizeAll(fx.conn, "SELECT \"abc\"") == Complete({{KEYWORD, 0, 6}, {IDENTIFIER, 7, 5}}));
	REQUIRE(TokenizeAll(fx.conn, "SELECT /* abc */") == Complete({{KEYWORD, 0, 6}, {COMMENT, 7, 9}}));
	REQUIRE(TokenizeAll(fx.conn, "SELECT $tag$abc$tag$") == Complete({{KEYWORD, 0, 6}, {STRING, 7, 13}}));
}

TEST_CASE("V2 tokenizer: ends_unterminated is a property of the input", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert("SELECT 'abc");
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Before, during and after draining, and after exhaustion, the answer is the same.
	REQUIRE(EndsUnterminated(it));
	REQUIRE(TokNext(it) == Tok {KEYWORD, 0, 6});
	REQUIRE(EndsUnterminated(it));
	REQUIRE(TokNext(it) == Tok {STRING, 7, 4});
	REQUIRE(TokNext(it) == Tok {END, 11, 0});
	REQUIRE(EndsUnterminated(it));
	REQUIRE(TokNext(it) == Tok {END, 11, 0});
	REQUIRE(EndsUnterminated(it));
	duckdb_v2_token_iterator_destroy(&it);
}

TEST_CASE("V2 tokenizer: bytes, not characters", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	// SELECT 'héllo' AS ü: é and ü are two bytes each, and offsets count bytes.
	std::string sql = "SELECT 'h\xc3\xa9llo' AS \xc3\xbc";
	REQUIRE(sql.size() == 21);
	REQUIRE(TokenizeAll(fx.conn, sql) ==
	        Complete({{KEYWORD, 0, 6}, {STRING, 7, 8}, {KEYWORD, 16, 2}, {IDENTIFIER, 19, 2}}));

	// Invalid UTF-8 is not validated: a lone 0xFF byte is an identifier character.
	std::string invalid = "SELECT a\xff"
	                      "b";
	REQUIRE(invalid.size() == 10);
	REQUIRE(TokenizeAll(fx.conn, invalid) == Complete({{KEYWORD, 0, 6}, {IDENTIFIER, 7, 3}}));

	// An interior NUL is an ordinary byte; the view's length, not a terminator, bounds the input.
	std::string with_nul("SELECT 1\0SELECT 2", 17);
	REQUIRE(TokenizeAll(fx.conn, with_nul) ==
	        Complete({{KEYWORD, 0, 6}, {NUMBER, 7, 1}, {IDENTIFIER, 8, 7}, {NUMBER, 16, 1}}));
}

// ===========================================================================
// Exhaustion
// ===========================================================================

TEST_CASE("V2 tokenizer: exhaustion is in-band and idempotent", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	std::string sql = "SELECT 1";
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert(sql);
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(TokNext(it) == Tok {KEYWORD, 0, 6});
	REQUIRE(TokNext(it) == Tok {NUMBER, 7, 1});
	// No token at len other than END_OF_INPUT, and every further call reports it again.
	for (int i = 0; i < 3; i++) {
		REQUIRE(TokNext(it) == Tok {END, sql.size(), 0});
	}
	REQUIRE_FALSE(EndsUnterminated(it));
	duckdb_v2_token_iterator_destroy(&it);
}

TEST_CASE("V2 tokenizer: empty and whitespace-only input", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	REQUIRE(TokenizeAll(fx.conn, "") == Complete({}));
	REQUIRE(TokenizeAll(fx.conn, duckdb_v2_str {nullptr, 0}) == Complete({}));
	// Whitespace is not a token, and END_OF_INPUT still sits at the input length.
	std::string blank = " \t\r\n ";
	REQUIRE(TokenizeAll(fx.conn, blank) == Complete({}));
}

// ===========================================================================
// Lifetime
// ===========================================================================

TEST_CASE("V2 tokenizer: the input is borrowed for the call only", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	auto *buffer = new std::string("SELECT 42");
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert(*buffer);
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);
	// Clobber, then free, the caller's bytes before the first next().
	buffer->assign(buffer->size(), 'x');
	delete buffer;

	REQUIRE(TokDrain(it, 9) == Toks {{KEYWORD, 0, 6}, {NUMBER, 7, 2}});
	REQUIRE_FALSE(EndsUnterminated(it));
	duckdb_v2_token_iterator_destroy(&it);
}

TEST_CASE("V2 tokenizer: the iterator outlives the connection and the database", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert("SELECT 1; 'x");
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_connection_destroy(&fx.conn) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_instance_destroy(&fx.instance) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_environment_destroy(&fx.env) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(TokDrain(it, 12) == Toks {{KEYWORD, 0, 6}, {NUMBER, 7, 1}, {TERMINATOR, 8, 1}, {STRING, 10, 2}});
	REQUIRE(EndsUnterminated(it));
	duckdb_v2_token_iterator_destroy(&it);
}

TEST_CASE("V2 tokenizer: the keyword set is the connection's grammar", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	auto &instance = *duckdb::capiv2::Convert(fx.instance)->GetDatabase().instance;
	duckdb::GrammarExtension::Register(instance, duckdb::make_shared_ptr<TokenizerTestKeywordExtension>());

	// Base grammar: ANSWER is a plain identifier.
	REQUIRE(TokenizeAll(fx.conn, "ANSWER") == Complete({{IDENTIFIER, 0, 6}}));

	// Selecting the extension makes it a keyword on that connection only.
	ExecSQL(fx.conn, "SET active_grammar_extensions = ['tokenizer_test_keyword']");
	REQUIRE(TokenizeAll(fx.conn, "ANSWER") == Complete({{KEYWORD, 0, 6}}));

	duckdb_v2_connection_handle other = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(TokenizeAll(other, "ANSWER") == Complete({{IDENTIFIER, 0, 6}}));
	duckdb_v2_connection_destroy(&other);

	// The grammar is read when the iterator is created, not when it is stepped.
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert("ANSWER");
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);
	ExecSQL(fx.conn, "RESET active_grammar_extensions");
	REQUIRE(TokDrain(it, 6) == Toks {{KEYWORD, 0, 6}});
	duckdb_v2_token_iterator_destroy(&it);
	REQUIRE(TokenizeAll(fx.conn, "ANSWER") == Complete({{IDENTIFIER, 0, 6}}));
}

// ===========================================================================
// Argument checking and destruction
// ===========================================================================

TEST_CASE("V2 tokenizer: tokenize_sql argument checks", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	auto stale = reinterpret_cast<duckdb_v2_token_iterator_handle>(uintptr_t(STALE_IDX));
	auto sql = Convert("SELECT 1");

	// A null connection or a null view with a non-zero length is an input error, and the slot is reset.
	auto it = stale;
	REQUIRE(duckdb_v2_tokenize_sql(nullptr, &sql, &it, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(it == nullptr);
	it = stale;
	duckdb_v2_error_info_handle err = nullptr;
	auto sql_str = duckdb_v2_str {nullptr, 3};
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(it == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_str message = {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(err, &message) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Convert(message).find("sql") != std::string::npos);
	duckdb_v2_error_info_destroy(&err);

	// A null out slot is an input error.
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 tokenizer: next argument checks", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert("SELECT 1");
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);

	// All three out-params are required.
	auto type = STALE_TYPE;
	idx_t start = STALE_IDX;
	idx_t length = STALE_IDX;
	REQUIRE(duckdb_v2_token_iterator_next(it, nullptr, &start, &length, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_token_iterator_next(it, &type, nullptr, &length, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_token_iterator_next(it, &type, &start, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A null iterator fails and resets the out-params.
	REQUIRE(duckdb_v2_token_iterator_next(nullptr, &type, &start, &length, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(type == DUCKDB_V2_TOKEN_TYPE_INVALID);
	REQUIRE(start == 0);
	REQUIRE(length == 0);

	// The failed calls did not advance the iterator.
	REQUIRE(TokDrain(it, 8) == Toks {{KEYWORD, 0, 6}, {NUMBER, 7, 1}});
	duckdb_v2_token_iterator_destroy(&it);
}

TEST_CASE("V2 tokenizer: ends_unterminated argument checks", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	duckdb_v2_token_iterator_handle it = nullptr;
	auto sql_str = Convert("SELECT 'abc");
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);

	// A null slot is an input error; a null iterator fails and resets the slot.
	REQUIRE(duckdb_v2_token_iterator_ends_unterminated(it, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	bool out = true;
	REQUIRE(duckdb_v2_token_iterator_ends_unterminated(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE_FALSE(out);

	REQUIRE(EndsUnterminated(it));
	duckdb_v2_token_iterator_destroy(&it);
}

TEST_CASE("V2 tokenizer: destroy is null-safe and idempotent", "[capi_v2][tokenizer]") {
	EnvFixture fx;
	REQUIRE(duckdb_v2_token_iterator_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_token_iterator_handle it = nullptr;
	REQUIRE(duckdb_v2_token_iterator_destroy(&it) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(it == nullptr);

	auto sql_str = Convert("SELECT 1");
	REQUIRE(duckdb_v2_tokenize_sql(fx.conn, &sql_str, &it, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(it != nullptr);
	// Destroying a half-consumed iterator, then destroying the emptied slot again.
	REQUIRE(TokNext(it) == Tok {KEYWORD, 0, 6});
	REQUIRE(duckdb_v2_token_iterator_destroy(&it) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(it == nullptr);
	REQUIRE(duckdb_v2_token_iterator_destroy(&it) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(it == nullptr);
}

} // namespace test_capi_v2
