#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"

using namespace duckdb;

class EmptyKeywordHelper final : public PEGKeywordHelper {
public:
	EmptyKeywordHelper() : literal_table(ParsedGrammar(), {}) {
	}
	const GrammarLiteralTable &GetLiteralTable() const override {
		return literal_table;
	}
	keyword_categories_t GetIdentifierMask(SuggestionState) const override {
		return keyword_categories_t();
	}
	vector<ParserKeyword> KeywordList() const override {
		return {};
	}

private:
	GrammarLiteralTable literal_table;
};

class HookTokenizer final : public Tokenizer {
public:
	HookTokenizer(const PEGKeywordHelper &helper, bool strings, bool backticks, bool split_operators)
	    : Tokenizer(helper), strings(strings), backticks(backticks), split_operators(split_operators) {
	}

protected:
	bool BackslashEscapesStringLiterals() const override {
		return strings;
	}
	bool IsQuotedIdentifierDelimiter(char character) const override {
		return character == '"' || (backticks && character == '`');
	}
	void PushOperatorToken(TokenizerBehavior &behavior, idx_t start, idx_t end) const override {
		auto &sql = behavior.sql;
		if (!split_operators || end - start < 2 || sql[start] != '>') {
			Tokenizer::PushOperatorToken(behavior, start, end);
			return;
		}
		behavior.PushToken(start, start + 1, TokenType::OPERATOR);
		behavior.PushToken(start + 1, end, TokenType::OPERATOR);
	}

private:
	bool strings;
	bool backticks;
	bool split_operators;
};

static vector<MatcherToken> Tokenize(const Tokenizer &tokenizer, const string &sql) {
	vector<MatcherToken> tokens;
	TokenizerBehavior behavior(sql, tokens);
	tokenizer.TokenizeInput(behavior);
	return tokens;
}

TEST_CASE("Register and select a dialect extension", "[api][dialect_extension]") {
	DBConfig config;
	DialectExtension::Register(config, make_uniq<DialectExtension>("test"));

	DuckDB db(nullptr, &config);
	Connection con(db);

	auto dialects = con.Query("SELECT dialect_name FROM duckdb_dialects() ORDER BY dialect_name");
	REQUIRE_NO_FAIL(*dialects);
	REQUIRE(dialects->RowCount() == 1);
	REQUIRE(dialects->Collection().GetValue(0, 0) == Value("test"));

	REQUIRE_NO_FAIL(con.Query("SET current_dialect = 'test'"));
	auto current_dialect = con.Query("SELECT current_setting('current_dialect')");
	REQUIRE_NO_FAIL(*current_dialect);
	REQUIRE(current_dialect->Collection().GetValue(0, 0) == Value("test"));
}

TEST_CASE("Dialect tokenizer hooks are opt-in", "[api][dialect_extension]") {
	EmptyKeywordHelper helper;
	Tokenizer default_tokenizer(helper);
	HookTokenizer hook_tokenizer(helper, true, true, true);

	// backticks are accepted as identifier delimiters by default, a dialect can opt out of them
	HookTokenizer no_backtick_tokenizer(helper, false, false, false);
	auto default_backtick = Tokenize(default_tokenizer, "`a b`");
	auto hooked_backtick = Tokenize(hook_tokenizer, "`a b`");
	auto no_backtick = Tokenize(no_backtick_tokenizer, "`a b`");
	REQUIRE(default_backtick.size() >= 1);
	REQUIRE(default_backtick[0].type == TokenType::IDENTIFIER);
	REQUIRE(default_backtick[0].text == "`a b`");
	REQUIRE(hooked_backtick.size() >= 1);
	REQUIRE(hooked_backtick[0].type == TokenType::IDENTIFIER);
	REQUIRE(hooked_backtick[0].text == "`a b`");
	REQUIRE(no_backtick.size() > 2);

	auto default_operator = Tokenize(default_tokenizer, ">>=");
	auto hooked_operator = Tokenize(hook_tokenizer, ">>=");
	REQUIRE(default_operator.size() >= 1);
	REQUIRE(default_operator[0].text == ">>=");
	REQUIRE(default_operator.size() >= 2);
	REQUIRE(hooked_operator[0].text == ">");
	REQUIRE(hooked_operator[1].text == ">=");

	auto default_string = Tokenize(default_tokenizer, "'a\\'b'");
	auto hooked_string = Tokenize(hook_tokenizer, "'a\\'b'");
	REQUIRE(default_string.size() > 2);
	REQUIRE(hooked_string[0].text == "'a\\'b'");
}

