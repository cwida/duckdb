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
	bool HasKeyword(const string &keyword) const {
		return std::find(keywords.begin(), keywords.end(), keyword) != keywords.end();
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
               idx_t &furthest, optional_ptr<vector<MatcherSuggestion>> suggestions = nullptr,
               bool expand_labels = false) {
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
	match_context.expand_labels = expand_labels;
	TokenIterator token_iterator(statement_tokens);
	MatchState state(token_iterator, match_context);
	return grammar.TopLevelStatementMatcher().MatchParseResult(state).IsSuccess();
}

enum class EditKind { INSERT, REPLACE, DELETE };

//! Copy the statement's tokens with one token inserted before, replaced at, or deleted from `index`.
vector<MatcherToken> EditedStatement(const TokenIterator &tokens, idx_t statement_start, idx_t index, EditKind kind,
                                     const string &text = string()) {
	vector<MatcherToken> result;
	result.reserve(tokens.Size() - statement_start + 1);
	for (idx_t i = statement_start; i < tokens.Size(); i++) {
		if (i == index) {
			if (kind != EditKind::DELETE) {
				auto type = IsWordKeyword(text) ? TokenType::KEYWORD : TokenType::OPERATOR;
				result.emplace_back(text, tokens.GetToken(i).offset, type);
			}
			if (kind != EditKind::INSERT) {
				continue;
			}
		}
		result.push_back(tokens.GetToken(i));
	}
	return result;
}

//! Whether the statement parses after the edit.
bool EditRepairs(const TokenIterator &tokens, idx_t statement_start, idx_t index, EditKind kind,
                 const CompiledGrammar &grammar, const ParserOptions &options, const string &text = string()) {
	auto repaired = EditedStatement(tokens, statement_start, index, kind, text);
	idx_t furthest;
	return Recognize(repaired, grammar, options, furthest);
}

//! Re-run the grammar on the statement prefix with an autocomplete cursor at `position` to collect what fits there.
//! With `expand_labels` the keywords inside labelled rules are collected as well, for correction candidates.
ExpectedTokens ComputeExpectedTokens(const TokenIterator &tokens, idx_t statement_start, idx_t position,
                                     const CompiledGrammar &grammar, const ParserOptions &options,
                                     bool expand_labels = false) {
	vector<MatcherToken> prefix;
	prefix.reserve(position - statement_start + 1);
	for (idx_t i = statement_start; i < position; i++) {
		prefix.push_back(tokens.GetToken(i));
	}
	prefix.emplace_back("", tokens.GetToken(position).offset, TokenType::END_OF_INPUT_AUTOCOMPLETE);

	vector<MatcherSuggestion> suggestions;
	idx_t furthest;
	Recognize(prefix, grammar, options, furthest, &suggestions, expand_labels);

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
			if (suggestion.type != SuggestionState::SUGGEST_KEYWORD) {
				continue;
			}
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
	auto repaired = EditedStatement(tokens, statement_start, index, EditKind::REPLACE, replacement);
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
	vector<string> candidates;
	if (expected.labels.empty()) {
		candidates = SimilarKeywords(tokens.GetToken(index).text, expected, grammar.GetKeywordHelper());
	} else {
		// keywords that start a labelled rule (CASE, CAST, ...) are hidden behind the label - collect them too
		auto expanded = ComputeExpectedTokens(tokens, statement_start, index, grammar, options, true);
		candidates = SimilarKeywords(tokens.GetToken(index).text, expanded, grammar.GetKeywordHelper());
	}
	for (idx_t i = 0; i < MinValue<idx_t>(candidates.size(), MAX_CANDIDATES); i++) {
		if (SubstitutionAdvances(tokens, statement_start, index, candidates[i], error_index, grammar, options)) {
			return "Did you mean \"" + candidates[i] + "\"?";
		}
	}
	return string();
}

//! A token whose insertion repairs the statement, and the index it belongs before.
struct MissingToken {
	string text;
	idx_t index = 0;
};

