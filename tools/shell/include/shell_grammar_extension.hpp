//===----------------------------------------------------------------------===//
//                         DuckDB
//
// shell_grammar_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/grammar_extension.hpp"

namespace duckdb {

//! CLI shorthand: a bare expression list ("1 + 1", "x: 42") or table name ("tbl") is a statement
class ShellGrammarExtension final : public GrammarExtension {
public:
	ShellGrammarExtension();

public:
	vector<GrammarChange> GetChanges() const override;
};

} // namespace duckdb