class TerminatorTokenizerBehavior final : public TokenizerBehavior {
public:
	TerminatorTokenizerBehavior(const string &sql, vector<MatcherToken> &tokens, bool autocomplete)
	    : TokenizerBehavior(sql, tokens), autocomplete(autocomplete) {
	}
	TokenType GetTerminator() const override {
		return autocomplete ? TokenType::END_OF_INPUT_AUTOCOMPLETE : TokenType::END_OF_INPUT;
	}

private:
	bool autocomplete;
};

TEST_CASE("Tokenizer preserves delimiters and incomplete input across long runs", "[api][dialect_extension]") {
	EmptyKeywordHelper helper;
	Tokenizer tokenizer(helper);
	HookTokenizer escaped_tokenizer(helper, true, true, false);
	for (idx_t length : {0, 1, 7, 8, 9, 31, 32, 33, 39, 40, 41, 63, 64, 65, 127, 128, 129, 4096}) {
		const string padding(length, 'a');
		for (bool autocomplete : {false, true}) {
			const auto terminator = autocomplete ? TokenType::END_OF_INPUT_AUTOCOMPLETE : TokenType::END_OF_INPUT;
			for (auto &literal : {"'" + padding + "''b'", "E'" + padding + "\\'b'", "\"" + padding + "\"\"b\"",
			                      "`" + padding + "``b`", "$tag$" + padding + "$wrong$b$tag$"}) {
				auto sql = literal + " 42";
				vector<MatcherToken> tokens;
				TerminatorTokenizerBehavior behavior(sql, tokens, autocomplete);
				REQUIRE(tokenizer.TokenizeInput(behavior) == autocomplete);
				REQUIRE(tokens.size() == 3);
				REQUIRE(tokens[0].text == literal);
				REQUIRE(tokens[0].offset == 0);
				REQUIRE(tokens[0].length == literal.size());
				REQUIRE_FALSE(tokens[0].unterminated);
				REQUIRE(tokens[1].text == "42");
				REQUIRE(tokens[1].offset == literal.size() + 1);
				REQUIRE(tokens[2].type == terminator);
			}
			{
				auto sql = "'" + padding + "\\'b' 42";
				vector<MatcherToken> tokens;
				TerminatorTokenizerBehavior behavior(sql, tokens, autocomplete);
				REQUIRE(escaped_tokenizer.TokenizeInput(behavior) == autocomplete);
				REQUIRE(tokens.size() == 3);
				REQUIRE(tokens[0].text == sql.substr(0, sql.size() - 3));
				REQUIRE(tokens[1].text == "42");
				REQUIRE(tokens.back().type == terminator);
			}
			{
				auto sql = "1 /*" + padding + "/*nested*/" + padding + "*/\n2 ";
				vector<MatcherToken> tokens;
				TerminatorTokenizerBehavior behavior(sql, tokens, autocomplete);
				REQUIRE(tokenizer.TokenizeInput(behavior) == autocomplete);
				REQUIRE(tokens.size() == 3);
				REQUIRE(tokens[0].text == "1");
				REQUIRE(tokens[1].text == "2");
				REQUIRE(tokens[1].preceded_by_newline);
				REQUIRE(tokens[1].preceded_by_block_comment);
				REQUIRE(tokens.back().type == terminator);
			}
			for (auto &ending : {"", "\n", "\r"}) {
				auto sql = "--" + padding + ending;
				vector<MatcherToken> tokens;
				TerminatorTokenizerBehavior behavior(sql, tokens, autocomplete);
				bool clean = ending[0] != '\0';
				REQUIRE(tokenizer.TokenizeInput(behavior) == (clean && autocomplete));
				REQUIRE(tokens.size() == 1);
				REQUIRE(tokens.back().type == (clean ? terminator : TokenType::END_OF_INPUT));
			}
			for (auto &sql : {"/*" + padding, "$tag$" + padding + "$ta"}) {
				vector<MatcherToken> tokens;
				TerminatorTokenizerBehavior behavior(sql, tokens, autocomplete);
				REQUIRE_FALSE(tokenizer.TokenizeInput(behavior));
				REQUIRE(tokens.back().type == TokenType::END_OF_INPUT);
			}
			{
				auto sql = "'" + padding;
				vector<MatcherToken> tokens;
				TerminatorTokenizerBehavior behavior(sql, tokens, autocomplete);
				REQUIRE(tokenizer.TokenizeInput(behavior) == autocomplete);
				REQUIRE(tokens.size() == 2);
				REQUIRE(tokens[0].text == sql);
				REQUIRE(tokens[0].unterminated);
				REQUIRE(tokens.back().type == terminator);
			}
		}
	}
}
