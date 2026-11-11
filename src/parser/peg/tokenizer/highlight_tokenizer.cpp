#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"

namespace duckdb {

HighlightTokenizerBehavior::HighlightTokenizerBehavior(const string &sql) : TokenizerBehavior(sql) {
}

void HighlightTokenizerBehavior::PushToken(idx_t start, idx_t end, TokenType type, bool unterminated) {
	if (start >= end) {
		return;
	}
	string last_token = sql.substr(start, end - start);
	tokens.emplace_back(std::move(last_token), start, type, unterminated);
}

void HighlightTokenizerBehavior::OnStatementEnd(idx_t pos) {
	tokens.emplace_back(";", pos, TokenType::TERMINATOR);
}
} // namespace duckdb