//! The missing token at the failure point, if one can be named with confidence.
//! `previous_expected` describes the position one token earlier, where a comma may also be missing.
optional_idx FindMissingToken(const TokenIterator &tokens, idx_t statement_start, idx_t error_index,
                              const ExpectedTokens &expected, const ExpectedTokens &previous_expected,
                              const CompiledGrammar &grammar, const ParserOptions &options, MissingToken &result) {
	// each candidate costs one parse of the statement
	static constexpr idx_t MAX_REPAIR_CANDIDATES = 32;
	// A forgotten comma or closing bracket is by far the most common cause, and the grammar usually also accepts
	// an operator keyword there (AND, OR, LIKE) that is rarely what was meant - so punctuation is tried first.
	const char *punctuations[] = {",", ")", "]", "}"};
	for (auto punctuation : punctuations) {
		if (expected.AcceptsIdentifiers() && punctuation != punctuations[0]) {
			// where an identifier would also fit, only a comma is a safe guess
			break;
		}
		if (expected.HasKeyword(punctuation) &&
		    EditRepairs(tokens, statement_start, error_index, EditKind::INSERT, grammar, options, punctuation)) {
			result = {punctuation, error_index};
			return result.index;
		}
	}
	// a comma may be missing one token earlier, when the previous word was taken as an alias
	if (error_index > statement_start) {
		auto &previous = tokens.GetToken(error_index - 1);
		if (IsWordToken(previous) && !grammar.GetKeywordHelper().IsKeyword(previous.text) &&
		    previous_expected.HasKeyword(",") &&
		    EditRepairs(tokens, statement_start, error_index - 1, EditKind::INSERT, grammar, options, ",")) {
			result = {",", error_index - 1};
			return result.index;
		}
	}
	if (expected.AcceptsIdentifiers()) {
		// a missing identifier cannot be guessed, and a keyword that happens to parse (LIMIT ALL) is rarely meant
		return optional_idx();
	}
	vector<string> candidates;
	for (auto &keyword : expected.keywords) {
		if (IsWordKeyword(keyword)) {
			candidates.push_back(keyword);
		}
	}
	if (candidates.empty() || candidates.size() > MAX_REPAIR_CANDIDATES) {
		return optional_idx();
	}
	vector<string> repairs;
	for (auto &candidate : candidates) {
		if (repairs.size() > 1) {
			// only a unique keyword repair is reported
			break;
		}
		if (EditRepairs(tokens, statement_start, error_index, EditKind::INSERT, grammar, options, candidate)) {
			repairs.push_back(candidate);
		}
	}
	if (repairs.size() != 1) {
		return optional_idx();
	}
	result = {repairs[0], error_index};
	return result.index;
}

//! Keywords and punctuation can be stray; literals and identifiers are content, a terminator is where input ends.
//! Opening brackets are excluded: nesting repeats them legitimately, and a missing closer is reported instead.
bool IsDroppable(const MatcherToken &token, const PEGKeywordHelper &keyword_helper) {
	if (token.type == TokenType::OPERATOR) {
		return token.text != "(" && token.text != "[" && token.text != "{";
	}
	return IsWordToken(token) && keyword_helper.IsKeyword(token.text);
}

bool RepeatsPredecessor(const TokenIterator &tokens, idx_t statement_start, idx_t index) {
	return index > statement_start &&
	       StringUtil::CIEquals(tokens.GetToken(index - 1).text, tokens.GetToken(index).text);
}

//! A plain word followed by "(", as in "COUNT COUNT(*)": a doubled function name rather than a column and an alias.
bool IsRepeatedFunctionName(const TokenIterator &tokens, idx_t index) {
	if (!IsWordToken(tokens.GetToken(index))) {
		return false;
	}
	if (index + 1 >= tokens.Size()) {
		return false;
	}
	return tokens.GetToken(index + 1).text == "(";
}

//! Whether the token at `index` repeats its predecessor ("WHERE WHERE", "AS AS", ",,") and dropping it repairs.
bool IsRepeatedToken(const TokenIterator &tokens, idx_t statement_start, idx_t index, const CompiledGrammar &grammar,
                     const ParserOptions &options) {
	if (!RepeatsPredecessor(tokens, statement_start, index)) {
		return false;
	}
	if (!IsDroppable(tokens.GetToken(index), grammar.GetKeywordHelper()) && !IsRepeatedFunctionName(tokens, index)) {
		return false;
	}
	return EditRepairs(tokens, statement_start, index, EditKind::DELETE, grammar, options);
}

