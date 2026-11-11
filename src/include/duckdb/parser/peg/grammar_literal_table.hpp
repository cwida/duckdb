//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/grammar_literal_table.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/peg/literal_info.hpp"

namespace duckdb {

class ParsedGrammar;

//! Immutable after construction, including literals only present in keyword-category rules.
class GrammarLiteralTable {
public:
	DUCKDB_API GrammarLiteralTable(const ParsedGrammar &grammar, const case_insensitive_map_t<LiteralInfo> &keywords);
	GrammarLiteralTable(const GrammarLiteralTable &) = delete;
	GrammarLiteralTable &operator=(const GrammarLiteralTable &) = delete;

	uint64_t CacheId() const {
		return cache_id;
	}

	LiteralInfo Lookup(const string &text) const {
		if (text.size() > max_literal_length) {
			return LiteralInfo();
		}
		auto entry = literals.find(text);
		return entry == literals.end() ? LiteralInfo() : entry->second;
	}

private:
	void Register(const string &text, keyword_categories_t categories = keyword_categories_t());

private:
	const uint64_t cache_id;
	idx_t max_literal_length = 0;
	case_insensitive_map_t<LiteralInfo> literals;
};

} // namespace duckdb
