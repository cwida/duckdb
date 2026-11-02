#pragma once

#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/keyword_helper/default_keyword_maps.hpp"

namespace duckdb {

class DuckDBKeywordHelper : public PEGKeywordHelper {
private:
	DuckDBKeywordHelper();

public:
	static const DuckDBKeywordHelper &Instance();

public:
	keyword_categories_t GetIdentifierMask(SuggestionState type) const override;
	KeywordCategory GetKeywordCategory(const string &text) const;
	vector<ParserKeyword> KeywordList() const override;
	const GrammarLiteralTable &GetLiteralTable() const override {
		return literal_table;
	}

private:
	static DefaultKeywordMaps InitializeKeywordMaps();

private:
	DefaultKeywordMaps keyword_maps;
	GrammarLiteralTable literal_table;
};

} // namespace duckdb
