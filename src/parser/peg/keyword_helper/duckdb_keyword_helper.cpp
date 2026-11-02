#include "duckdb/parser/peg/keyword_helper/duckdb_keyword_helper.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"

namespace duckdb {

DuckDBKeywordHelper::DuckDBKeywordHelper()
    : keyword_maps(InitializeKeywordMaps()),
      literal_table(ParsedGrammar::CreateDefault(), keyword_maps.ToLiteralMap()) {
}

const DuckDBKeywordHelper &DuckDBKeywordHelper::Instance() {
	static DuckDBKeywordHelper instance;
	return instance;
}

keyword_categories_t DuckDBKeywordHelper::GetIdentifierMask(SuggestionState type) const {
	return DefaultKeywordMaps::GetIdentifierMask(type);
}

KeywordCategory DuckDBKeywordHelper::GetKeywordCategory(const string &text) const {
	return DefaultKeywordMaps::GetKeywordCategory(LookupKeyword(text));
}

vector<ParserKeyword> DuckDBKeywordHelper::KeywordList() const {
	return keyword_maps.ToList();
}

} // namespace duckdb
