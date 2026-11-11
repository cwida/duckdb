#pragma once
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"

namespace duckdb {

class HighlightTokenizerBehavior : public TokenizerBehavior {
public:
	explicit HighlightTokenizerBehavior(const string &sql);
	~HighlightTokenizerBehavior() override = default;

	void PushToken(idx_t start, idx_t end, TokenType type, bool unterminated) override;
	void OnStatementEnd(idx_t pos) override;
};

} // namespace duckdb
