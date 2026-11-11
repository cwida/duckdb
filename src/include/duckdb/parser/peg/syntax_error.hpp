//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/syntax_error.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/parser_options.hpp"

namespace duckdb {
class TokenIterator;
struct CompiledGrammar;

//! Builds the ParserException for a statement the PEG matcher rejected.
class PEGSyntaxError {
public:
	//! `failure_index` is the furthest token the matcher reached for the statement at `statement_start`.
	static ParserException Create(const TokenIterator &tokens, idx_t statement_start, idx_t failure_index,
	                              const CompiledGrammar &grammar, const ParserOptions &options);
};

} // namespace duckdb
