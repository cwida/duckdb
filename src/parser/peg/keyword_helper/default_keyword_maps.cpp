#include "duckdb/parser/peg/keyword_helper/default_keyword_maps.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

static constexpr keyword_categories_t KEYWORD_UNRESERVED = keyword_categories_t::CreateCategory(0);
static constexpr keyword_categories_t KEYWORD_RESERVED = keyword_categories_t::CreateCategory(1);
static constexpr keyword_categories_t KEYWORD_TYPE_FUNC = keyword_categories_t::CreateCategory(2);
static constexpr keyword_categories_t KEYWORD_COL_NAME = keyword_categories_t::CreateCategory(3);
static constexpr keyword_categories_t KEYWORD_TYPE_NAME = keyword_categories_t::CreateCategory(4);
static constexpr keyword_categories_t KEYWORD_BARE_LABEL = keyword_categories_t::CreateCategory(5);

LiteralInfo DefaultKeywordMaps::LookupKeyword(const string &text, uint16_t literal_id) const {
	keyword_categories_t flags;
	if (unreserved_keyword_map.count(text)) {
		flags |= KEYWORD_UNRESERVED;
	}
	if (reserved_keyword_map.count(text)) {
		flags |= KEYWORD_RESERVED;
	}
	if (typefunc_keyword_map.count(text)) {
		flags |= KEYWORD_TYPE_FUNC;
	}
	if (colname_keyword_map.count(text)) {
		flags |= KEYWORD_COL_NAME;
	}
	if (typename_keyword_map.count(text)) {
		flags |= KEYWORD_TYPE_NAME;
	}
	if (bare_label_keyword_map.count(text)) {
		flags |= KEYWORD_BARE_LABEL;
	}
	return LiteralInfo(literal_id, flags);
}

keyword_categories_t DefaultKeywordMaps::GetIdentifierMask(SuggestionState type) {
	switch (type) {
	case SuggestionState::SUGGEST_BARE_LABEL:
		return KEYWORD_BARE_LABEL;
	case SuggestionState::SUGGEST_TYPE_NAME:
		return KEYWORD_UNRESERVED | KEYWORD_TYPE_NAME;
	case SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME:
	case SuggestionState::SUGGEST_TABLE_FUNCTION_NAME:
		return KEYWORD_UNRESERVED | KEYWORD_TYPE_FUNC;
	default:
		return KEYWORD_UNRESERVED | KEYWORD_COL_NAME;
	}
}

KeywordCategory DefaultKeywordMaps::GetKeywordCategory(LiteralInfo info) {
	auto categories = GetKeywordCategories(info);
	if (categories.empty()) {
		return KeywordCategory::KEYWORD_NONE;
	}
	return categories[0];
}

vector<KeywordCategory> DefaultKeywordMaps::GetKeywordCategories(LiteralInfo info) {
	vector<KeywordCategory> result;
	if (info.HasAnyFlags(KEYWORD_RESERVED)) {
		result.push_back(KeywordCategory::KEYWORD_RESERVED);
	}
	if (info.HasAnyFlags(KEYWORD_UNRESERVED)) {
		result.push_back(KeywordCategory::KEYWORD_UNRESERVED);
	}
	if (info.HasAnyFlags(KEYWORD_TYPE_FUNC)) {
		result.push_back(KeywordCategory::KEYWORD_TYPE_FUNC);
	}
	if (info.HasAnyFlags(KEYWORD_COL_NAME)) {
		result.push_back(KeywordCategory::KEYWORD_COL_NAME);
	}
	if (info.HasAnyFlags(KEYWORD_TYPE_NAME)) {
		result.push_back(KeywordCategory::KEYWORD_TYPE_NAME);
	}
	return result;
}

case_insensitive_map_t<LiteralInfo> DefaultKeywordMaps::ToLiteralMap() const {
	case_insensitive_map_t<LiteralInfo> result;
	auto add_keywords = [&](const case_insensitive_set_t &words) {
		for (auto &word : words) {
			result.emplace(word, LookupKeyword(word));
		}
	};
	add_keywords(reserved_keyword_map);
	add_keywords(unreserved_keyword_map);
	add_keywords(colname_keyword_map);
	add_keywords(typefunc_keyword_map);
	add_keywords(typename_keyword_map);
	return result;
}

vector<ParserKeyword> DefaultKeywordMaps::ToList() const {
	vector<ParserKeyword> result;
	for (auto &kw : reserved_keyword_map) {
		result.push_back({kw, "reserved"});
	}
	for (auto &kw : unreserved_keyword_map) {
		result.push_back({kw, "unreserved"});
	}
	for (auto &kw : typefunc_keyword_map) {
		result.push_back({kw, "type_function"});
	}
	for (auto &kw : colname_keyword_map) {
		result.push_back({kw, "column_name"});
	}
	return result;
}

} // namespace duckdb
