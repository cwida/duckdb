#include "duckdb/parser/peg/syntax_error.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/token_iterator.hpp"

#include <algorithm>
#include <utility>

namespace duckdb {

namespace {

//! What the grammar accepts at a token position, gathered through the autocomplete suggestion walk.
struct ExpectedTokens {
	//! Literal keywords and punctuation, as spelled in the grammar.
	vector<string> keywords;
	//! Human-readable descriptions of non-literal alternatives (e.g. "a table name").
	vector<string> categories;
	//! Labels of whole sub-grammars that were accepted (e.g. "an expression").
	vector<string> labels;
	//! Keyword categories the identifier positions here accept, as IdentifierMatcher sees them.
	keyword_categories_t identifier_mask;

	bool AcceptsIdentifiers() const {
		return !categories.empty() || !labels.empty();
	}
};

bool IsEndOfInput(const MatcherToken &token) {
	return token.type == TokenType::END_OF_INPUT || token.type == TokenType::END_OF_INPUT_AUTOCOMPLETE;
}

//! An unquoted word token, i.e. a keyword or a plain identifier - the only tokens that can be misspelled keywords.
bool IsWordToken(const MatcherToken &token) {
	switch (token.type) {
	case TokenType::STRING_LITERAL:
	case TokenType::NUMBER_LITERAL:
	case TokenType::OPERATOR:
	case TokenType::COMMENT:
	case TokenType::TERMINATOR:
	case TokenType::TOKEN_ERROR:
	case TokenType::END_OF_INPUT:
	case TokenType::END_OF_INPUT_AUTOCOMPLETE:
		return false;
	default:
		break;
	}
	if (token.text.empty()) {
		return false;
	}
	return StringUtil::CharacterIsAlpha(token.text[0]) || token.text[0] == '_';
}

bool IsWordKeyword(const string &keyword) {
	if (keyword.empty()) {
		return false;
	}
	for (auto c : keyword) {
		if (!StringUtil::CharacterIsAlpha(c) && c != '_') {
			return false;
		}
	}
	return true;
}

const char *CategoryDescription(SuggestionState type) {
	switch (type) {
	case SuggestionState::SUGGEST_CATALOG_NAME:
		return "a catalog name";
	case SuggestionState::SUGGEST_SCHEMA_NAME:
		return "a schema name";
	case SuggestionState::SUGGEST_TABLE_NAME:
		return "a table name";
	case SuggestionState::SUGGEST_TYPE_NAME:
		return "a type name";
	case SuggestionState::SUGGEST_COLUMN_NAME:
		return "a column name";
	case SuggestionState::SUGGEST_FILE_NAME:
		return "a file name";
	case SuggestionState::SUGGEST_DIRECTORY:
		return "a directory";
	case SuggestionState::SUGGEST_VARIABLE:
	case SuggestionState::SUGGEST_BARE_LABEL:
		return "an identifier";
	case SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME:
		return "a function name";
	case SuggestionState::SUGGEST_TABLE_FUNCTION_NAME:
		return "a table function name";
	case SuggestionState::SUGGEST_PRAGMA_NAME:
		return "a pragma name";
	case SuggestionState::SUGGEST_SETTING_NAME:
		return "a setting name";
	default:
		return nullptr;
	}
}

//! Run the grammar over `statement_tokens`; `furthest` receives the highest token index reached.
//! With `suggestions`, the autocomplete cursor at the end of the tokens collects them.
//! The parse tree is built and discarded: RECOGNIZE_ONLY mode backtracks several times more on nested input.
bool Recognize(vector<MatcherToken> &statement_tokens, const CompiledGrammar &grammar, const ParserOptions &options,
               idx_t &furthest, optional_ptr<vector<MatcherSuggestion>> suggestions = nullptr) {
	vector<MatcherSuggestion> unused_suggestions;
	ParseResultAllocator parse_result_allocator;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	ArenaAllocator packrat_allocator(Allocator::DefaultAllocator());
	ParserPackratCache packrat_cache(packrat_allocator);
	furthest = 0;
	MatchContext match_context(suggestions ? *suggestions : unused_suggestions, parse_result_allocator,
	                           process_allocator, furthest, MatchMode::BUILD_PARSE_RESULT, options.identifier_case_mode,
	                           &packrat_cache);
	match_context.memoizable_suggestions = suggestions != nullptr;
	TokenIterator token_iterator(statement_tokens);
	MatchState state(token_iterator, match_context);
	return grammar.TopLevelStatementMatcher().MatchParseResult(state).IsSuccess();
}

//! Copy the statement's tokens, replacing or inserting one token at `index`.
vector<MatcherToken> EditedStatement(const TokenIterator &tokens, idx_t statement_start, idx_t index,
                                     const string &text, bool replace) {
	vector<MatcherToken> result;
	result.reserve(tokens.Size() - statement_start + 1);
	for (idx_t i = statement_start; i < tokens.Size(); i++) {
		if (i == index) {
			auto type = IsWordKeyword(text) ? TokenType::KEYWORD : TokenType::OPERATOR;
			result.emplace_back(text, tokens.GetToken(i).offset, type);
			if (replace) {
				continue;
			}
		}
		result.push_back(tokens.GetToken(i));
	}
	return result;
}

//! Re-run the grammar on the statement prefix with an autocomplete cursor at `position` to collect what fits there.
ExpectedTokens ComputeExpectedTokens(const TokenIterator &tokens, idx_t statement_start, idx_t position,
                                     const CompiledGrammar &grammar, const ParserOptions &options) {
	vector<MatcherToken> prefix;
	prefix.reserve(position - statement_start + 1);
	for (idx_t i = statement_start; i < position; i++) {
		prefix.push_back(tokens.GetToken(i));
	}
	prefix.emplace_back("", tokens.GetToken(position).offset, TokenType::END_OF_INPUT_AUTOCOMPLETE);

	vector<MatcherSuggestion> suggestions;
	idx_t furthest;
	Recognize(prefix, grammar, options, furthest, &suggestions);

	ExpectedTokens result;
	case_insensitive_set_t seen_keywords;
	auto add_unique = [](vector<string> &list, const string &entry) {
		if (std::find(list.begin(), list.end(), entry) == list.end()) {
			list.push_back(entry);
		}
	};
	auto &keyword_helper = grammar.GetKeywordHelper();
	for (auto &suggestion : suggestions) {
		if (suggestion.type != SuggestionState::SUGGEST_KEYWORD) {
			result.identifier_mask |= keyword_helper.GetIdentifierMask(suggestion.type);
		}
		if (suggestion.label) {
			add_unique(result.labels, *suggestion.label);
			continue;
		}
		if (suggestion.type == SuggestionState::SUGGEST_KEYWORD) {
			auto &keyword = suggestion.keyword.candidate;
			if (keyword.empty() || !seen_keywords.insert(keyword).second) {
				continue;
			}
			result.keywords.push_back(keyword);
			continue;
		}
		auto description = CategoryDescription(suggestion.type);
		if (description) {
			add_unique(result.categories, description);
		}
	}
	std::sort(result.keywords.begin(), result.keywords.end());
	return result;
}

//! A keyword the identifier positions here accept as an identifier is no alternative worth naming.
bool IsRedundantKeyword(const string &keyword, const ExpectedTokens &expected, const PEGKeywordHelper &keyword_helper) {
	return IsWordKeyword(keyword) && keyword_helper.LookupKeyword(keyword).HasAnyFlags(expected.identifier_mask);
}

//! Expected keywords that `token_text` could be a misspelling of, closest first.
vector<string> SimilarKeywords(const string &token_text, const ExpectedTokens &expected,
                               const PEGKeywordHelper &keyword_helper) {
	if (token_text.size() < 2) {
		return {};
	}
	// short words tolerate a single edit, longer ones two
	const idx_t max_distance = token_text.size() <= 4 ? 1 : 2;
	vector<std::pair<idx_t, string>> scored;
	for (auto &keyword : expected.keywords) {
		if (!IsWordKeyword(keyword) || keyword.size() < 2 || IsRedundantKeyword(keyword, expected, keyword_helper)) {
			continue;
		}
		auto distance = StringUtil::DamerauLevenshteinDistance(token_text, keyword);
		if (distance <= max_distance) {
			scored.emplace_back(distance, keyword);
		}
	}
	std::sort(scored.begin(), scored.end());
	vector<string> result;
	for (auto &entry : scored) {
		result.push_back(entry.second);
	}
	return result;
}

//! Does replacing the token at `index` with `replacement` let the parser get past the original failure?
bool SubstitutionAdvances(const TokenIterator &tokens, idx_t statement_start, idx_t index, const string &replacement,
                          idx_t error_index, const CompiledGrammar &grammar, const ParserOptions &options) {
	auto repaired = EditedStatement(tokens, statement_start, index, replacement, true);
	idx_t furthest;
	if (Recognize(repaired, grammar, options, furthest)) {
		return true;
	}
	// the parser must get past the token that originally failed
	return statement_start + furthest > error_index;
}

//! "Did you mean" hint for the token at `index`, verified by re-parsing with the correction in place.
string KeywordCorrectionHint(const TokenIterator &tokens, idx_t statement_start, idx_t index, idx_t error_index,
                             const ExpectedTokens &expected, const CompiledGrammar &grammar,
                             const ParserOptions &options) {
	static constexpr idx_t MAX_CANDIDATES = 3;
	auto candidates = SimilarKeywords(tokens.GetToken(index).text, expected, grammar.GetKeywordHelper());
	for (idx_t i = 0; i < MinValue<idx_t>(candidates.size(), MAX_CANDIDATES); i++) {
		if (SubstitutionAdvances(tokens, statement_start, index, candidates[i], error_index, grammar, options)) {
			return "Did you mean \"" + candidates[i] + "\"?";
		}
	}
	return string();
}

bool IsRepairCandidate(const string &keyword) {
	return IsWordKeyword(keyword) || keyword == "," || keyword == ")" || keyword == "]" || keyword == "}";
}

//! Expected tokens whose insertion at the failure point makes the whole statement parse.
vector<string> FindMissingTokens(const TokenIterator &tokens, idx_t statement_start, idx_t error_index,
                                 const ExpectedTokens &expected, const CompiledGrammar &grammar,
                                 const ParserOptions &options) {
	// each candidate costs one parse of the statement
	static constexpr idx_t MAX_REPAIR_CANDIDATES = 32;
	if (expected.AcceptsIdentifiers()) {
		// a missing identifier cannot be guessed, and a keyword that happens to parse (LIMIT ALL) is rarely meant
		return {};
	}
	vector<string> candidates;
	for (auto &keyword : expected.keywords) {
		if (IsRepairCandidate(keyword)) {
			candidates.push_back(keyword);
		}
	}
	if (candidates.empty() || candidates.size() > MAX_REPAIR_CANDIDATES) {
		return {};
	}
	vector<string> result;
	for (auto &candidate : candidates) {
		if (result.size() > 1) {
			// the caller only acts on a unique repair
			break;
		}
		auto repaired = EditedStatement(tokens, statement_start, error_index, candidate, false);
		idx_t furthest;
		if (Recognize(repaired, grammar, options, furthest)) {
			result.push_back(candidate);
		}
	}
	return result;
}

string DescribeExpected(const ExpectedTokens &expected, const PEGKeywordHelper &keyword_helper) {
	// long lists (e.g. every operator that can follow an expression) tell the user nothing
	static constexpr idx_t MAX_EXPECTED = 8;
	vector<string> items;
	for (auto &keyword : expected.keywords) {
		if (IsRedundantKeyword(keyword, expected, keyword_helper)) {
			continue;
		}
		items.push_back("\"" + keyword + "\"");
	}
	for (auto &label : expected.labels) {
		items.push_back(label);
	}
	for (auto &category : expected.categories) {
		items.push_back(category);
	}
	if (items.size() > MAX_EXPECTED) {
		// too many literal alternatives - only a sub-grammar label summarises the position well enough
		if (expected.labels.empty() || expected.labels.size() + expected.categories.size() > MAX_EXPECTED) {
			return string();
		}
		items = expected.labels;
		items.insert(items.end(), expected.categories.begin(), expected.categories.end());
	}
	if (items.empty()) {
		return string();
	}
	if (items.size() == 1) {
		return "Expected " + items[0];
	}
	return "Expected one of: " + StringUtil::Join(items, ", ");
}

} // namespace

ParserException PEGSyntaxError::Create(const TokenIterator &tokens, idx_t statement_start, idx_t failure_index,
                                       const CompiledGrammar &grammar, const ParserOptions &options) {
	// hints re-parse the statement several times - skip them for huge statements
	static constexpr idx_t MAX_HINT_TOKENS = 5000;
	D_ASSERT(tokens.Size() > 0);
	idx_t error_index = MinValue<idx_t>(failure_index, tokens.Size() - 1);
	error_index = MaxValue(error_index, statement_start);
	auto &error_token = tokens.GetToken(error_index);
	bool at_end = IsEndOfInput(error_token);
	const bool compute_hints = tokens.Size() - statement_start <= MAX_HINT_TOKENS;

	ExpectedTokens expected;
	if (compute_hints) {
		expected = ComputeExpectedTokens(tokens, statement_start, error_index, grammar, options);
	}
	idx_t report_index = error_index;
	string hint;
	if (compute_hints && !at_end && IsWordToken(error_token)) {
		hint = KeywordCorrectionHint(tokens, statement_start, error_index, error_index, expected, grammar, options);
	}
	if (compute_hints && hint.empty() && error_index > statement_start) {
		// a misspelled keyword is often swallowed as an alias, so the failure surfaces one token late
		auto &previous = tokens.GetToken(error_index - 1);
		if (IsWordToken(previous) && !grammar.GetKeywordHelper().IsKeyword(previous.text)) {
			auto previous_expected = ComputeExpectedTokens(tokens, statement_start, error_index - 1, grammar, options);
			hint = KeywordCorrectionHint(tokens, statement_start, error_index - 1, error_index, previous_expected,
			                             grammar, options);
			if (!hint.empty()) {
				report_index = error_index - 1;
				at_end = false;
			}
		}
	}
	if (compute_hints && hint.empty()) {
		auto missing = FindMissingTokens(tokens, statement_start, error_index, expected, grammar, options);
		if (missing.size() == 1) {
			hint = "Missing \"" + missing[0] + "\"";
		}
	}
	if (hint.empty()) {
		hint = DescribeExpected(expected, grammar.GetKeywordHelper());
	}

	string message;
	if (at_end) {
		message = "syntax error at end of input";
		// the end-of-input sentinel has no text to underline
		if (report_index > statement_start) {
			report_index--;
		}
	} else {
		message = "syntax error at or near \"" + tokens.GetToken(report_index).text + "\"";
	}
	if (!hint.empty()) {
		message += "\n" + hint;
	}
	auto &location_token = tokens.GetToken(report_index);
	return ParserException::SyntaxError(string(), message, QueryLocation(location_token.offset, location_token.length));
}

} // namespace duckdb
