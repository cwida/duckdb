#pragma once

#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

class StringLiteralMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::STRING_LITERAL;

public:
	explicit StringLiteralMatcher() : AtomicMatcher(TYPE) {
		name = "StringLiteral";
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto token = state.token_iterator.Current();
		if (!token) {
			return MatcherResult::Failure();
		}

		auto start_offset = optional_idx(token->offset);
		auto string_info = GetSpecialStringInfo(token->text);
		const bool allows_continuation = IsSingleQuotedStringLiteral(*token, string_info);

		if (!MatchStringLiteral(state, string_info)) {
			return MatcherResult::Failure();
		}

		string stripped_string;
		idx_t end_offset = token->offset + token->length;
		if (state.BuildParseResult()) {
			stripped_string = StripStringLiteral(*token, string_info);
		}
		if (allows_continuation) {
			while (IsStringLiteralContinuation(state)) {
				token = state.token_iterator.Current();
				if (state.BuildParseResult()) {
					auto continuation_info = GetSpecialStringInfo(token->text);
					stripped_string += StripStringLiteral(*token, continuation_info);
				}
				end_offset = token->offset + token->length;
				Advance(state);
			}
		}
		state.token_iterator.SetPreviousTokenType(TokenType::STRING_LITERAL);
		if (!state.BuildParseResult()) {
			return MatcherResult::Success();
		}

		auto token_length = optional_idx(end_offset - start_offset.GetIndex());

		auto result = state.AllocateParseResult<StringLiteralParseResult>(stripped_string, string_info.type,
		                                                                  start_offset, token_length);
		if (result.HasParseResult()) {
			result.GetParseResult()->SetNameFrom(*this);
		}
		return result;
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "STRING_LITERAL";
	}

	void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const override {
		first_set.class_mask = MatcherTokenClass::STRING;
	}

private:
	static bool IsStringLiteral(const MatcherToken &token, const SpecialStringInfo &string_info) {
		idx_t dollar_quote_delimiter_length;
		return IsSingleQuotedStringLiteral(token, string_info) ||
		       TryGetDollarQuoteDelimiterLength(token, dollar_quote_delimiter_length);
	}

	static bool IsSingleQuotedStringLiteral(const MatcherToken &token, const SpecialStringInfo &string_info) {
		if (string_info.prefix_len == 0) {
			return false;
		}
		const idx_t minimum_literal_length = string_info.prefix_len + 1;
		if (token.text.size() < minimum_literal_length) {
			return false;
		}
		const idx_t opening_quote_position = string_info.prefix_len - 1;
		if (token.text[opening_quote_position] != '\'') {
			return false;
		}
		return token.text.back() == '\'';
	}

	static bool TryGetDollarQuoteDelimiterLength(const MatcherToken &token, idx_t &delimiter_length) {
		if (token.unterminated || token.text.empty() || token.text.front() != '$') {
			return false;
		}
		auto delimiter_end = token.text.find('$', 1);
		if (delimiter_end == string::npos) {
			return false;
		}
		delimiter_length = delimiter_end + 1;
		const idx_t minimum_literal_length = delimiter_length * 2;
		if (token.text.size() < minimum_literal_length) {
			return false;
		}
		const auto delimiter = token.text.substr(0, delimiter_length);
		return StringUtil::EndsWith(token.text, delimiter);
	}

	static string StripStringLiteral(const MatcherToken &token, const SpecialStringInfo &string_info) {
		idx_t delimiter_length;
		if (TryGetDollarQuoteDelimiterLength(token, delimiter_length)) {
			return token.text.substr(delimiter_length, token.text.length() - 2 * delimiter_length);
		}
		idx_t suffix_len = 1;
		auto stripped_string =
		    token.text.substr(string_info.prefix_len, token.text.length() - (string_info.prefix_len + suffix_len));
		return StringUtil::Replace(stripped_string, "''", "'");
	}

	static void Advance(MatchState &state) {
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
	}

	static bool IsStringLiteralContinuation(MatchState &state) {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		if (!token->preceded_by_newline) {
			return false;
		}
		if (token->preceded_by_block_comment) {
			return false;
		}
		if (token->type != TokenType::STRING_LITERAL) {
			return false;
		}
		auto string_info = GetSpecialStringInfo(token->text);
		if (string_info.type != SpecialStringCharacter::STANDARD) {
			return false;
		}
		return IsSingleQuotedStringLiteral(*token, string_info);
	}

	static bool MatchStringLiteral(MatchState &state, const SpecialStringInfo &string_info) {
		auto token = state.token_iterator.Current();
		if (!token) {
			return false;
		}
		if (IsStringLiteral(*token, string_info)) {
			Advance(state);
			return true;
		}
		return false;
	}
};

} // namespace duckdb
