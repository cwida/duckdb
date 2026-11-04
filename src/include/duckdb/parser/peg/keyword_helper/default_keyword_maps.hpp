#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/peg/literal_info.hpp"
#include "duckdb/parser/simplified_token.hpp"

namespace duckdb {

enum class SuggestionState : uint8_t;

class DefaultKeywordMaps {
public:
	DUCKDB_API LiteralInfo LookupKeyword(const string &text, uint16_t literal_id = 0) const;
	DUCKDB_API static keyword_categories_t GetIdentifierMask(SuggestionState type);
	DUCKDB_API static KeywordCategory GetKeywordCategory(LiteralInfo info);
	//! All recognized categories, in reserved, unreserved, type-function, column-name, type-name order.
	DUCKDB_API static vector<KeywordCategory> GetKeywordCategories(LiteralInfo info);
	DUCKDB_API case_insensitive_map_t<LiteralInfo> ToLiteralMap() const;
	vector<ParserKeyword> ToList() const;

public:
	case_insensitive_set_t reserved_keyword_map;
	case_insensitive_set_t unreserved_keyword_map;
	case_insensitive_set_t colname_keyword_map;
	case_insensitive_set_t typefunc_keyword_map;
	case_insensitive_set_t typename_keyword_map;
	//! Keywords that can be used as a column alias without AS (orthogonal to the other categories)
	case_insensitive_set_t bare_label_keyword_map;
};

} // namespace duckdb
