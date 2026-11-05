//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/literal_info.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/peg/keyword_categories.hpp"
#include <cstdint>

namespace duckdb {

//! A grammar-local literal ID and opaque keyword properties.
class LiteralInfo {
public:
	static constexpr uint16_t MAX_LITERAL_ID = 0xFFFF;

public:
	//! Zero denotes an unknown literal with no keyword flags, such as a non-keyword identifier.
	LiteralInfo() : literal_id(0) {
	}
	explicit LiteralInfo(uint16_t literal_id, keyword_categories_t category_flags = keyword_categories_t())
	    : literal_id(literal_id), category_flags(category_flags) {
	}

	//! Zero means no grammar-local ID has been assigned; keyword flags may still be present.
	uint16_t LiteralId() const {
		return literal_id;
	}

	keyword_categories_t CategoryFlags() const {
		return category_flags;
	}

	void AddCategories(keyword_categories_t flags) {
		category_flags |= flags;
	}

	bool IsKeyword() const {
		return category_flags != keyword_categories_t();
	}

	bool HasAnyFlags(keyword_categories_t mask) const {
		return (category_flags & mask) != keyword_categories_t();
	}

	bool operator==(const LiteralInfo &other) const {
		return literal_id == other.literal_id && category_flags == other.category_flags;
	}

private:
	uint16_t literal_id = 0;
	keyword_categories_t category_flags = keyword_categories_t();
};

} // namespace duckdb
