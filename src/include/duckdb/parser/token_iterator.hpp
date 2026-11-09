//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/token_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/peg/matcher_token.hpp"

namespace duckdb {
struct SimpleToken;

//! Iterates over an already-tokenized query. A root iterator can own its tokens; child iterators
//! reference the same tokens and carry an independent position for speculative parsing.
class TokenIterator {
public:
	DUCKDB_API explicit TokenIterator(unique_ptr<vector<MatcherToken>> owned_tokens);
	DUCKDB_API explicit TokenIterator(vector<MatcherToken> &tokens);
	DUCKDB_API TokenIterator(const TokenIterator &other) : tokens(other.tokens), position(other.position) {
	}
	DUCKDB_API TokenIterator(TokenIterator &&other) noexcept;
	TokenIterator &operator=(const TokenIterator &) = delete;
	TokenIterator &operator=(TokenIterator &&) = delete;

	DUCKDB_API bool AtEnd() const {
		auto current = Current();
		return !current || current->type == TokenType::END_OF_INPUT;
	}
	DUCKDB_API bool HasMoreStatements() const;
	DUCKDB_API idx_t Position() const {
		return position;
	}
	DUCKDB_API idx_t Size() const {
		return tokens.size();
	}
	DUCKDB_API idx_t EndOffset() const;

	DUCKDB_API optional_ptr<const MatcherToken> Current() const {
		if (position >= tokens.size()) {
			return nullptr;
		}
		return tokens[position];
	}
	LiteralInfo CurrentLiteralInfo(const GrammarLiteralTable &table) {
		if (position >= tokens.size()) {
			return LiteralInfo();
		}
		return tokens[position].GetLiteralInfo(table);
	}
	DUCKDB_API const MatcherToken &Previous() const;
	DUCKDB_API const MatcherToken &GetToken(idx_t index) const;

	DUCKDB_API void Advance(idx_t count = 1);
	DUCKDB_API void SetPosition(idx_t position);
	DUCKDB_API void SetPosition(const TokenIterator &other);
	DUCKDB_API void SetPreviousTokenType(TokenType type);

	DUCKDB_API vector<SimpleToken> RemainingTokens() const;
	DUCKDB_API string ToString() const;

private:
	unique_ptr<vector<MatcherToken>> owned_tokens;
	vector<MatcherToken> &tokens;
	idx_t position = 0;
};

} // namespace duckdb
