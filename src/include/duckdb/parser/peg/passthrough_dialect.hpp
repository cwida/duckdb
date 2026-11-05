//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/passthrough_dialect.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

//! Consumes any single token that is not a statement boundary, without interpreting it. Repeating this matcher
//! covers a statement whose syntax this parser does not know, e.g. SQL destined for a remote engine.
class PassthroughTokenMatcher : public AtomicMatcher {
public:
	static constexpr MatcherType TYPE = MatcherType::CUSTOM;

public:
	PassthroughTokenMatcher() : AtomicMatcher(TYPE) {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		auto current = state.token_iterator.Current();
		if (!current || current->type == TokenType::TERMINATOR || current->type == TokenType::END_OF_INPUT ||
		    current->type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			return MatcherResult::Failure();
		}
		// always advances, so a repeat over this matcher terminates
		auto text = current->text;
		auto offset = current->offset;
		auto length = current->length;
		state.token_iterator.Advance();
		state.UpdateMaxTokenIndex();
		return state.AllocateParseResult<TokenParseResult>(std::move(text), offset, length);
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		// any token matches - there is nothing to suggest
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "PassthroughToken";
	}
};

//! The grammar used while a client is CONNECT-ed to a database that parses nothing locally:
//!
//!   Statement            <- DisconnectStatement / PassthroughStatement
//!   PassthroughStatement <- PassthroughToken+
//!
//! DISCONNECT stays interpreted so the client can always end the connection; every other statement is handed to
//! the remote verbatim. The tokenizer is unchanged, so statement boundaries are DuckDB's.
class PassthroughDialect : public DialectExtension {
public:
	static constexpr const char *NAME = "passthrough";

public:
	PassthroughDialect() : DialectExtension(NAME) {
	}

	void ApplyGrammarChanges(GrammarChangesInput &input) override;
};

} // namespace duckdb
