//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/keyword_categories.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"

namespace duckdb {

//! Opaque, dialect-defined keyword category flags.
struct keyword_categories_t { // NOLINT
private:
	using UNDERLYING_TYPE = uint8_t;

public:
	//! Category IDs are zero-based; the empty mask has no bits set.
	static constexpr idx_t MAX_CATEGORY_ID = sizeof(UNDERLYING_TYPE) * 8;

	constexpr keyword_categories_t() : value(0) {
	}
	explicit constexpr keyword_categories_t(UNDERLYING_TYPE value) : value(value) {
	}

	static constexpr keyword_categories_t CreateCategory(idx_t category_id) {
		if (category_id >= MAX_CATEGORY_ID) {
			throw InvalidInputException("Keyword category ID must be between 0 and %llu", MAX_CATEGORY_ID - 1);
		}
		return keyword_categories_t(static_cast<UNDERLYING_TYPE>(UNDERLYING_TYPE(1) << category_id));
	}

	explicit constexpr operator UNDERLYING_TYPE() const {
		return value;
	}

	constexpr bool operator==(keyword_categories_t other) const {
		return value == other.value;
	}
	constexpr bool operator!=(keyword_categories_t other) const {
		return value != other.value;
	}
	constexpr keyword_categories_t operator|(keyword_categories_t other) const {
		return keyword_categories_t(static_cast<UNDERLYING_TYPE>(value | other.value));
	}
	keyword_categories_t &operator|=(keyword_categories_t other) {
		value |= other.value;
		return *this;
	}
	constexpr keyword_categories_t operator&(keyword_categories_t other) const {
		return keyword_categories_t(static_cast<UNDERLYING_TYPE>(value & other.value));
	}
	constexpr keyword_categories_t operator~() const {
		return keyword_categories_t(static_cast<UNDERLYING_TYPE>(~value));
	}

private:
	UNDERLYING_TYPE value;
};

} // namespace duckdb
