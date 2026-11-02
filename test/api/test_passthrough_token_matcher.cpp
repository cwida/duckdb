#include "catch.hpp"

#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/passthrough_dialect.hpp"
#include "duckdb/parser/peg/tokenizer/parser_tokenizer.hpp"
#include "duckdb/parser/token_iterator.hpp"

using namespace duckdb;

namespace {

struct MatchOutcome {
	bool success;
	idx_t consumed;
	string text;
};

//! Match the matcher once against the token at `position` of the tokenized query
MatchOutcome MatchTokenAt(const string &query, idx_t position) {
	auto compiled = CompiledGrammar::Create();
	vector<MatcherToken> tokens;
	ParserTokenizerBehavior behavior(query, tokens);
	compiled->GetTokenizer().TokenizeInput(behavior);

	TokenIterator iterator(tokens);
	iterator.SetPosition(position);

	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	ArenaAllocator packrat_allocator(Allocator::DefaultAllocator());
	ParserPackratCache packrat(packrat_allocator);
	idx_t max_position = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, parse_results, process_allocator, max_position, MatchMode::BUILD_PARSE_RESULT,
	                     IdentifierCaseMode::PRESERVE_CASE, &packrat);
	MatchState state(iterator, context);

	PassthroughTokenMatcher matcher;
	auto result = matcher.MatchParseResult(state);
	MatchOutcome outcome;
	outcome.success = result.IsSuccess();
	outcome.consumed = state.token_iterator.Position() - position;
	if (result.HasParseResult()) {
		outcome.text = result.GetParseResult()->Cast<TokenParseResult>().text;
	}
	return outcome;
}

//! Consume tokens until the matcher fails, returning how many it took
idx_t MatchTokensUntilFailure(const string &query) {
	idx_t position = 0;
	while (MatchTokenAt(query, position).success) {
		position++;
	}
	return position;
}

} // namespace

TEST_CASE("PassthroughTokenMatcher consumes a token of any type", "[api][passthrough_token]") {
	// a keyword, an identifier, an operator, a string literal and a number all match
	for (auto &query : vector<string> {"CREATE", "my_table", "(", "'a string'", "42"}) {
		auto outcome = MatchTokenAt(query, 0);
		REQUIRE(outcome.success);
		REQUIRE(outcome.consumed == 1);
	}
}

TEST_CASE("PassthroughTokenMatcher stops at a statement boundary", "[api][passthrough_token]") {
	// GRANT is not DuckDB syntax - the point is that it is consumed anyway, up to the ';'
	REQUIRE(MatchTokensUntilFailure("GRANT ALL PRIVILEGES ON DATABASE pg TO bob; SELECT 42") == 8);

	// the terminator itself never matches, nor does the end of input
	REQUIRE(!MatchTokenAt(";", 0).success);
	REQUIRE(!MatchTokenAt("", 0).success);
}

TEST_CASE("PassthroughTokenMatcher does not split quoted text", "[api][passthrough_token]") {
	// a ';' inside a string, a quoted identifier or a dollar-quoted body is not a boundary
	REQUIRE(MatchTokensUntilFailure("SELECT 'a;b'") == 2);
	REQUIRE(MatchTokensUntilFailure("SELECT \"a;b\"") == 2);
	REQUIRE(MatchTokensUntilFailure("CREATE FUNCTION f() AS $$ SELECT 1; $$") == 7);

	auto outcome = MatchTokenAt("SELECT 'a;b'", 1);
	REQUIRE(outcome.success);
	REQUIRE(outcome.text == "'a;b'");
}
