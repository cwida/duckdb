#pragma once
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"

namespace duckdb {

class ParserTokenizerBehavior : public TokenizerBehavior {
public:
	explicit ParserTokenizerBehavior(const string &sql);
	~ParserTokenizerBehavior() override = default;

	void PushToken(idx_t start, idx_t end, TokenType type, bool unterminated = false) override;
	void OnStatementEnd(idx_t pos) override;
	void OnLastToken(const Tokenizer &tokenizer, TokenizeState state, string last_word, idx_t last_pos) override;

	//! A zero-length delimited identifier ("" or ``) - a fatal lexical error at parse time
	static bool IsEmptyQuotedIdentifier(const string &sql, idx_t start, idx_t end, TokenType type);
};

} // namespace duckdb
