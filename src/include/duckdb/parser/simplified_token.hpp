//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/simplified_token.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Simplified tokens are a simplified (dense) representation of the lexer
//! Used for simple syntax highlighting in the tests
enum class SimplifiedTokenType : uint8_t {
	SIMPLIFIED_TOKEN_IDENTIFIER,
	SIMPLIFIED_TOKEN_NUMERIC_CONSTANT,
	SIMPLIFIED_TOKEN_STRING_CONSTANT,
	SIMPLIFIED_TOKEN_OPERATOR,
	SIMPLIFIED_TOKEN_KEYWORD,
	SIMPLIFIED_TOKEN_COMMENT,
	SIMPLIFIED_TOKEN_ERROR,
	SIMPLIFIED_TOKEN_ERROR_EMPHASIS,
	SIMPLIFIED_TOKEN_ERROR_SUGGESTION
};

struct SimplifiedToken {
	SimplifiedTokenType type;
	idx_t start;
};

enum class KeywordCategory : uint8_t {
	KEYWORD_RESERVED,
	KEYWORD_UNRESERVED,
	KEYWORD_TYPE_FUNC,
	KEYWORD_COL_NAME,
	KEYWORD_NONE,
	KEYWORD_TYPE_NAME
};

struct ParserKeyword {
	string name;
	string category;
};

} // namespace duckdb