//! A repeated token whose removal makes the statement parse: the failing token, or the token right before it when
//! the repeat was still consumed. Repeats are a safer guess than any insertion.
optional_idx FindRepeatedToken(const TokenIterator &tokens, idx_t statement_start, idx_t error_index,
                               const CompiledGrammar &grammar, const ParserOptions &options) {
	if (IsRepeatedToken(tokens, statement_start, error_index, grammar, options)) {
		return error_index;
	}
	if (error_index == statement_start) {
		return optional_idx();
	}
	if (IsRepeatedToken(tokens, statement_start, error_index - 1, grammar, options)) {
		return error_index - 1;
	}
	return optional_idx();
}

//! Whether the failing token is stray where only literal tokens fit, and its removal makes the statement parse.
//! Where an identifier or expression fits, the failure usually means something is missing, not that a token is extra.
bool IsStrayToken(const TokenIterator &tokens, idx_t statement_start, idx_t error_index, const ExpectedTokens &expected,
                  const CompiledGrammar &grammar, const ParserOptions &options) {
	if (expected.AcceptsIdentifiers()) {
		return false;
	}
	if (!IsDroppable(tokens.GetToken(error_index), grammar.GetKeywordHelper())) {
		return false;
	}
	return EditRepairs(tokens, statement_start, error_index, EditKind::DELETE, grammar, options);
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

//! A hint for the syntax error and the token it is about.
struct SyntaxHint {
	string text;
	idx_t index;
};

SyntaxHint UnexpectedHint(const TokenIterator &tokens, idx_t index) {
	return {"Unexpected \"" + tokens.GetToken(index).text + "\"", index};
}

//! The most specific hint for the failure at `error_index`, tried from the most to the least confident kind.
SyntaxHint FindHint(const TokenIterator &tokens, idx_t statement_start, idx_t error_index,
                    const CompiledGrammar &grammar, const ParserOptions &options) {
	auto expected = ComputeExpectedTokens(tokens, statement_start, error_index, grammar, options);
	auto &error_token = tokens.GetToken(error_index);
	const bool at_end = IsEndOfInput(error_token);
	if (!at_end && IsWordToken(error_token)) {
		auto correction =
		    KeywordCorrectionHint(tokens, statement_start, error_index, error_index, expected, grammar, options);
		if (!correction.empty()) {
			return {correction, error_index};
		}
	}

	// a misspelled keyword or a missing comma often hides one token earlier, in a word taken as an alias
	const bool previous_is_word = error_index > statement_start && IsWordToken(tokens.GetToken(error_index - 1)) &&
	                              !grammar.GetKeywordHelper().IsKeyword(tokens.GetToken(error_index - 1).text);
	ExpectedTokens previous_expected;
	if (previous_is_word) {
		previous_expected = ComputeExpectedTokens(tokens, statement_start, error_index - 1, grammar, options);
		auto correction = KeywordCorrectionHint(tokens, statement_start, error_index - 1, error_index,
		                                        previous_expected, grammar, options);
		if (!correction.empty()) {
			return {correction, error_index - 1};
		}
	}

	if (!at_end) {
		auto repeated = FindRepeatedToken(tokens, statement_start, error_index, grammar, options);
		if (repeated.IsValid()) {
			return UnexpectedHint(tokens, repeated.GetIndex());
		}
	}

	MissingToken missing;
	if (FindMissingToken(tokens, statement_start, error_index, expected, previous_expected, grammar, options, missing)
	        .IsValid()) {
		return {"Missing \"" + missing.text + "\"", missing.index};
	}

	if (!at_end && IsStrayToken(tokens, statement_start, error_index, expected, grammar, options)) {
		return UnexpectedHint(tokens, error_index);
	}

	return {DescribeExpected(expected, grammar.GetKeywordHelper()), error_index};
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

	SyntaxHint hint {string(), error_index};
	if (compute_hints) {
		hint = FindHint(tokens, statement_start, error_index, grammar, options);
	}
	idx_t report_index = error_index;
	if (hint.index != error_index) {
		// the hint is about an earlier token, so the error is reported there and not at the end of the input
		report_index = hint.index;
		at_end = false;
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
	if (!hint.text.empty()) {
		message += "\n" + hint.text;
	}
	auto &location_token = tokens.GetToken(report_index);
	return ParserException::SyntaxError(string(), message, QueryLocation(location_token.offset, location_token.length));
}

} // namespace duckdb
